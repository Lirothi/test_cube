// Tessy N3 (docs/tessy_plan.md): the VSM page render's CLUSTER cull. The page setup (vsm_page_setup_cs.hlsl) hands
// every (dirty page x caster bucket) whose mesh has clusters to this pass as a RUN instead of drawing the bucket
// whole: {physical page, its first entry in PageVisibleList, how many casters, virtual group}. A run's clusters are
// the virtual group's (ShadowGpuData's TessyVg table: that group's submesh at that shadow LOD), and each (caster,
// cluster) pair of the run is one test -- the cluster's world box, padded by how far this frame's wind can carry its
// vertices, against the page's six planes (the setup's own PageIntersects). A survivor is one record of
// TessyVisible, which the VSM_TESSY permutation of shadow_indirect_csm.hlsl draws as DrawInstanced(128 * 3, N):
// UE's VS raster path (NaniteRasterizer.usf:3161-3222), one instance per cluster, no index buffer built -- or, N6,
// its mesh shader (MSMain, one group per cluster, tessy_ms_args_cs.hlsl turning the count into DispatchMesh).
//
// UE gives each surviving cluster to the pages it overlaps in 4x4 windows (NaniteClusterCulling.usf:877-936); ours
// arrive already split by page -- the scatter made the (page, caster) pairs -- so the test is per page.
//
// The wind pad (UE pads the cluster box by MaxWPOExtent, NaniteClusterCulling.usf:160-169): a caster's box is
// deliberately NOT padded (ShadowGpuData::FillBounds -- a whole-caster pad cost the page render 2.5x), which was
// harmless while a caster was drawn whole into every page its box touched; a 128-triangle box is small enough that
// a swaying crown would leave it for a page it is not drawn into -- a hole in the shadow. The pad is WindOffset's
// (wind.hlsli) own bound for THIS frame: the per-tree amplitude, stiffness and phase are exact (they are functions of
// the tree's origin and the frame's wind, which are known here), the per-vertex weights are the cluster's maxima
// (Mesh::Cluster::windAlongMax / windLeafMax, every term grows with them): d <= D, the shear WindOffset adds along the
// wind w, and F, its flutter; the world-distance fade only shrinks them.
//
// PER AXIS, WITH ITS SIGN, as plan 4.6 asks. The isotropic 2D + F (the first version) let 5x the clusters through in
// the grove; a chord-and-orthogonal bound (the second) still gave the boxes 19x the growth the validator measured they
// need (the shear bound D itself was only 1.3x the true largest displacement -- the slack was all geometry). The bend
// is a ROTATION about the pivot: q = rel + d w is renormalised to |rel| = L, turning r^ = rel / L by phi toward w in
// their plane. Exactly (law of sines: sin phi = d sin(theta) / |q|):
//     c = s (w - (w.r^) r^) - t r^,   s = L d / |q|,   t = L (1 - cos phi).
// With L >= L_min > 2D (phi < 90 degrees, |q| >= L - d): s in [0, D L_min / (L_min - D)], t in [0, D^2 L_min /
// (L_min - D)^2]. Each component of r^ over the box is an interval (rel's over L in [L_min, L_max]), so each component
// of c is one by interval arithmetic: the crown's box grows DOWNWIND by ~D, DOWN by the drop, barely across -- about
// the growth it needs. Every axis is also cut to |c| <= min(2D, 2 L_max) (||q| - L| + |q - rel| <= 2d; both ends on the
// sphere of radius L), which is all a cluster reaching within ~D of the pivot (a trunk's foot, a fern) gets. The
// flutter: +-F up, +-F/2 across.
#pragma pack_matrix(row_major)
#include "wind.hlsli"
#include "tessy_common.hlsli" // TessyCluster, the TessyVisible record

// The page outputs it reads (the runs, the list, the projections) are read through the UAVs the setup wrote them
// with: this pass runs right after it, before the render's consume point turns them into draw inputs.
#define TESSY_VSM_CULL_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=10, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CullCB : register(b0)
{
    uint gRunCap;      // TessyRuns' capacity (the setup stops appending there)
    uint gGroups;      // thread groups dispatched: each walks runs gid, gid + gGroups, ...
    uint gWindPad;     // the sway pad: 1 directional, 2 isotropic (the A/B), 0 none (only to SEE the holes it prevents)
    // 1 = the pad's VALIDATOR (tessy.validate): every tested cluster's vertices are swayed exactly as the shadow VS
    // sways them and their box checked against the padded one -- [52] how many left it, [56] by how far at most (um).
    // Costs a frame's worth of vertex work per page; a debug mode.
    uint gValidate;
    // N6: the mesh shader's triangle culls, handed on in each record's .w (tessy_common.hlsli kTessyMsCull*).
    uint gMsCull;
    // N6: 1 = the mesh shader draws: a surviving cluster appends one record per MESHLET (its index in .y); 0 = the VS
    // path: one record per cluster.
    uint gMsMeshlets;
    // N7c: 1 = a run of a group with a cluster DAG takes the DAG's cut instead of its discrete LOD's clusters -- on a
    // clipmap page (N7c) and, N7e, on a local light's (perspective) one; gDagTexels = the shadow texels its error may span.
    uint gDagOn;
    float gDagTexels;
};

#include "tessy_cull_common.hlsli" // InstancePerObject, WindPadBox (shared with tessy_atlas_cull_cs.hlsl)

StructuredBuffer<InstancePerObject> Instances       : register(t0);
StructuredBuffer<TessyCluster>      Clusters        : register(t1);
StructuredBuffer<uint2>             TessyVg         : register(t2); // per virtual group {first cluster, count}
StructuredBuffer<uint4>             GroupLodMega    : register(t3); // per virtual group; .w = the mesh's base vertex
ByteAddressBuffer                   MegaVB          : register(t4); // the validator's vertices (VertexPNTUV, 52 B)
StructuredBuffer<uint>              MegaIB          : register(t5);
// N7c: the cluster DAG (ShadowGpuData's N7 tables): per cluster its LOD record, per mesh group {first DAG cluster, count,
// first level, levels}, per level {first cluster, count, min lodError, max parentError (float bits)}.
StructuredBuffer<TessyClusterLod>   ClusterLods     : register(t6);
StructuredBuffer<uint4>             DagGroups       : register(t7);
StructuredBuffer<uint4>             DagLevels       : register(t8);
StructuredBuffer<float4>            DagSpheres      : register(t9); // N7e: per mesh group, around its DAG's spheres
static const uint kVgLods = 4u; // render::kMaxShadowLods: virtual group = mesh group * 4 + LOD

RWStructuredBuffer<uint4> TessyRuns       : register(u0); // {page, first list entry, casters, virtual group}
RWStructuredBuffer<uint>  PageVisibleList : register(u1); // caster id | page << 22
RWByteAddressBuffer       PageProj        : register(u2); // 256 bytes per page (the setup's off-center projection + wind)
RWStructuredBuffer<uint4> TessyVisible    : register(u3); // {caster id | page << 22, cluster / meshlet, base vertex, MS culls}
// Byte layout (VirtualShadowMap::TessyStats): [0..15] the draw's D3D12_DRAW_ARGUMENTS {384, clusters, 0, 0},
// [16] runs the setup appended, [20..32] the setup's pair counts, [36] clusters tested, [40] their triangles drawn,
// [44] clusters past TessyVisible's capacity, [48] runs past TessyRuns', [52]/[56] the validator's misses / worst (um), [60]/[64] the growth needed / given (cm),
// [68..76] the validator's breakdown, [80..91] the mesh shader's D3D12_DISPATCH_MESH_ARGUMENTS (tessy_ms_args_cs.hlsl),
// [96] clusters drawn ([4] counts records: one a cluster on the VS path, one a meshlet on the mesh shader's).
RWByteAddressBuffer       TessyStats      : register(u4);

static const uint kCasterMask = (1u << 22) - 1u; // vsm::kPageIdShift
static const float kPageTexels = 128.0f;         // vsm::kPageSize

// N7c: the run's casters' thresholds -- each caster's object-space error at which a DAG level becomes coarse enough,
// smallest and largest over the run: they decide the DAG levels the run can cut (the cull walks casters x one range).
groupshared uint gsTMin;
groupshared uint gsTMax;

// N7c/N7e: a page's DAG cut. A shadow texel of the page spans 2 / (kPageTexels |column x|) x w metres at its clip w (the
// view depth for a light's perspective page, 1 for an orthographic clipmap page), so an error e x the caster's scale
// spans e s K / w texels, K = |column x| kPageTexels / 2 / tessy.lodShadowTexels; w of a sphere's nearest point is
// linear: w(centre) - radius s |column w|. One function for a cluster's own error and its parent's, `precise`: the two
// must answer alike for siblings or the cut tears (tessy_atlas_cull_cs DagTooCoarse).
struct PageDag
{
    float3 colW;   // the projection's w column (0 for an orthographic page)
    float  tW;     // its translation (1 for an orthographic page)
    float  colWLen;
    float  k;
};
bool PageDagTooCoarse(float error, float4 sphere, float4x4 world, float scale, PageDag p)
{
    precise const float3 c = mul(float4(sphere.xyz, 1.0f), world).xyz;
    precise const float w = max(dot(c, p.colW) + p.tW - sphere.w * scale * p.colWLen, 1.0e-4f);
    precise const float projected = error * scale * p.k;
    return projected > w;
}

// The page's six planes from its off-center projection (vsm_page_setup_cs.hlsl builds the same).
void PagePlanes(uint page, out float4 planes[6], out float4 w0, out float4 w1)
{
    const uint b = page * 256u;
    const float4x4 pm = float4x4(asfloat(PageProj.Load4(b + 0u)), asfloat(PageProj.Load4(b + 16u)),
                                 asfloat(PageProj.Load4(b + 32u)), asfloat(PageProj.Load4(b + 48u)));
    w0 = asfloat(PageProj.Load4(b + 192u)); // (time, prevTime, dirX, dirZ)
    w1 = asfloat(PageProj.Load4(b + 208u)); // (swayAmp -- 0 on a rigid page, freq, gust, prevGust)
    const float4x4 pt = transpose(pm);
    planes[0] = pt[3] + pt[0];
    planes[1] = pt[3] - pt[0];
    planes[2] = pt[3] + pt[1];
    planes[3] = pt[3] - pt[1];
    planes[4] = pt[2];
    planes[5] = pt[3] - pt[2];
}

[numthreads(8, 8, 1)]
[RootSignature(TESSY_VSM_CULL_RS)]
void CSMain(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (gid.x == 0u && lane == 0u) { TessyStats.Store(0u, 384u); } // VertexCountPerInstance: 128 triangles x 3
    const uint runs = min(TessyStats.Load(16u), gRunCap);
    uint tested = 0u, drawnTris = 0u, lost = 0u, drawnClusters = 0u;
    for (uint r = gid.x; r < runs; r += gGroups)
    {
        const uint4 run = TessyRuns[r];
        uint2 range = TessyVg[run.w];
        if (range.y == 0u) { continue; } // the setup appends only groups with clusters; never divide by 0
        const uint baseVertex = GroupLodMega[run.w].w;
        float4 planes[6];
        float4 w0, w1;
        PagePlanes(run.x, planes, w0, w1);
        // N7c: a group with a cluster DAG takes the DAG's cut (PageDagTooCoarse; UE NaniteClusterCulling.usf:287-299): a
        // cluster is drawn when its error fits the page's texels and its parent's does not. On a clipmap page (orthographic:
        // the w column is (0,0,0,1)) a texel is the same size at any distance; N7e: on a local light's page (perspective) it
        // grows with the depth, and the caster's distance to the light decides. The levels that can hold such a cluster for
        // the run's casters are a contiguous run of clusters (the DAG stores them level by level): that is the range.
        bool dag = false;
        PageDag pd;
        const uint4 dg = DagGroups[run.w / kVgLods];
        {
            const uint b = run.x * 256u;
            const float4 c0 = asfloat(PageProj.Load4(b + 0u)), c1 = asfloat(PageProj.Load4(b + 16u)),
                         c2 = asfloat(PageProj.Load4(b + 32u)), c3 = asfloat(PageProj.Load4(b + 48u));
            pd.colW = float3(c0.w, c1.w, c2.w);
            pd.tW = c3.w;
            pd.colWLen = length(pd.colW);
            pd.k = max(length(float3(c0.x, c1.x, c2.x)), 1.0e-12f) * kPageTexels * 0.5f / max(gDagTexels, 1.0e-3f);
            dag = gDagOn != 0u && dg.y != 0u; // uniform over the group (one run at a time)
        }
        if (dag)
        {
            if (lane == 0u) { gsTMin = asuint(3.0e38f); gsTMax = 0u; }
            GroupMemoryBarrierWithGroupSync();
            // Each caster's range of object-space thresholds over its DAG's sphere: w / (s k) at the sphere's nearest and
            // farthest clip w (one value on an orthographic page).
            const float4 sphere = DagSpheres[run.w / kVgLods];
            float tMin = 3.0e38f, tMax = 0.0f;
            for (uint k = lane; k < run.z; k += 64u)
            {
                const float4x4 world = Instances[PageVisibleList[run.y + k] & kCasterMask].world;
                const float s = max(TessyWorldScale(world), 1.0e-6f);
                const float3 c = mul(float4(sphere.xyz, 1.0f), world).xyz;
                const float wc = dot(c, pd.colW) + pd.tW;
                const float reach = sphere.w * s * pd.colWLen;
                tMin = min(tMin, max(wc - reach, 1.0e-4f) / (s * pd.k));
                tMax = max(tMax, max(wc + reach, 1.0e-4f) / (s * pd.k));
            }
            tMin = WaveActiveMin(tMin);
            tMax = WaveActiveMax(tMax);
            if (WaveIsFirstLane()) { InterlockedMin(gsTMin, asuint(tMin)); InterlockedMax(gsTMax, asuint(tMax)); }
            GroupMemoryBarrierWithGroupSync();
            // A level can hold a cut cluster when its smallest error fits the loosest threshold and its largest parent error
            // passes the tightest (widened by a part in 1e4 for the rounding: the cut is still each cluster's own test).
            const float tLoose = asfloat(gsTMax) * 1.0001f, tTight = asfloat(gsTMin) * 0.9999f;
            uint lo = 0xFFFFFFFFu, hi = 0u;
            for (uint k = 0u; k < dg.w; ++k)
            {
                const uint4 l = DagLevels[dg.z + k];
                if (asfloat(l.z) <= tLoose && asfloat(l.w) > tTight)
                {
                    lo = min(lo, l.x);
                    hi = max(hi, l.x + l.y);
                }
            }
            range = lo < hi ? uint2(lo, hi - lo) : uint2(dg.x, 0u);
            GroupMemoryBarrierWithGroupSync(); // gsT* are the next run's
            if (range.y == 0u) { continue; }
        }
        const float4 w2 = asfloat(PageProj.Load4(run.x * 256u + 224u)); // (camPos, fade end): the validator's fade
        const uint total = run.z * range.y;
        for (uint i = lane; i < total; i += 64u)
        {
            const uint k = i / range.y;
            const uint clusterIndex = range.x + (i - k * range.y);
            const TessyCluster c = Clusters[clusterIndex];
            const uint entry = PageVisibleList[run.y + k];
            const InstancePerObject ip = Instances[entry & kCasterMask];
            if (dag)
            {
                // The cut, this caster's: its error fits the page's texels, its parent's does not.
                const TessyClusterLod cl = ClusterLods[clusterIndex];
                const float s = TessyWorldScale(ip.world);
                if (PageDagTooCoarse(cl.lodError, cl.lodSphere, ip.world, s, pd) ||
                    !PageDagTooCoarse(cl.parentError, cl.parentSphere, ip.world, s, pd)) { continue; }
            }
            float3 ctr = mul(float4(c.center, 1.0f), ip.world).xyz;
            float3 ext = abs(ip.world[0].xyz) * c.extent.x + abs(ip.world[1].xyz) * c.extent.y +
                         abs(ip.world[2].xyz) * c.extent.z;
            const float3 rawLo = ctr - ext, rawHi = ctr + ext;
            g_lastDF = float2(0.0f, 0.0f);
            if (gWindPad != 0u) { WindPadBox(ip, c, w0, w1, gWindPad, ctr, ext); }
            ++tested;
            if (gValidate != 0u)
            {
                // The cluster's vertices swayed exactly as WindTransformCore sways them (shadow_indirect_csm.hlsl),
                // the world-distance fade included; any of them outside the padded box is a miss of the bound.
                // Also how much the box had to grow (the six faces' outward moves, mm) against how much the pad grew
                // it -- [60] / [64]: the bound's slack.
                const float3 origin = float3(ip.world._41, ip.world._42, ip.world._43);
                float excess = 0.0f, maxOff = 0.0f;
                float3 exLo = rawLo, exHi = rawHi;
                for (uint v = 0u; v < c.triCount * 3u; ++v)
                {
                    const uint b = (MegaIB[c.firstIndex + v] + baseVertex) * 52u;
                    const float3 objPos = asfloat(MegaVB.Load3(b));
                    const uint col = MegaVB.Load(b + 48u);
                    const float4 weights = float4(col & 0xFFu, (col >> 8) & 0xFFu, (col >> 16) & 0xFFu, col >> 24) * (1.0f / 255.0f);
                    const float3 wp = mul(float4(objPos, 1.0f), ip.world).xyz;
                    float3 off = WindOffset(objPos, wp, origin, ip.windStrength, weights, ip.windFoliage, ip.windTrunkStiff,
                                            ip.windLeafScale, w0.zw, w1.x, w1.y, w1.z, w0.x);
                    if (w2.w > 0.0f) { off *= 1.0f - smoothstep(0.5f * w2.w, w2.w, length(wp.xz - w2.xz)); }
                    const float3 out3 = max(abs(wp + off - ctr) - ext, 0.0f);
                    excess = max(excess, max(out3.x, max(out3.y, out3.z)));
                    exLo = min(exLo, wp + off);
                    exHi = max(exHi, wp + off);
                    maxOff = max(maxOff, length(off));
                }
                if (excess > 1.0e-4f)
                {
                    TessyStats.InterlockedAdd(52u, 1u);
                    TessyStats.InterlockedMax(56u, (uint)min(excess * 1.0e6f, 4.0e9f));
                }
                const float3 need = (rawLo - exLo) + (exHi - rawHi);
                const float3 got = ((rawLo - (ctr - ext)) + ((ctr + ext) - rawHi));
                TessyStats.InterlockedAdd(60u, (uint)((need.x + need.y + need.z) * 100.0f)); // cm
                TessyStats.InterlockedAdd(64u, (uint)((got.x + got.y + got.z) * 100.0f));
                // The breakdown (cm): the largest true displacement of a vertex, the shear bound D, the flutter bound F.
                TessyStats.InterlockedAdd(68u, (uint)(maxOff * 100.0f));
                TessyStats.InterlockedAdd(72u, (uint)(g_lastDF.x * 100.0f));
                TessyStats.InterlockedAdd(76u, (uint)(g_lastDF.y * 100.0f));
            }
            bool inside = true;
            [unroll] for (int p = 0; p < 6; ++p)
            {
                if (dot(planes[p].xyz, ctr) + planes[p].w + dot(abs(planes[p].xyz), ext) < 0.0f) { inside = false; }
            }
            if (!inside) { continue; }
            // The VS path draws the cluster as one instance; the mesh shader one group per meshlet (N6).
            const uint records = gMsMeshlets != 0u ? c.meshletCount : 1u;
            uint slot;
            TessyStats.InterlockedAdd(4u, records, slot); // the draw's InstanceCount / the dispatch's group count
            uint cap, stride;
            TessyVisible.GetDimensions(cap, stride);
            if (slot + records <= cap)
            {
                for (uint s = 0u; s < records; ++s)
                {
                    TessyVisible[slot + s] = uint4(entry, gMsMeshlets != 0u ? c.firstMeshlet + s : clusterIndex, baseVertex, gMsCull);
                }
                drawnTris += c.triCount;
                ++drawnClusters;
            }
            else
            {
                // The cluster is lost whole; its records that still fit are marked dead (they would read last frame's),
                // the ones past the end the draw rejects by GetDimensions.
                for (uint s = slot; s < cap; ++s) { TessyVisible[s] = uint4(kTessyDeadRecord, 0u, 0u, 0u); }
                ++lost;
            }
        }
    }
    // One atomic per wave per counter, not per thread.
    const uint wTested = WaveActiveSum(tested);
    const uint wTris = WaveActiveSum(drawnTris);
    const uint wLost = WaveActiveSum(lost);
    const uint wClusters = WaveActiveSum(drawnClusters);
    if (WaveIsFirstLane())
    {
        if (wTested != 0u) { TessyStats.InterlockedAdd(36u, wTested); }
        if (wTris != 0u) { TessyStats.InterlockedAdd(40u, wTris); }
        if (wLost != 0u) { TessyStats.InterlockedAdd(44u, wLost); }
        if (wClusters != 0u) { TessyStats.InterlockedAdd(96u, wClusters); }
    }
}
