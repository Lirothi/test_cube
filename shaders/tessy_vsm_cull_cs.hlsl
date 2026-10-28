// Tessy N3 (docs/tessy_plan.md): the VSM page render's CLUSTER cull. The page setup (vsm_page_setup_cs.hlsl) hands
// every (dirty page x caster bucket) whose mesh has clusters to this pass as a RUN instead of drawing the bucket
// whole: {physical page, its first entry in PageVisibleList, how many casters, virtual group}. A run's clusters are
// the virtual group's (ShadowGpuData's TessyVg table: that group's submesh at that shadow LOD), and each (caster,
// cluster) pair of the run is one test -- the cluster's world box, padded by how far this frame's wind can carry its
// vertices, against the page's six planes (the setup's own PageIntersects). A survivor is one record of
// TessyVisible, which the VSM_TESSY permutation of shadow_indirect_csm.hlsl draws as DrawInstanced(128 * 3, N):
// UE's VS raster path (NaniteRasterizer.usf:3161-3222), one instance per cluster, no index buffer built.
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

// The page outputs it reads (the runs, the list, the projections) are read through the UAVs the setup wrote them
// with: this pass runs right after it, before the render's consume point turns them into draw inputs.
#define TESSY_VSM_CULL_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
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
};

// Matches render::InstancePerObject (224 bytes), as shadow_indirect_csm.hlsl reads it.
struct InstancePerObject
{
    float4x4 world;
    float4x4 prevWorld;
    float4   baseColor;
    float2   metalRough;
    float    alphaCutoff;
    float    mrMultiply;
    float4   texOffsScale;
    float4   texFlags;
    uint     objectId;
    uint3    _instPad1;
    float    windStrength;
    float    windLeafScale;
    float    windFoliage;
    float    windTrunkStiff;
};

// Matches ShadowGpuData::TessyCluster (48 bytes).
struct TessyCluster
{
    uint   firstIndex;   // mega-absolute
    uint   triCount;
    float  windAlongMax;
    float  windLeafMax;
    float3 center;       // object space
    float  radius;
    float3 extent;
    float  pad;
};

StructuredBuffer<InstancePerObject> Instances       : register(t0);
StructuredBuffer<TessyCluster>      Clusters        : register(t1);
StructuredBuffer<uint2>             TessyVg         : register(t2); // per virtual group {first cluster, count}
StructuredBuffer<uint4>             GroupLodMega    : register(t3); // per virtual group; .w = the mesh's base vertex
ByteAddressBuffer                   MegaVB          : register(t4); // the validator's vertices (VertexPNTUV, 52 B)
StructuredBuffer<uint>              MegaIB          : register(t5);

RWStructuredBuffer<uint4> TessyRuns       : register(u0); // {page, first list entry, casters, virtual group}
RWStructuredBuffer<uint>  PageVisibleList : register(u1); // caster id | page << 22
RWByteAddressBuffer       PageProj        : register(u2); // 256 bytes per page (the setup's off-center projection + wind)
RWStructuredBuffer<uint4> TessyVisible    : register(u3); // {caster id | page << 22, mega first index, triangles, base vertex}
// Byte layout (VirtualShadowMap::TessyStats): [0..15] the draw's D3D12_DRAW_ARGUMENTS {384, clusters, 0, 0},
// [16] runs the setup appended, [20..32] the setup's pair counts, [36] clusters tested, [40] their triangles drawn,
// [44] clusters past TessyVisible's capacity, [48] runs past TessyRuns', [52]/[56] the validator's misses / worst (um), [60]/[64] the growth needed / given (cm).
RWByteAddressBuffer       TessyStats      : register(u4);

static const uint kCasterMask = (1u << 22) - 1u; // vsm::kPageIdShift

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

static float2 g_lastDF = float2(0.0f, 0.0f); // the last WindPadBox's (D, F), for the validator

// Pads the cluster's world box (ctr, ext) by how far this frame's wind can carry its vertices (see the header):
// WindOffset's terms with the per-tree values exact and the per-vertex weights at the cluster's maxima. mode 1 =
// directional, 2 = the isotropic 2D + F (the A/B).
void WindPadBox(InstancePerObject ip, TessyCluster c, float4 w0, float4 w1, uint mode, inout float3 ctr, inout float3 ext)
{
    if (ip.windStrength <= 0.0f || w1.x <= 0.0f) { return; }
    const float3 origin = float3(ip.world._41, ip.world._42, ip.world._43);
    const float2 groveHash = WindHash2(origin.xz);
    const float amp = w1.x * ip.windStrength * (1.0f + kWindGroveAmp * (groveHash.x * 2.0f - 1.0f));
    const float stiff = max(ip.windTrunkStiff * (1.0f + kWindGroveStiff * (groveHash.y * 2.0f - 1.0f)), 0.05f);
    const float p = w0.x * w1.y + dot(origin.xz, float2(0.13f, 0.17f)) + groveHash.x * 6.2831853f;
    const float osc = sin(p) + 0.5f * sin(p * 2.3f + 1.7f);
    const float bend = (kWindBendSteady + kWindBendOsc * osc) * w1.z;
    const float f = WindBendProfile(c.windAlongMax) / stiff;
    const float leaf = ip.windFoliage * c.windLeafMax;
    const float want = abs(amp * bend * leaf * kWindFoliageStream);
    const float ks = kWindLeafShear * c.windLeafMax * ip.windLeafScale;
    const float stream = (ip.windLeafScale > 0.0f) ? min(want, ks) : want; // want*ks/(ks+want) <= both
    // 1 % and a millimetre over each bound: the vertex shader evaluates the same terms in another order.
    const float D = (abs(amp * bend * f) + stream) * 1.01f + 0.001f;
    const float F = abs(amp * w1.z * leaf * kWindFlutterAmp) * 1.01f + 0.001f;
    g_lastDF = float2(D, F); // the validator's breakdown

    const float3 rel = ctr - origin;                                   // the box about the pivot
    const float3 nearest = max(abs(rel) - ext, 0.0f);
    const float lMin = length(nearest);
    if (mode == 2u)
    {
        ext += 2.0f * D + F;
        return;
    }
    // Whatever the geometry: |c| <= 2D (above) and <= 2 L_max (both ends lie on the sphere of radius L). Each axis's
    // interval below is cut to this; a cluster within ~D of the pivot gets it alone.
    const float lMax = length(abs(rel) + ext);
    const float R = min(2.0f * D, 2.0f * lMax);
    const float3 flutter = float3(0.5f * F, F, 0.5f * F);
    if (lMin <= 1.05f * D)
    {
        ext += R + flutter;
        return;
    }
    const float3 w = float3(w0.z, 0.0f, w0.w);                         // unit wind direction
    // L > D keeps phi acute (d is not the triangle's longest side): sin(phi) <= d / (L - d), 1 - cos(phi) <= sin^2(phi).
    const float S = D * lMin / (lMin - D);
    const float T = D * D * lMin / ((lMin - D) * (lMin - D));
    // r^ = rel / |rel| per component over the box, |rel| in [lMin, lMax].
    const float3 rLo = rel - ext, rHi = rel + ext;
    const float3 nLo = clamp(rLo / select(rLo >= 0.0f, lMax, lMin), -1.0f, 1.0f);
    const float3 nHi = clamp(rHi / select(rHi >= 0.0f, lMin, lMax), -1.0f, 1.0f);
    // m = w.r^ (w.y = 0)
    const float2 mx = float2(min(w.x * nLo.x, w.x * nHi.x), max(w.x * nLo.x, w.x * nHi.x));
    const float2 mz = float2(min(w.z * nLo.z, w.z * nHi.z), max(w.z * nLo.z, w.z * nHi.z));
    const float mLo = mx.x + mz.x, mHi = mx.y + mz.y;
    float3 cLo, cHi;
    [unroll] for (int i = 0; i < 3; ++i)
    {
        // g = w_i - m r^_i over the intervals; c_i = s g - t r^_i with s in [0, S], t in [0, T].
        const float p0 = mLo * nLo[i], p1 = mLo * nHi[i], p2 = mHi * nLo[i], p3 = mHi * nHi[i];
        const float gLo = w[i] - max(max(p0, p1), max(p2, p3));
        const float gHi = w[i] - min(min(p0, p1), min(p2, p3));
        cLo[i] = max(min(0.0f, S * gLo) + min(0.0f, -T * nHi[i]), -R);
        cHi[i] = min(max(0.0f, S * gHi) + max(0.0f, -T * nLo[i]), R);
    }
    const float3 lo = ctr - ext + cLo - flutter;
    const float3 hi = ctr + ext + cHi + flutter;
    ctr = 0.5f * (lo + hi);
    ext = 0.5f * (hi - lo);
}

[numthreads(8, 8, 1)]
[RootSignature(TESSY_VSM_CULL_RS)]
void CSMain(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (gid.x == 0u && lane == 0u) { TessyStats.Store(0u, 384u); } // VertexCountPerInstance: 128 triangles x 3
    const uint runs = min(TessyStats.Load(16u), gRunCap);
    uint tested = 0u, drawnTris = 0u, lost = 0u;
    for (uint r = gid.x; r < runs; r += gGroups)
    {
        const uint4 run = TessyRuns[r];
        const uint2 range = TessyVg[run.w];
        if (range.y == 0u) { continue; } // the setup appends only groups with clusters; never divide by 0
        const uint baseVertex = GroupLodMega[run.w].w;
        float4 planes[6];
        float4 w0, w1;
        PagePlanes(run.x, planes, w0, w1);
        const float4 w2 = asfloat(PageProj.Load4(run.x * 256u + 224u)); // (camPos, fade end): the validator's fade
        const uint total = run.z * range.y;
        for (uint i = lane; i < total; i += 64u)
        {
            const uint k = i / range.y;
            const TessyCluster c = Clusters[range.x + (i - k * range.y)];
            const uint entry = PageVisibleList[run.y + k];
            const InstancePerObject ip = Instances[entry & kCasterMask];
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
            uint slot;
            TessyStats.InterlockedAdd(4u, 1u, slot); // the draw's InstanceCount
            uint cap, stride;
            TessyVisible.GetDimensions(cap, stride);
            if (slot < cap)
            {
                TessyVisible[slot] = uint4(entry, c.firstIndex, c.triCount, baseVertex);
                drawnTris += c.triCount;
            }
            else
            {
                ++lost;
            }
        }
    }
    // One atomic per wave per counter, not per thread.
    const uint wTested = WaveActiveSum(tested);
    const uint wTris = WaveActiveSum(drawnTris);
    const uint wLost = WaveActiveSum(lost);
    if (WaveIsFirstLane())
    {
        if (wTested != 0u) { TessyStats.InterlockedAdd(36u, wTested); }
        if (wTris != 0u) { TessyStats.InterlockedAdd(40u, wTris); }
        if (wLost != 0u) { TessyStats.InterlockedAdd(44u, wLost); }
    }
}
