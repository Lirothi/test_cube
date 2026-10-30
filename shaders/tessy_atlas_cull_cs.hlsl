// Tessy N4 (docs/tessy_plan.md): the cluster cull of the LOCAL lights' atlas views (a point light's cube face, a
// spot). The shadow cull (shadow_cull_cs.hlsl) leaves a row of draw args per (view, virtual group) and the casters it
// kept in the view's visible list; a point face's frustum is a whole 90-degree view, and a mesh that touches it is
// drawn whole -- with wind_test's "Lamps" group on, 5.6 M triangles into the grove's faces of which 1.1 M survive
// clipping (plan section 9). This pass takes each record whose group has clusters, tests every (caster, cluster) box --
// padded by this frame's wind, as the VSM cull pads it (tessy_cull_common.hlsli) -- against the face's six frustum
// planes, and writes the survivors' MESHLETS to the view's run of TessyAtlasVisible; then it zeroes the record's
// InstanceCount, so the compaction after it (shadow_args_compact_cs.hlsl) leaves the record out of the classic draw.
// shadow_indirect_csm.hlsl TESSY_ATLAS_MS draws the run with one DispatchMesh per view. Tessy N5 runs it on the
// CAMERA's row as well (kFlagByRun below): the camera's G-buffer by clusters.
//
// One 256-thread group per view, two sweeps over the view's records: the first counts the meshlets that survive, one
// atomic then claims the view's run in the buffer, the second writes it. A view whose run does not fit keeps its
// records classic (nothing zeroed): a full buffer costs speed, never a shadow. The records are taken 256 at a time, and
// within each batch the group shares the (caster, cluster) tests out (a prefix sum in groupshared), as the VSM page
// scatter shares its pages.
#pragma pack_matrix(row_major)
#include "wind.hlsli"
#include "tessy_common.hlsli"
#include "tessy_cull_common.hlsli"
#include "hzb_cull.hlsli"

#define TESSY_ATLAS_CULL_RS \
    "CBV(b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=7, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=7, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer AtlasCullCB : register(b0)
{
    uint gNumGroups;   // virtual groups (a row's width)
    uint gFirstView;   // the row of local view 0 in the args (the main rows: kShadowViewCascades; S7's static rows and
                       // the camera: 0)
    uint gNumViews;    // local views (the camera: 1, or with kFlagByRun its G-buffer's PSO runs)
    uint gRowSet;      // 0 = the main rows, 1 = S7's static rows, 2 = the camera's row (the cluster view, N5's G-buffer):
                       // its TessyAtlasViewInfo / args slot is gRowSet * gViewStride + view, its claim cursor gRowSet * 4
    uint gViewStride;  // render::kShadowViewCeiling
    uint gFrustumFirst;// the frustum slot of local view 0 (render::kShadowViewCascades)
    uint gRecordBase;  // this row set's part of TessyAtlasVisible: [gRecordBase, gRecordBase + gRecordCap)
    uint gRecordCap;
    float4 gWind0;     // the frame's wind, as the views' PerView carries it: (time, prevTime, dir.x, dir.z)
    float4 gWind1;     // (sway amplitude, frequency, gust, previous gust); a rigid light's view is padded anyway
    uint gWindPad;     // tessy.windPad
    uint gFlags;       // kFlag* below
    uint gClassicCountOffset; // kFlagByRun: where in TessyAtlasArgs each run's classic command count goes (bytes)
    uint gCamDeferCap; // N5's two-pass occlusion: TessyCamDeferred's capacity (entries); 0 = no pyramid test
    // S7's rows (gRowSet 1): one bit per view slot, the views that draw their static rows this frame (a cache miss, an
    // uncached view). A hit copies them from the pool, so its rows are left as they are (nothing draws them).
    uint4 gStaticViews[(TC_SHADOW_VIEW_CEILING + 127u) / 128u];
};

// N5: mirrors ShadowGpuData::CameraHzbParams (shadow_cull_cs.hlsl's b2). Bound on every row set; read only with
// gCamDeferCap != 0 (the camera's runs, the camera's two-pass HZB on this frame).
cbuffer CameraHzbCB : register(b1)
{
    row_major float4x4 gCamPrevViewProj;
    row_major float4x4 gCamPrevViewToClip;
    row_major float4x4 gCamViewProj;
    row_major float4x4 gCamViewToClip;
    int4  gCamViewRect;
    uint2 gCamHzbSize;
    uint  gCamHzbPrevValid;
    uint  gCamHzbOn;
};

struct ViewFrustum { float4 planes[16]; }; // shadow_cull_cs.hlsl's; a local view's slots 0-5 are its frustum

StructuredBuffer<InstancePerObject> Instances    : register(t0);
StructuredBuffer<ViewFrustum>       Frustums     : register(t1);
StructuredBuffer<TessyCluster>      Clusters     : register(t2);
StructuredBuffer<uint2>             TessyVg      : register(t3); // per virtual group {first cluster, count}
StructuredBuffer<uint4>             GroupLodMega : register(t4); // per virtual group; .w = the mesh's base vertex
StructuredBuffer<uint>              VgRun        : register(t5); // kFlagByRun: per virtual group, the camera G-buffer's
                                                                  // PSO run that draws it as clusters (else ~0: classic)
Texture2D<float>                    CamHzbPrev   : register(t6); // N5: last frame's camera pyramid (placeholder when off)

RWByteAddressBuffer       Args               : register(u0); // the cull's rows (D3D12_DRAW_INDEXED_ARGUMENTS, 20 bytes)
RWStructuredBuffer<uint>  VisibleList        : register(u1); // the rows' caster ids (this frame's region)
RWStructuredBuffer<uint4> TessyVisible       : register(u2); // {caster id, meshlet, base vertex, 0}
RWStructuredBuffer<uint2> TessyAtlasViewInfo : register(u3); // per (row set, view): {first record, records}
RWByteAddressBuffer       TessyAtlasArgs     : register(u4); // per (row set, view): D3D12_DISPATCH_MESH_ARGUMENTS
RWByteAddressBuffer       TessyAtlasCounter  : register(u5); // [0] / [4] / [8]: the row sets' claimed records, [12]:
                                                              // views that did not fit, [16]: clusters tested, [20]: the
                                                              // camera's deferred clusters claimed (cleared per frame)
RWStructuredBuffer<uint4> TessyCamDeferred   : register(u6); // N5: {caster id, cluster, camId, base vertex} the previous
                                                              // pyramid hid (tessy_camera_post_cs retests them), or dead

// The camera row is not a shadow view. For the cluster view alone (tessy::g_debugView) its records are drawn AS WELL as
// its rows (the G-buffer reads them): every cluster of every caster the camera kept. For N5 (tessy::g_gbuffer) the row
// is taken like an atlas view's -- the clusters culled by the camera frustum, the rows zeroed for the classic G-buffer --
// but sorted by the G-buffer's PSO runs: group `view` takes the virtual groups whose VgRun is `view`, and each run is
// one DispatchMesh of that run's mesh shader twin (gbuffer_indirect.hlsl TESSY_GBUFFER_MS).
static const uint kFlagKeepRows = 1u;  // leave the rows' InstanceCount alone
static const uint kFlagNoCull = 2u;    // no frustum test: the camera's cull already kept the caster
static const uint kFlagClusterId = 4u; // record .w = the virtual group | the cluster's index in its range << 16
static const uint kFlagByRun = 8u;     // one row (gFirstView's), its groups split by VgRun; view = the run

static const uint kArgStride = 20u;
// The group's threads: 256 for the atlas views (many groups, a few hundred tests each); the camera's PSO runs are a
// handful of groups with thousands of tests each, and 1024 threads take them in a quarter of the batches (N5).
#ifndef TESSY_CULL_THREADS
#define TESSY_CULL_THREADS 256
#endif
static const uint kThreads = TESSY_CULL_THREADS;
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
groupshared uint gsLeftClassic;      // a row with casters this pass cannot take (no clusters, past kMaxEntries)
groupshared uint gsDeferred;         // N5: the clusters last frame's pyramid hid (sweep 1), then the cursor (sweep 2)
groupshared uint gsDeferredBase;
groupshared uint gsDeferredClaimed;

// Cluster `c` of caster `cid`: its world box, wind padded.
void ClusterBox(uint cid, TessyCluster c, out float3 ctr, out float3 ext)
{
    const InstancePerObject ip = Instances[cid];
    ctr = mul(float4(c.center, 1.0f), ip.world).xyz;
    ext = abs(ip.world[0].xyz) * c.extent.x + abs(ip.world[1].xyz) * c.extent.y + abs(ip.world[2].xyz) * c.extent.z;
    if (gWindPad != 0u) { WindPadBox(ip, c, gWind0, gWind1, gWindPad, ctr, ext); }
}

// Does that box touch the view (the six planes)?
bool BoxInView(float3 ctr, float3 ext, ViewFrustum vf)
{
    [unroll] for (int p = 0; p < 6; ++p)
    {
        if (dot(vf.planes[p].xyz, ctr) + vf.planes[p].w + dot(abs(vf.planes[p].xyz), ext) < 0.0f) { return false; }
    }
    return true;
}

static const float4x4 kTessyIdentity = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
// N5: was the box hidden in LAST frame's camera view? shadow_cull_cs.hlsl CamHiddenLastFrame, per cluster (UE's
// main pass, NaniteCullingCommon.ush:463-482): a box crossing the near plane is visible, the post pass cleans up.
bool BoxHiddenLastFrame(float3 ctr, float3 ext)
{
    if (gCamHzbPrevValid == 0u) { return false; }
    const HzbFrustumCull prev = HzbBoxCullFrustumPerspective(ctr, ext, kTessyIdentity, gCamPrevViewProj, gCamPrevViewToClip, true);
    if (!prev.isVisible || prev.crossesNearPlane) { return false; }
    const HzbScreenRect rect = HzbGetScreenRect(gCamViewRect, prev.rectMin, prev.rectMax, 4);
    return !HzbIsVisible(CamHzbPrev, gCamHzbSize, rect);
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
            if (write && (gFlags & kFlagKeepRows) == 0u) { Args.Store((row + gsEntryVg[e]) * kArgStride + 4u, 0u); }
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
            const uint clusterIndex = range.x + (local - k * range.y);
            const TessyCluster c = Clusters[clusterIndex];
            if (!write) { ++tested; }
            float3 ctr = 0.0f, ext = 0.0f;
            if ((gFlags & kFlagNoCull) == 0u)
            {
                ClusterBox(cid, c, ctr, ext);
                if (!BoxInView(ctr, ext, vf)) { continue; }
            }
            // The camera's records carry their virtual group (the G-buffer's material and LOD) and, with it, an id unique
            // per cluster (the cluster view's colour). A range past 65535 clusters is 8 M triangles.
            const uint camId = vg | (min(clusterIndex - range.x, 0xFFFFu) << 16);
            const uint baseVertex = GroupLodMega[vg].w;
            // N5: hidden by last frame's pyramid -> deferred to pass B (tessy_camera_post_cs retests it against this
            // frame's). Each sweep is compiled on its own and a box on an edge may not answer the same twice: a cluster
            // sweep 2 defers past the claim is drawn here instead, and one it keeps past the records' claim is deferred.
            const bool hidden = gCamDeferCap != 0u && BoxHiddenLastFrame(ctr, ext);
            if (!write)
            {
                if (hidden) { InterlockedAdd(gsDeferred, 1u); }
                else { InterlockedAdd(gsRecords, c.meshletCount); }
                continue;
            }
            if (hidden)
            {
                uint d;
                InterlockedAdd(gsDeferred, 1u, d);
                if (d < gsDeferredClaimed)
                {
                    TessyCamDeferred[gsDeferredBase + d] = uint4(cid, clusterIndex, camId, baseVertex);
                    continue;
                }
            }
            uint at;
            InterlockedAdd(gsRecords, c.meshletCount, at);
            if (at + c.meshletCount > gsClaimed && gCamDeferCap != 0u)
            {
                uint d;
                InterlockedAdd(gsDeferred, 1u, d);
                if (d < gsDeferredClaimed)
                {
                    TessyCamDeferred[gsDeferredBase + d] = uint4(cid, clusterIndex, camId, baseVertex);
                    continue;
                }
            }
            for (uint s = 0u; s < c.meshletCount && at + s < gsClaimed; ++s)
            {
                TessyVisible[gRecordBase + gsRunBase + at + s] =
                    uint4(cid, c.firstMeshlet + s, baseVertex, (gFlags & kFlagClusterId) != 0u ? camId : 0u);
            }
        }
        GroupMemoryBarrierWithGroupSync(); // gsPrefix / gsWaveSum are the next batch's
    }
}

[numthreads(TESSY_CULL_THREADS, 1, 1)]
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
    const bool byRun = (gFlags & kFlagByRun) != 0u;
    const ViewFrustum vf = Frustums[gFrustumFirst + (byRun ? 0u : view)];
    const uint row = (gFirstView + (byRun ? 0u : view)) * gNumGroups;
    if (gi == 0u) { gsEntries = 0u; gsRecords = 0u; gsWaves = 0u; gsLeftClassic = 0u; gsDeferred = 0u; }
    GroupMemoryBarrierWithGroupSync();
    // This thread's place in wave order (Sweep says why): its wave's index, taken once by the wave's first lane, and its lane.
    uint waveIndex = 0u;
    if (WaveIsFirstLane()) { InterlockedAdd(gsWaves, 1u, waveIndex); }
    const uint slot = WaveReadLaneFirst(waveIndex) * WaveGetLaneCount() + WaveGetLaneIndex();
    // Gather the row's records with casters AND clusters.
    for (uint vg = gi; vg < gNumGroups; vg += kThreads)
    {
        if (byRun && VgRun[vg] != view) { continue; }
        const uint instances = Args.Load((row + vg) * kArgStride + 4u);
        if (instances == 0u) { continue; }
        if (TessyVg[vg].y == 0u) { gsLeftClassic = 1u; continue; }
        uint idx;
        InterlockedAdd(gsEntries, 1u, idx);
        if (idx < kMaxEntries)
        {
            gsEntryVg[idx] = vg;
            gsEntryStart[idx] = Args.Load((row + vg) * kArgStride + 16u);
            gsEntryInst[idx] = instances;
        }
        else { gsLeftClassic = 1u; }
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
        bool fits = records == 0u || base + records <= gRecordCap;
        // N5: the deferred clusters' room in the frame's list; past its capacity the run stays classic, its claimed
        // slots dead (below) -- the post pass reads every slot up to the claim.
        const uint deferred = gsDeferred;
        uint dbase = 0u;
        if (deferred != 0u) { TessyAtlasCounter.InterlockedAdd(20u, deferred, dbase); }
        fits = fits && (deferred == 0u || dbase + deferred <= gCamDeferCap);
        gsFits = fits;
        if (!fits) { TessyAtlasCounter.InterlockedAdd(12u, 1u); }
        gsRunBase = base;
        gsClaimed = records;
        gsRecords = 0u;
        gsDeferredBase = dbase;
        gsDeferredClaimed = dbase < gCamDeferCap ? min(deferred, gCamDeferCap - dbase) : 0u;
        gsDeferred = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gsFits) { Sweep(row, entries, slot, true, vf, tested); }
    GroupMemoryBarrierWithGroupSync();
    // N5: the deferred slots this group claimed and did not write (a run that did not fit, sweep 2 deferring fewer than
    // sweep 1): dead, so the post pass draws nothing stale out of them.
    {
        const uint written = gsFits ? min(gsDeferred, gsDeferredClaimed) : 0u;
        for (uint d = written + gi; d < gsDeferredClaimed; d += kThreads)
        {
            TessyCamDeferred[gsDeferredBase + d] = uint4(kTessyDeadRecord, 0u, 0u, 0u);
        }
    }
    // The run as WRITTEN: sweep 2 repeats sweep 1's tests, and a box on a plane may not answer the same twice (each
    // sweep is compiled on its own); it never writes past the claim, and a short count draws only what it wrote.
    if (gi == 0u)
    {
        const uint fitted = gsFits ? min(gsRecords, gsClaimed) : 0u;
        const uint info = gRowSet * gViewStride + view;
        TessyAtlasViewInfo[info] = uint2(gRecordBase + gsRunBase, fitted);
        TessyAtlasArgs.Store3(info * 12u, fitted == 0u ? uint3(0u, 0u, 1u)
                                                      : uint3(min(fitted, kTessyMsRow), (fitted + kTessyMsRow - 1u) / kTessyMsRow, 1u));
        // The camera G-buffer's run, taken whole: its classic merged commands are all empty now -- the count says skip
        // them (0); otherwise all of them (ExecuteIndirect takes min(count, the run's commands)).
        if (byRun) { TessyAtlasArgs.Store(gClassicCountOffset + view * 4u, (gsFits && gsLeftClassic == 0u) ? 0u : 0xFFFFFFFFu); }
    }
    const uint wTested = WaveActiveSum(tested);
    if (WaveIsFirstLane() && wTested != 0u) { TessyAtlasCounter.InterlockedAdd(16u, wTested); }
}
