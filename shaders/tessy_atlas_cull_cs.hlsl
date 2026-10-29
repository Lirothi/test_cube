// Tessy N4 (docs/tessy_plan.md): the cluster cull of the LOCAL lights' atlas views (a point light's cube face, a
// spot). The shadow cull (shadow_cull_cs.hlsl) leaves a row of draw args per (view, virtual group) and the casters it
// kept in the view's visible list; a point face's frustum is a whole 90-degree view, and a mesh that touches it is
// drawn whole -- with wind_test's "Lamps" group on, 5.6 M triangles into the grove's faces of which 1.1 M survive
// clipping (plan section 9). This pass takes each record whose group has clusters, tests every (caster, cluster) box --
// padded by this frame's wind, as the VSM cull pads it (tessy_cull_common.hlsli) -- against the face's six frustum
// planes, and writes the survivors' MESHLETS to the view's run of TessyAtlasVisible; then it zeroes the record's
// InstanceCount, so the compaction after it (shadow_args_compact_cs.hlsl) leaves the record out of the classic draw.
// shadow_indirect_csm.hlsl TESSY_ATLAS_MS draws the run with one DispatchMesh per view.
//
// One 64-thread group per view, two sweeps over the view's records: the first counts the meshlets that survive, one
// atomic then claims the view's run in the buffer, the second writes it. A view whose run does not fit keeps its
// records classic (nothing zeroed): a full buffer costs speed, never a shadow. The records are taken 64 at a time, and
// within each batch the group shares the (caster, cluster) tests out (a prefix sum in groupshared), as the VSM page
// scatter shares its pages.
#pragma pack_matrix(row_major)
#include "wind.hlsli"
#include "tessy_common.hlsli"
#include "tessy_cull_common.hlsli"

#define TESSY_ATLAS_CULL_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer AtlasCullCB : register(b0)
{
    uint gNumGroups;   // virtual groups (a row's width)
    uint gFirstView;   // the row of local view 0 in the args (the main rows: kShadowViewCascades; S7's static rows: 0)
    uint gNumViews;    // local views
    uint gRowSet;      // 0 = the main rows, 1 = S7's static rows: TessyAtlasViewInfo / Args row gRowSet * gViewStride + view
    uint gViewStride;  // render::kShadowViewCeiling
    uint gFrustumFirst;// the frustum slot of local view 0 (render::kShadowViewCascades)
    uint gRecordBase;  // this row set's part of TessyAtlasVisible: [gRecordBase, gRecordBase + gRecordCap)
    uint gRecordCap;
    float4 gWind0;     // the frame's wind, as the views' PerView carries it: (time, prevTime, dir.x, dir.z)
    float4 gWind1;     // (sway amplitude, frequency, gust, previous gust); a rigid light's view is padded anyway
    uint gWindPad;     // tessy.windPad
    uint3 gPad;
    // S7's rows (gRowSet 1): one bit per view slot, the views that draw their static rows this frame (a cache miss, an
    // uncached view). A hit copies them from the pool, so its rows are left as they are (nothing draws them).
    uint4 gStaticViews[(TC_SHADOW_VIEW_CEILING + 127u) / 128u];
};

struct ViewFrustum { float4 planes[16]; }; // shadow_cull_cs.hlsl's; a local view's slots 0-5 are its frustum

StructuredBuffer<InstancePerObject> Instances    : register(t0);
StructuredBuffer<ViewFrustum>       Frustums     : register(t1);
StructuredBuffer<TessyCluster>      Clusters     : register(t2);
StructuredBuffer<uint2>             TessyVg      : register(t3); // per virtual group {first cluster, count}
StructuredBuffer<uint4>             GroupLodMega : register(t4); // per virtual group; .w = the mesh's base vertex

RWByteAddressBuffer       Args               : register(u0); // the cull's rows (D3D12_DRAW_INDEXED_ARGUMENTS, 20 bytes)
RWStructuredBuffer<uint>  VisibleList        : register(u1); // the rows' caster ids (this frame's region)
RWStructuredBuffer<uint4> TessyVisible       : register(u2); // {caster id, meshlet, base vertex, 0}
RWStructuredBuffer<uint2> TessyAtlasViewInfo : register(u3); // per (row set, view): {first record, records}
RWByteAddressBuffer       TessyAtlasArgs     : register(u4); // per (row set, view): D3D12_DISPATCH_MESH_ARGUMENTS
RWByteAddressBuffer       TessyAtlasCounter  : register(u5); // [0] / [4]: the row sets' claimed records, [8]: views that
                                                              // did not fit, [12]: clusters tested (cleared per frame)

static const uint kArgStride = 20u;
static const uint kThreads = 256u;
// A view's records with clusters, gathered first: a face has a handful (the classic row is every virtual group, 824 on
// wind_test, nearly all empty). Walking the whole row per batch, with a serial prefix sum in each, cost the cull pass
// 0.14 ms -- more than the clusters saved the draw. A record past this many stays classic.
static const uint kMaxEntries = 1024u;

groupshared uint gsEntryVg[kMaxEntries];
groupshared uint gsEntryStart[kMaxEntries];
groupshared uint gsEntryInst[kMaxEntries];
groupshared uint gsEntries;
groupshared uint gsPrefix[kThreads]; // per entry of the batch: its first (caster, cluster) test
groupshared uint gsWaveSum[kThreads / 4u]; // the batch's tests per wave (a wave holds 4 to 128 lanes)
groupshared uint gsWaves;            // waves that took their index (CSMain)
groupshared uint gsRecords;          // the view's surviving meshlets (sweep 1), then the cursor (sweep 2)
groupshared uint gsRunBase;
groupshared uint gsClaimed;          // the records sweep 1 counted and claimed: sweep 2 writes no more
groupshared bool gsFits;

// Does cluster `c` of caster `cid` touch the view (its world box, wind padded, against the six planes)?
bool ClusterInView(uint cid, TessyCluster c, ViewFrustum vf)
{
    const InstancePerObject ip = Instances[cid];
    float3 ctr = mul(float4(c.center, 1.0f), ip.world).xyz;
    float3 ext = abs(ip.world[0].xyz) * c.extent.x + abs(ip.world[1].xyz) * c.extent.y + abs(ip.world[2].xyz) * c.extent.z;
    if (gWindPad != 0u) { WindPadBox(ip, c, gWind0, gWind1, gWindPad, ctr, ext); }
    [unroll] for (int p = 0; p < 6; ++p)
    {
        if (dot(vf.planes[p].xyz, ctr) + vf.planes[p].w + dot(abs(vf.planes[p].xyz), ext) < 0.0f) { return false; }
    }
    return true;
}

// One sweep over the gathered records, 64 at a time, the (caster, cluster) tests shared out by a prefix sum (wave
// intrinsics, then the waves' totals). write = false: count the surviving meshlets into gsRecords; true: write them at
// gsRunBase + the cursor, and zero each record's InstanceCount in the cull's row (the compaction then skips it).
// `slot` is the thread's place in WAVE order (CSMain), never SV_GroupIndex: a wave's prefix sum runs in lane order,
// and an 8x8 group's lanes do not hold consecutive SV_GroupIndex on NVIDIA -- indexed by it, the prefixes belonged to
// other records, tests went to the wrong (caster, cluster) and every frame lost a different handful of lamps' shadows
// (the records' order is the atomics', new each frame).
void Sweep(uint row, uint entries, uint slot, bool write, ViewFrustum vf, inout uint tested)
{
    const uint lanes = WaveGetLaneCount();
    const uint wave = slot / lanes;
    const uint waves = (kThreads + lanes - 1u) / lanes;
    for (uint first = 0u; first < entries; first += kThreads)
    {
        const uint e = first + slot;
        uint tests = 0u;
        if (e < entries)
        {
            tests = gsEntryInst[e] * TessyVg[gsEntryVg[e]].y;
            if (write) { Args.Store((row + gsEntryVg[e]) * kArgStride + 4u, 0u); }
        }
        const uint pre = WavePrefixSum(tests);
        const uint sum = WaveActiveSum(tests);
        if (WaveIsFirstLane()) { gsWaveSum[wave] = sum; }
        GroupMemoryBarrierWithGroupSync();
        uint before = 0u, total = 0u;
        for (uint w = 0u; w < waves; ++w)
        {
            before += (w < wave) ? gsWaveSum[w] : 0u;
            total += gsWaveSum[w];
        }
        gsPrefix[slot] = before + pre;
        GroupMemoryBarrierWithGroupSync();
        // The owner of q: the last entry whose first test is at or before it (an entry with no tests shares its prefix
        // with the next one; entries past the batch's end hold `total`, never at or before q). Searched once, followed.
        uint t = 0u;
        [unroll] for (uint step = kThreads / 2u; step > 0u; step >>= 1u)
        {
            if (gsPrefix[t + step] <= slot) { t += step; }
        }
        for (uint q = slot; q < total; q += kThreads)
        {
            while (t + 1u < kThreads && gsPrefix[t + 1u] <= q) { ++t; }
            const uint vg = gsEntryVg[first + t];
            const uint2 range = TessyVg[vg];
            const uint local = q - gsPrefix[t];
            const uint k = local / range.y;                     // the caster
            const uint cid = VisibleList[gsEntryStart[first + t] + k];
            const TessyCluster c = Clusters[range.x + (local - k * range.y)];
            if (!write) { ++tested; }
            if (!ClusterInView(cid, c, vf)) { continue; }
            uint at;
            InterlockedAdd(gsRecords, c.meshletCount, at);
            if (write)
            {
                const uint baseVertex = GroupLodMega[vg].w;
                for (uint s = 0u; s < c.meshletCount && at + s < gsClaimed; ++s)
                {
                    TessyVisible[gRecordBase + gsRunBase + at + s] = uint4(cid, c.firstMeshlet + s, baseVertex, 0u);
                }
            }
        }
        GroupMemoryBarrierWithGroupSync(); // gsPrefix / gsWaveSum are the next batch's
    }
}

[numthreads(16, 16, 1)]
[RootSignature(TESSY_ATLAS_CULL_RS)]
void CSMain(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint view = gid.x; // the dispatch has exactly gNumViews groups
    if (view >= gNumViews) { return; } // uniform: the whole group
    if (gRowSet == 1u)
    {
        const uint s = gFrustumFirst + view; // the view's slot
        if (((gStaticViews[s / 128u][(s / 32u) % 4u] >> (s % 32u)) & 1u) == 0u) // uniform
        {
            if (gi == 0u)
            {
                const uint info = gRowSet * gViewStride + view;
                TessyAtlasViewInfo[info] = uint2(0u, 0u);
                TessyAtlasArgs.Store3(info * 12u, uint3(0u, 0u, 1u));
            }
            return;
        }
    }
    const ViewFrustum vf = Frustums[gFrustumFirst + view];
    const uint row = (gFirstView + view) * gNumGroups;
    if (gi == 0u) { gsEntries = 0u; gsRecords = 0u; gsWaves = 0u; }
    GroupMemoryBarrierWithGroupSync();
    // This thread's place in wave order (Sweep says why): its wave's index, taken once by the wave's first lane, and its lane.
    uint waveIndex = 0u;
    if (WaveIsFirstLane()) { InterlockedAdd(gsWaves, 1u, waveIndex); }
    const uint slot = WaveReadLaneFirst(waveIndex) * WaveGetLaneCount() + WaveGetLaneIndex();
    // Gather the row's records with casters AND clusters.
    for (uint vg = gi; vg < gNumGroups; vg += kThreads)
    {
        const uint instances = Args.Load((row + vg) * kArgStride + 4u);
        if (instances == 0u || TessyVg[vg].y == 0u) { continue; }
        uint idx;
        InterlockedAdd(gsEntries, 1u, idx);
        if (idx < kMaxEntries)
        {
            gsEntryVg[idx] = vg;
            gsEntryStart[idx] = Args.Load((row + vg) * kArgStride + 16u);
            gsEntryInst[idx] = instances;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint entries = min(gsEntries, kMaxEntries);
    uint tested = 0u;
    Sweep(row, entries, slot, false, vf, tested);
    if (gi == 0u)
    {
        const uint records = gsRecords;
        uint base = 0u;
        if (records != 0u) { TessyAtlasCounter.InterlockedAdd(gRowSet * 4u, records, base); }
        gsFits = records == 0u || base + records <= gRecordCap;
        if (!gsFits) { TessyAtlasCounter.InterlockedAdd(8u, 1u); }
        gsRunBase = base;
        gsClaimed = records;
        gsRecords = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gsFits) { Sweep(row, entries, slot, true, vf, tested); }
    GroupMemoryBarrierWithGroupSync();
    // The run as WRITTEN: sweep 2 repeats sweep 1's tests, and a box on a plane may not answer the same twice (each
    // sweep is compiled on its own); it never writes past the claim, and a short count draws only what it wrote.
    if (gi == 0u)
    {
        const uint fitted = gsFits ? min(gsRecords, gsClaimed) : 0u;
        const uint info = gRowSet * gViewStride + view;
        TessyAtlasViewInfo[info] = uint2(gRecordBase + gsRunBase, fitted);
        TessyAtlasArgs.Store3(info * 12u, fitted == 0u ? uint3(0u, 0u, 1u)
                                                      : uint3(min(fitted, kTessyMsRow), (fitted + kTessyMsRow - 1u) / kTessyMsRow, 1u));
    }
    const uint wTested = WaveActiveSum(tested);
    if (WaveIsFirstLane() && wTested != 0u) { TessyAtlasCounter.InterlockedAdd(12u, wTested); }
}
