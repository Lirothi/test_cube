// Tessy N4 (docs/tessy_plan.md): the cluster cull of the LOCAL lights' atlas views (a point light's cube face, a
// spot). The shadow cull (shadow_cull_cs.hlsl) leaves a row of draw args per (view, virtual group) and the casters it
// kept in the view's visible list; a point face's frustum is a whole 90-degree view, and a mesh that touches it is
// drawn whole -- with wind_test's "Lamps" group on, 5.6 M triangles into the grove's faces of which 1.1 M survive
// clipping (plan section 9). This pass takes each record whose group has clusters, tests every (caster, cluster) box --
// padded by this frame's wind, as the VSM cull pads it (tessy_cull_common.hlsli) -- against the face's six frustum
// planes, and writes the survivors' MESHLETS to the view's run of TessyAtlasVisible; then it zeroes the record's
// InstanceCount, so the compaction after it (shadow_args_compact_cs.hlsl) leaves the record out of the classic draw.
// shadow_indirect_csm.hlsl TESSY_ATLAS_MS draws the run with one DispatchMesh per view. Tessy N5 runs it on the
// CAMERA's row as well (kFlagByRun below): the camera's G-buffer by clusters. And on the SDSM partitions' rows (row set
// 3, tessy.sdsm): Main_SdsmCull re-culls rows 0..3 against the analyze's planes, this kernel takes their records against
// the same 16 planes, and Main_SdsmShadow draws each partition's run with the same mesh shader (the cascade atlas has the
// local atlases' PSO state).
//
// N7d: on the camera's runs a mesh group with a cluster DAG (gDagOn) takes the DAG's cut instead of its LOD tier's
// clusters, each caster its own: UE Nanite's test (NaniteHierarchyTraversalCommon.ush / ClusterCulling: a cluster is
// drawn when its own error projects to at most the threshold and its parent's to more), with LodSelect's projection --
// error x scale x (pixels per unit at depth 1 / lod.pixelError) against the distance to the NEAREST point of the error's
// sphere, which is where a discrete LOD of the same deviation switches (ViewDistance + R). The cut is exact by
// construction: siblings share their parent's error and sphere, a parent's sphere holds its children's and its error is
// no smaller, so every point of the surface is covered once whatever the camera.
//
// One 256-thread group per view (an SDSM partition: gSplit groups, each a slice of its records and a run of its own), two
// sweeps over the view's records: the first counts the meshlets that survive, one
// atomic then claims the view's run in the buffer, the second writes it. A view whose run does not fit keeps its
// records classic (nothing zeroed): a full buffer costs speed, never a shadow. The records are taken 256 at a time, and
// within each batch the group shares the (caster, cluster) tests out (a prefix sum in groupshared), as the VSM page
// scatter shares its pages.
#pragma pack_matrix(row_major)
#include "wind.hlsli"
#include "tessy_common.hlsli"
#include "tessy_cull_common.hlsli"
#include "hzb_cull.hlsli"
#include "sdsm_partitions.hlsli" // SdsmPartition: the sun rows' matrices (kFlagRectCull)

#define TESSY_ATLAS_CULL_RS \
    "CBV(b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=16, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=7, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer AtlasCullCB : register(b0)
{
    uint gNumGroups;   // virtual groups (a row's width)
    uint gFirstView;   // the row of local view 0 in the args (the main rows: kShadowViewCascades; S7's static rows and
                       // the camera: 0)
    uint gNumViews;    // local views (the camera: 1, or with kFlagByRun its G-buffer's PSO runs)
    uint gRowSet;      // 0 = the main rows, 1 = S7's static rows, 2 = the camera's row (the cluster view, N5's G-buffer),
                       // 3 = the SDSM partitions' rows (tessy.sdsm: the sun, re-culled by Main_SdsmCull): its
                       // TessyAtlasViewInfo / args slot is gRowSet * gViewStride + view, its claim cursor gCounterAt.x
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
    // N7d: the camera's DAG cut (the camera's runs only). xyz = the camera, w = pixels per unit at depth 1 / lod.pixelError
    // (/ lod.distanceScale); gDagNear floors the distance (a sphere around the camera takes its leaves).
    float4 gDagCam;
    uint gDagOn;
    float gDagNear;
    uint gDagBand; // N7e: 1 = each caster tests the band of levels its distance can cut (tessy.lodBand), 0 = every level
    uint gPlanes;  // the frustum planes a box is tested against: a local view's 6; an SDSM partition's 16 (its cull volume
                   // and its box, the tail accept-all -- shadow_cull_cs tests the same 16)
    // Where this row set counts in TessyAtlasCounter (bytes): x its claimed records, y its views that did not fit, z its
    // clusters tested, w its triangles written (+4: its DAG clusters), ~0 = not counted. Row sets 0-2 share y / z.
    uint4 gCounterAt;
    // Groups a view (the SDSM partitions: ShadowGpuData::kTessySunSplit): group g takes view g / gSplit and its virtual
    // groups vg % gSplit == g % gSplit, and writes its own run at slot g -- one group a partition left the GPU idle. 1 else.
    uint gSplit;
    float2 gRectSize;  // kFlagRectCull on the SDSM partitions' rows: the partition's viewport (the tile's content rect)
    uint gRectCountAt; // kFlagRectCull: where in TessyAtlasCounter the clusters it dropped are counted (bytes)
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
StructuredBuffer<TessyClusterLod>   ClusterLods  : register(t7); // N7d: per cluster (zeros on a discrete LOD's)
StructuredBuffer<uint4>             DagGroups    : register(t8); // N7d: per mesh group {first DAG cluster, count, ...}
StructuredBuffer<uint>              CasterLods   : register(t9); // N7d: the caster's camera tier (shadow_cull_cs CasterLod)
StructuredBuffer<uint4>             DagLevels    : register(t10); // N7e: per level {first cluster, count, min lodError,
                                                                   // max parentError (float bits)}
StructuredBuffer<float4>            DagSpheres   : register(t11); // N7e: per mesh group, a sphere around every lod and parent
                                                                   // sphere of its DAG (object space)
StructuredBuffer<float4>            ViewDag      : register(t12); // N7e: per view slot {the light, texels per unit at distance
                                                                   // 1 / tessy.lodShadowTexels}; w 0 = no cut (the atlas views)
StructuredBuffer<uint>              RunCmds      : register(t13); // kFlagByRun: per run {its first command, its commands} at
                                                                   // [2r], [2r + 1], then the commands' virtual groups in the
                                                                   // classic merged draw's order (ShadowGpuData cmdOrder_)
StructuredBuffer<SdsmPartition>     SunPartitions : register(t14); // kFlagRectCull, row set 3: each partition's lightViewProj
// kFlagRectCull, the lamps' rows: per view slot the matrix its atlas draw projects with and its viewport's size (0 = none)
// -- ShadowGpuData::UpdateViewFrustums from Scene's LocalViewCull.
struct TessyViewRect { float4x4 viewProj; float4 size; };
StructuredBuffer<TessyViewRect>     ViewRects    : register(t15);

RWByteAddressBuffer       Args               : register(u0); // the cull's rows (D3D12_DRAW_INDEXED_ARGUMENTS, 20 bytes)
RWStructuredBuffer<uint>  VisibleList        : register(u1); // the rows' caster ids (this frame's region)
RWStructuredBuffer<uint4> TessyVisible       : register(u2); // {caster id, meshlet, base vertex, 0}
RWStructuredBuffer<uint2> TessyAtlasViewInfo : register(u3); // per (row set, view): {first record, records}
RWByteAddressBuffer       TessyAtlasArgs     : register(u4); // per (row set, view): D3D12_DISPATCH_MESH_ARGUMENTS
RWByteAddressBuffer       TessyAtlasCounter  : register(u5); // [0] / [4] / [8]: the row sets' claimed records, [12]:
                                                              // views that did not fit, [16]: clusters tested, [20]: the
                                                              // camera's deferred clusters claimed, [24] / [28]: the
                                                              // camera's runs' triangles / DAG clusters written; [32..64)
                                                              // the SDSM rows' own claim, not fit, tested, triangles and
                                                              // clusters holding no sample; [64] row sets 0-2's (cleared
                                                              // per frame; the SDSM part again by Main_SdsmCull)
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
// N7e, the cluster view's "Levels" (tessy.debug:3): a camera record's .w bits 16-31 carry the cluster's LEVEL instead of
// its index -- 0x100 | its DAG level, or its discrete LOD tier (the G-buffer reads only bits 0-15).
static const uint kFlagLevelId = 16u;
// The camera's rows lay their entries and records out in order (Sweep says why); without it, where the atomics land
// (tessy.ordered:0, the A/B).
static const uint kFlagOrdered = 32u;
// The view cuts the DAG with the CAMERA's eye and threshold (gDagCam), not its own (ViewDag): the SDSM sun partitions,
// so a caster draws the very clusters its receiver drew (the caster/receiver LOD contract; their discrete tier under the
// camera's cut is another surface than the one it shades).
static const uint kFlagCameraCut = 64u;
// Drop a cluster whose padded box holds no sample of the view (TessyBoxHoldsSample: its rectangle falls between pixel
// / texel centres -- exact, it would rasterize nothing): the camera's runs by gCamViewProj (b1, the G-buffer's jittered
// matrix) on the render target, the lamps' views by ViewRects, the SDSM partitions by their lightViewProj on gRectSize.
// tessy.clusterCull.
static const uint kFlagRectCull = 128u;
static const uint kEntryDag = 0x80000000u; // gsEntryVg: the entry takes its mesh group's DAG, every caster all of it (N7d)
// N7e: gsEntryVg: the entry is ONE caster (gsEntryStart) and the band of its DAG's levels it can cut at its distance
// (gsEntryFirst, gsEntryInst = the band's clusters): the levels whose error interval cannot meet its threshold anywhere
// on the DAG's sphere are not tested at all -- the plain DAG entry tested every level of every caster.
static const uint kEntryInst = 0x40000000u;
static const uint kEntryVgMask = 0x3FFFFFFFu;
static const uint kVgLods = 4u;            // render::kMaxShadowLods: virtual group = mesh group * 4 + LOD
static const uint kCasterLodMask = 0x0Fu;  // shadow_cull_cs's

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
groupshared uint gsEntryFirst[kMaxEntries]; // N7e: a caster entry's band start
groupshared uint gsEntries;
// N7e: the run's virtual groups with a DAG, gathered apart: thread 0 gives each a block of caster entries (or, past
// kMaxEntries, one plain DAG entry), then the group fills the blocks.
static const uint kMaxDagVgs = 128u;
groupshared uint gsDagVg[kMaxDagVgs];
groupshared uint gsDagStart[kMaxDagVgs];
groupshared uint gsDagInst[kMaxDagVgs];
groupshared uint gsDagBlock[kMaxDagVgs];  // its block's first entry, or ~0 (a plain DAG entry instead)
groupshared uint gsDagPrefix[kMaxDagVgs]; // its first caster among the blocks' casters
groupshared uint gsDagVgs;
groupshared uint gsDagCasters;
// N7e: the group's eye and its pixels (texels) per unit at distance 1 over the error it allows -- the camera's for its
// runs (gDagCam), the light's for an atlas view (ViewDag; w 0 = the view takes no DAG cut).
groupshared float4 gsDagEye;
groupshared float4x4 gsRectVP;  // kFlagRectCull: the view's matrix and its viewport (x 0 = no rectangle test)
groupshared float2 gsRectSize;
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

// Does that box touch the view (its first gPlanes planes)?
bool BoxInView(float3 ctr, float3 ext, ViewFrustum vf)
{
    [unroll] for (uint p = 0u; p < 16u; ++p)
    {
        if (p < gPlanes && dot(vf.planes[p].xyz, ctr) + vf.planes[p].w + dot(abs(vf.planes[p].xyz), ext) < 0.0f) { return false; }
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

// N7d: is the simplification behind this error too coarse for the camera here -- its error, grown by the caster's scale,
// spans more than lod.pixelError pixels at the nearest point of its sphere? ONE function for a cluster's own error and
// its parent's: a parent's test must answer exactly as its children's own tests do (they hold the same error and
// sphere), or the cut tears (`precise`: no contraction may differ between the two calls).
bool DagTooCoarse(float error, float4 sphere, float4x4 world, float scale)
{
    precise const float3 c = mul(float4(sphere.xyz, 1.0f), world).xyz;
    precise const float d = max(length(c - gsDagEye.xyz) - sphere.w * scale, gDagNear);
    precise const float projected = error * scale * gsDagEye.w;
    return projected > d;
}

// N7d: the entry's cluster range -- its mesh group's DAG, or its LOD tier's clusters (a caster entry's is its band).
uint2 EntryRange(uint entryVg)
{
    return (entryVg & kEntryDag) != 0u ? DagGroups[(entryVg & kEntryVgMask) / kVgLods].xy : TessyVg[entryVg & kEntryVgMask];
}

// N7e: the band of a caster's DAG levels its cut can reach -- {first cluster, clusters}. Every lod and parent sphere of
// the DAG lies inside DagSpheres', so each one's nearest distance lies in [dMin, dMax]; a level can hold a cut cluster
// only if its smallest error fits at dMax and its largest parent error does not at dMin (DagTooCoarse's test at the two
// ends). A necessary condition, widened by a part in 1e4 for the rounding: the cut itself is still each cluster's test.
uint2 DagBand(uint meshGroup, float4x4 world, float scale)
{
    const uint4 dg = DagGroups[meshGroup];
    const float4 sphere = DagSpheres[meshGroup];
    const float3 c = mul(float4(sphere.xyz, 1.0f), world).xyz;
    const float dist = length(c - gsDagEye.xyz);
    const float dMin = max(dist - sphere.w * scale, gDagNear) * 0.9999f;
    const float dMax = max(dist + sphere.w * scale, gDagNear) * 1.0001f;
    const float k = scale * gsDagEye.w;
    uint lo = 0xFFFFFFFFu, hi = 0u;
    for (uint l = 0u; l < dg.w; ++l)
    {
        const uint4 level = DagLevels[dg.z + l];
        if (asfloat(level.z) * k <= dMax && asfloat(level.w) * k > dMin)
        {
            lo = min(lo, level.x);
            hi = max(hi, level.x + level.y);
        }
    }
    return lo < hi ? uint2(lo, hi - lo) : uint2(dg.x, 0u);
}

// The group's exclusive prefix sum of `count` in `slot` order (wave order, CSMain), and its total. Every thread of the
// group must call it (two group barriers); gsWaveSum is free on entry and on return.
uint OrderedPrefix(uint count, uint slot, out uint total)
{
    const uint lanes = WaveGetLaneCount();
    const uint wave = slot / lanes;
    const uint waves = (kThreads + lanes - 1u) / lanes;
    const uint pre = WavePrefixSum(count);
    const uint sum = WaveActiveSum(count);
    if (WaveIsFirstLane()) { gsWaveSum[wave] = sum; }
    GroupMemoryBarrierWithGroupSync();
    uint before = 0u;
    total = 0u;
    for (uint w = 0u; w < waves; ++w)
    {
        before += (w < wave) ? gsWaveSum[w] : 0u;
        total += gsWaveSum[w];
    }
    GroupMemoryBarrierWithGroupSync();
    return before + pre;
}

// One sweep over the gathered records, 64 at a time, the (caster, cluster) tests shared out by a prefix sum (wave
// intrinsics, then the waves' totals). write = false: count the surviving meshlets into gsRecords; true: write them at
// gsRunBase + the cursor, and zero each record's InstanceCount in the cull's row (the compaction then skips it).
// `slot` is the thread's place in WAVE order (CSMain), never SV_GroupIndex: a wave's prefix sum runs in lane order,
// and an 8x8 group's lanes do not hold consecutive SV_GroupIndex on NVIDIA -- indexed by it, the prefixes belonged to
// other records, tests went to the wrong (caster, cluster) and every frame lost a different handful of lamps' shadows
// (the records' order is the atomics', new each frame).
// The CAMERA's rows (kFlagOrdered: the G-buffer's runs and the cluster view) are written in TEST order -- entry (CSMain's gather: a
// run's virtual groups in the classic merged draw's command order), caster, cluster -- by OrderedPrefix, never where an
// atomic lands: the mesh shader draws the run in record order, and two coplanar surfaces of two meshes
// (the_bathroom_free's ceiling, Selling_top and Selling_top_001) then draw in the classic draw's order every frame, so
// the same one wins the depth test as without clusters. Laid out by atomics the winner changed every frame and the roof
// flickered under the DAG (2026-10-30, owner). A shadow row writes depth alone, where the order does not matter: it
// keeps the atomics.
void Sweep(uint row, uint entries, uint slot, bool write, ViewFrustum vf, inout uint tested, inout uint tris, inout uint dagDrawn,
           inout uint rectDropped)
{
    const uint lanes = WaveGetLaneCount();
    const uint wave = slot / lanes;
    const uint waves = (kThreads + lanes - 1u) / lanes;
    const bool ordered = write && (gFlags & kFlagOrdered) != 0u; // uniform
    uint cursor = 0u; // ordered: the records laid out so far (the same in every thread)
    for (uint first = 0u; first < entries; first += kThreads)
    {
        const uint e = first + slot;
        uint tests = 0u;
        if (e < entries)
        {
            const uint ev = gsEntryVg[e];
            tests = (ev & kEntryInst) != 0u ? gsEntryInst[e] : gsEntryInst[e] * EntryRange(ev).y;
            if (write && (gFlags & kFlagKeepRows) == 0u) { Args.Store((row + (ev & kEntryVgMask)) * kArgStride + 4u, 0u); }
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
        // Every thread takes the same number of steps (OrderedPrefix's barriers sit inside); a step past `total` tests
        // nothing.
        for (uint qb = 0u; qb < total; qb += kThreads)
        {
            const uint q = qb + slot;
            uint m = 0u;                                  // write: the meshlets this thread lays out
            uint cid = 0u, clusterIndex = 0u, camId = 0u, baseVertex = 0u, firstMeshlet = 0u, triCount = 0u;
            bool dag = false;
            if (q < total)
            {
                while (t + 1u < kThreads && gsPrefix[t + 1u] <= q) { ++t; }
                const uint entryVg = gsEntryVg[first + t];
                const uint vg = entryVg & kEntryVgMask;
                const bool inst = (entryVg & kEntryInst) != 0u; // N7e: one caster, its band (the tier was checked at the band)
                dag = inst || (entryVg & kEntryDag) != 0u;
                const uint2 range = inst ? uint2(gsEntryFirst[first + t], gsEntryInst[first + t]) : EntryRange(entryVg);
                const uint local = q - gsPrefix[t];
                const uint k = inst ? 0u : local / range.y;         // the caster
                cid = inst ? gsEntryStart[first + t] : VisibleList[gsEntryStart[first + t] + k];
                clusterIndex = range.x + (local - k * range.y);
                const TessyCluster c = Clusters[clusterIndex];
                if (!write) { ++tested; }
                bool keep = true;
                if (dag)
                {
                    // A crossfading caster sits in the camera row's tier entry and the next one's: the cut draws it once,
                    // from its own (a shadow view's row holds a caster once, at its shadow tier).
                    if (!inst && (gFlags & kFlagByRun) != 0u && min(CasterLods[cid] & kCasterLodMask, kVgLods - 1u) != vg % kVgLods)
                    {
                        keep = false;
                    }
                    else
                    {
                        const float4x4 world = Instances[cid].world;
                        const float scale = TessyWorldScale(world);
                        const TessyClusterLod l = ClusterLods[clusterIndex];
                        keep = !DagTooCoarse(l.lodError, l.lodSphere, world, scale) &&
                               DagTooCoarse(l.parentError, l.parentSphere, world, scale);
                    }
                }
                float3 ctr = 0.0f, ext = 0.0f;
                if (keep && (gFlags & kFlagNoCull) == 0u)
                {
                    ClusterBox(cid, c, ctr, ext);
                    keep = BoxInView(ctr, ext, vf);
                    if (keep && gsRectSize.x > 0.0f)
                    {
                        keep = TessyBoxHoldsSample(ctr, ext, gsRectVP, gsRectSize);
                        if (!keep && !write) { ++rectDropped; }
                    }
                }
                if (keep)
                {
                    // The camera's records carry their virtual group (the G-buffer's material and LOD), whether it is the
                    // DAG's and, with them, an id unique per cluster (the cluster view's colour). A range past 65535
                    // clusters is 8 M triangles.
                    const uint idBase = inst ? DagGroups[vg / kVgLods].x : range.x; // a band's cluster keeps its DAG-wide index
                    const uint hiId = (gFlags & kFlagLevelId) == 0u ? min(clusterIndex - idBase, 0xFFFFu)
                                    : dag ? (0x100u | min(ClusterLods[clusterIndex].level, 0xFFu)) : vg % kVgLods;
                    camId = vg | (dag ? kTessyCamDagBit : 0u) | (hiId << 16);
                    baseVertex = GroupLodMega[vg].w;
                    // N5: hidden by last frame's pyramid -> deferred to pass B (tessy_camera_post_cs retests it against
                    // this frame's). Each sweep is compiled on its own and a box on an edge may not answer the same twice:
                    // a cluster sweep 2 defers past the claim is drawn here instead, and one it keeps past the records'
                    // claim is deferred.
                    const bool hidden = gCamDeferCap != 0u && BoxHiddenLastFrame(ctr, ext);
                    if (!write)
                    {
                        if (hidden) { InterlockedAdd(gsDeferred, 1u); }
                        else { InterlockedAdd(gsRecords, c.meshletCount); }
                    }
                    else
                    {
                        bool deferredNow = false;
                        if (hidden)
                        {
                            uint d;
                            InterlockedAdd(gsDeferred, 1u, d);
                            if (d < gsDeferredClaimed)
                            {
                                TessyCamDeferred[gsDeferredBase + d] = uint4(cid, clusterIndex, camId, baseVertex);
                                deferredNow = true;
                            }
                        }
                        if (!deferredNow)
                        {
                            m = c.meshletCount;
                            firstMeshlet = c.firstMeshlet;
                            triCount = c.triCount;
                        }
                    }
                }
            }
            if (write) // uniform
            {
                uint at = 0u;
                if (ordered)
                {
                    uint placed;
                    at = cursor + OrderedPrefix(m, slot, placed);
                    cursor += placed;
                }
                else if (m != 0u)
                {
                    InterlockedAdd(gsRecords, m, at);
                }
                if (m != 0u && at + m > gsClaimed && gCamDeferCap != 0u)
                {
                    uint d;
                    InterlockedAdd(gsDeferred, 1u, d);
                    if (d < gsDeferredClaimed)
                    {
                        TessyCamDeferred[gsDeferredBase + d] = uint4(cid, clusterIndex, camId, baseVertex);
                        m = 0u;
                    }
                }
                for (uint s = 0u; s < m && at + s < gsClaimed; ++s)
                {
                    TessyVisible[gRecordBase + gsRunBase + at + s] =
                        uint4(cid, firstMeshlet + s, baseVertex, (gFlags & kFlagClusterId) != 0u ? camId : 0u);
                }
                if (m != 0u && at + m <= gsClaimed)
                {
                    tris += triCount;
                    dagDrawn += dag ? 1u : 0u;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync(); // gsPrefix / gsWaveSum are the next batch's
    }
    if (ordered && slot == 0u) { gsRecords = cursor; } // CSMain reads it after its barrier
}

[numthreads(TESSY_CULL_THREADS, 1, 1)]
[RootSignature(TESSY_ATLAS_CULL_RS)]
void CSMain(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    // The dispatch has exactly gNumViews x gSplit groups; this one's run is its own slot (gid.x), its view gid.x / gSplit.
    const uint split = max(gSplit, 1u);
    const uint view = gid.x / split;
    const uint slice = gid.x % split;
    if (view >= gNumViews) { return; } // uniform: the whole group
    if (gRowSet == 1u)
    {
        const uint s = gFrustumFirst + view; // the view's slot
        if (((gStaticViews[s / 128u][(s / 32u) % 4u] >> (s % 32u)) & 1u) == 0u) // uniform
        {
            if (gi == 0u)
            {
                const uint info = gRowSet * gViewStride + gid.x;
                TessyAtlasViewInfo[info] = uint2(0u, 0u);
                TessyAtlasArgs.Store3(info * 12u, uint3(0u, 0u, 1u));
            }
            return;
        }
    }
    const bool byRun = (gFlags & kFlagByRun) != 0u;
    const ViewFrustum vf = Frustums[gFrustumFirst + (byRun ? 0u : view)];
    const uint row = (gFirstView + (byRun ? 0u : view)) * gNumGroups;
    if (gi == 0u)
    {
        gsEntries = 0u; gsRecords = 0u; gsWaves = 0u; gsLeftClassic = 0u; gsDeferred = 0u; gsDagVgs = 0u;
        gsDagEye = (byRun || (gFlags & kFlagCameraCut) != 0u) ? gDagCam : ViewDag[gFrustumFirst + view];
        gsRectSize = float2(0.0f, 0.0f);
        if ((gFlags & kFlagRectCull) != 0u)
        {
            if (gRowSet == 3u) { gsRectVP = SunPartitions[view].lightViewProj; gsRectSize = gRectSize; }
            else if (byRun) { gsRectVP = gCamViewProj; gsRectSize = float2(gCamViewRect.zw - gCamViewRect.xy); }
            else
            {
                const TessyViewRect r = ViewRects[gFrustumFirst + view];
                gsRectVP = r.viewProj;
                gsRectSize = r.size.xy;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // This thread's place in wave order (Sweep says why): its wave's index, taken once by the wave's first lane, and its lane.
    uint waveIndex = 0u;
    if (WaveIsFirstLane()) { InterlockedAdd(gsWaves, 1u, waveIndex); }
    const uint slot = WaveReadLaneFirst(waveIndex) * WaveGetLaneCount() + WaveGetLaneIndex();
    // Gather the row's records with casters AND clusters; a group with a DAG (N7d) apart, for its casters' bands (N7e).
    // The camera's rows gather in ascending virtual group (OrderedPrefix, Sweep says why); the shadow rows by atomics.
    if ((gFlags & kFlagOrdered) == 0u) // uniform
    {
        for (uint vg = slice + gi * split; vg < gNumGroups; vg += kThreads * split) // this group's slice of the row
        {
            if (byRun && VgRun[vg] != view) { continue; }
            const uint instances = Args.Load((row + vg) * kArgStride + 4u);
            if (instances == 0u) { continue; }
            // kFlagCameraCut: only where the camera's runs draw the cut too (a group without a mesh shader twin draws its
            // discrete tier there, and here then as well).
            const bool dag = gDagOn != 0u && gsDagEye.w > 0.0f && DagGroups[vg / kVgLods].y != 0u &&
                             ((gFlags & kFlagCameraCut) == 0u || VgRun[vg] != 0xFFFFFFFFu);
            if (!dag && TessyVg[vg].y == 0u) { gsLeftClassic = 1u; continue; }
            uint idx;
            if (dag && gDagBand != 0u)
            {
                InterlockedAdd(gsDagVgs, 1u, idx);
                if (idx < kMaxDagVgs)
                {
                    gsDagVg[idx] = vg;
                    gsDagStart[idx] = Args.Load((row + vg) * kArgStride + 16u);
                    gsDagInst[idx] = instances;
                    continue;
                }
            }
            InterlockedAdd(gsEntries, 1u, idx);
            if (idx < kMaxEntries)
            {
                gsEntryVg[idx] = vg | (dag ? kEntryDag : 0u);
                gsEntryStart[idx] = Args.Load((row + vg) * kArgStride + 16u);
                gsEntryInst[idx] = instances;
            }
            else { gsLeftClassic = 1u; }
        }
    }
    else
    {
        // Thread `slot` takes the i-th virtual group of each step -- of the run's commands in the classic merged draw's
        // order (a G-buffer run: the clusters then resolve a tie in depth as the classic draw did), else of the row in
        // ascending order (the cluster view); the step's DAG groups, then its entries, are laid out in that order. The
        // cursors are the same in every thread (OrderedPrefix's totals).
        const uint2 cmds = byRun ? uint2(RunCmds[2u * view], RunCmds[2u * view + 1u]) : uint2(0u, 0u);
        const uint n = byRun ? cmds.y : (gNumGroups > slice ? (gNumGroups - slice + split - 1u) / split : 0u);
        uint dagCursor = 0u, entryCursor = 0u;
        for (uint b = 0u; b < n; b += kThreads)
        {
            const uint i = b + slot;
            const uint vg = byRun ? (i < n ? RunCmds[cmds.x + i] : 0u) : slice + i * split;
            uint instances = 0u;
            bool dag = false, want = false;
            if (i < n && vg < gNumGroups && (!byRun || VgRun[vg] == view))
            {
                instances = Args.Load((row + vg) * kArgStride + 4u);
                if (instances != 0u)
                {
                    dag = gDagOn != 0u && gsDagEye.w > 0.0f && DagGroups[vg / kVgLods].y != 0u &&
                          ((gFlags & kFlagCameraCut) == 0u || VgRun[vg] != 0xFFFFFFFFu);
                    if (!dag && TessyVg[vg].y == 0u) { gsLeftClassic = 1u; }
                    else { want = true; }
                }
            }
            const bool toDag = want && dag && gDagBand != 0u;
            uint placed;
            const uint dagIdx = dagCursor + OrderedPrefix(toDag ? 1u : 0u, slot, placed);
            dagCursor += placed;
            const bool dagKept = toDag && dagIdx < kMaxDagVgs;
            if (dagKept)
            {
                gsDagVg[dagIdx] = vg;
                gsDagStart[dagIdx] = Args.Load((row + vg) * kArgStride + 16u);
                gsDagInst[dagIdx] = instances;
            }
            const bool toEntry = want && !dagKept;
            const uint idx = entryCursor + OrderedPrefix(toEntry ? 1u : 0u, slot, placed);
            entryCursor += placed;
            if (toEntry)
            {
                if (idx < kMaxEntries)
                {
                    gsEntryVg[idx] = vg | (dag ? kEntryDag : 0u);
                    gsEntryStart[idx] = Args.Load((row + vg) * kArgStride + 16u);
                    gsEntryInst[idx] = instances;
                }
                else { gsLeftClassic = 1u; }
            }
        }
        if (slot == 0u) { gsDagVgs = dagCursor; gsEntries = entryCursor; }
    }
    GroupMemoryBarrierWithGroupSync();
    // N7e: a block of caster entries per DAG group while they fit, else one plain DAG entry (every caster, every level).
    // A block is handed out only if every DAG group after it still has room for its plain entry: blocks given first come
    // served left wind_test's grove run (156 blocks) without room for 18 groups -- in some frames a rock's -- which fell
    // to the classic draw and its discrete tier, a pop from cut to tier and back from frame to frame.
    if (gi == 0u)
    {
        uint casters = 0u;
        const uint dagVgs = min(gsDagVgs, kMaxDagVgs);
        for (uint j = 0u; j < dagVgs; ++j)
        {
            gsDagPrefix[j] = casters;
            const uint later = dagVgs - 1u - j; // the plain entries kept for the groups after this one
            if (gsEntries + gsDagInst[j] + later <= kMaxEntries)
            {
                gsDagBlock[j] = gsEntries;
                gsEntries += gsDagInst[j];
                casters += gsDagInst[j];
            }
            else
            {
                gsDagBlock[j] = 0xFFFFFFFFu;
                if (gsEntries < kMaxEntries)
                {
                    gsEntryVg[gsEntries] = gsDagVg[j] | kEntryDag;
                    gsEntryStart[gsEntries] = gsDagStart[j];
                    gsEntryInst[gsEntries] = gsDagInst[j];
                    ++gsEntries;
                }
                else { gsLeftClassic = 1u; }
            }
        }
        gsDagCasters = casters;
    }
    GroupMemoryBarrierWithGroupSync();
    // Fill the blocks: each caster its band at its distance -- none for the second entry of a crossfading caster (the
    // cut draws it once, from its own tier's).
    {
        const uint dagVgs = min(gsDagVgs, kMaxDagVgs);
        for (uint n = gi; n < gsDagCasters; n += kThreads)
        {
            // Its DAG group: the last one with a block that starts at or before it (the groups are few: a walk).
            uint j = 0u;
            for (uint m = 0u; m < dagVgs; ++m)
            {
                if (gsDagBlock[m] != 0xFFFFFFFFu && gsDagPrefix[m] <= n) { j = m; }
            }
            const uint i = n - gsDagPrefix[j];
            const uint vg = gsDagVg[j];
            const uint cid = VisibleList[gsDagStart[j] + i];
            uint2 band = uint2(0u, 0u);
            if (!byRun || min(CasterLods[cid] & kCasterLodMask, kVgLods - 1u) == vg % kVgLods)
            {
                const float4x4 world = Instances[cid].world;
                band = DagBand(vg / kVgLods, world, TessyWorldScale(world));
            }
            const uint e = gsDagBlock[j] + i;
            gsEntryVg[e] = vg | kEntryInst;
            gsEntryStart[e] = cid;
            gsEntryFirst[e] = band.x;
            gsEntryInst[e] = band.y;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint entries = min(gsEntries, kMaxEntries);
    uint tested = 0u, tris = 0u, dagDrawn = 0u, rectDropped = 0u;
    Sweep(row, entries, slot, false, vf, tested, tris, dagDrawn, rectDropped);
    if (gi == 0u)
    {
        const uint records = gsRecords;
        uint base = 0u;
        if (records != 0u) { TessyAtlasCounter.InterlockedAdd(gCounterAt.x, records, base); }
        bool fits = records == 0u || base + records <= gRecordCap;
        // N5: the deferred clusters' room in the frame's list; past its capacity the run stays classic, its claimed
        // slots dead (below) -- the post pass reads every slot up to the claim.
        const uint deferred = gsDeferred;
        uint dbase = 0u;
        if (deferred != 0u) { TessyAtlasCounter.InterlockedAdd(20u, deferred, dbase); }
        fits = fits && (deferred == 0u || dbase + deferred <= gCamDeferCap);
        gsFits = fits;
        if (!fits) { TessyAtlasCounter.InterlockedAdd(gCounterAt.y, 1u); }
        gsRunBase = base;
        gsClaimed = records;
        gsRecords = 0u;
        gsDeferredBase = dbase;
        gsDeferredClaimed = dbase < gCamDeferCap ? min(deferred, gCamDeferCap - dbase) : 0u;
        gsDeferred = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gsFits) { Sweep(row, entries, slot, true, vf, tested, tris, dagDrawn, rectDropped); }
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
        const uint info = gRowSet * gViewStride + gid.x;
        TessyAtlasViewInfo[info] = uint2(gRecordBase + gsRunBase, fitted);
        TessyAtlasArgs.Store3(info * 12u, fitted == 0u ? uint3(0u, 0u, 1u)
                                                      : uint3(min(fitted, kTessyMsRow), (fitted + kTessyMsRow - 1u) / kTessyMsRow, 1u));
        // The camera G-buffer's run, taken whole: its classic merged commands are all empty now -- the count says skip
        // them (0); otherwise all of them, by their exact number. NOT ~0: ExecuteIndirect is specified to take
        // min(count, MaxCommandCount), but with 0xFFFFFFFF in the count buffer this driver (NVIDIA 32.0.16.2101) drew
        // NOTHING -- every group the run left classic vanished (2026-10-30, owner: a rock "dithering" under the DAG: its
        // group fell out of the kernel's entries in some frames and then out of the frame).
        if (byRun) { TessyAtlasArgs.Store(gClassicCountOffset + view * 4u, (gsFits && gsLeftClassic == 0u) ? 0u : RunCmds[2u * view + 1u]); }
    }
    const uint wTested = WaveActiveSum(tested);
    if (WaveIsFirstLane() && wTested != 0u) { TessyAtlasCounter.InterlockedAdd(gCounterAt.z, wTested); }
    const uint wRect = WaveActiveSum(rectDropped); // sweep 1's: a box that holds no sample (kFlagRectCull)
    if (WaveIsFirstLane() && wRect != 0u) { TessyAtlasCounter.InterlockedAdd(gRectCountAt, wRect); }
    if (gCounterAt.w != 0xFFFFFFFFu)
    {
        const uint wTris = WaveActiveSum(tris), wDag = WaveActiveSum(dagDrawn);
        if (WaveIsFirstLane() && wTris != 0u) { TessyAtlasCounter.InterlockedAdd(gCounterAt.w, wTris); }
        if (WaveIsFirstLane() && wDag != 0u) { TessyAtlasCounter.InterlockedAdd(gCounterAt.w + 4u, wDag); }
    }
}
