// SDSM analysis (docs/csm_improvement_plan.md S15): five dispatches that turn THIS frame's depth
// buffer into four shadow partitions -- their view-Z intervals, their light-space boxes, their
// projection matrices and the cull planes the caster cull tests against.
//
// Transcribed from the Intel SDSM DX11 sample (D:\Programming\sdsm\sdsm_dx11, Intel Sample Source
// Code License -- permissive with notice):
//   ClearBounds     <- LogPartitions.hlsl:71 ClearZBounds + CustomPartitions.hlsl:38 ClearPartitionBounds
//   ReduceZBounds   <- LogPartitions.hlsl:79 ReduceZBoundsFromGBuffer
//   LogPartitions   <- LogPartitions.hlsl:132 ComputeLogPartitionsFromZBounds
//   ReduceBounds    <- CustomPartitions.hlsl:45 ReduceBoundsFromGBuffer
//   Finalize        <- SDSMPartitions.hlsl:141 ComputePartitionDataFromBounds
// Deviations are listed at the head of sources/rendering/shadows/SdsmPartitions.h; the ones that
// show up in this file are marked DEVIATION where they happen.
//
// ONE PRODUCER RULE: after Finalize, nothing else writes the partition buffer or the SDSM frustum
// buffer for the rest of the frame. Every consumer (the cull, the depth VS, csm_sample) reads.
#pragma pack_matrix(row_major)
#include "sdsm_partitions.hlsli"

#define SDSM_ANALYZE_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// t0: the camera depth buffer at RENDER resolution, reverse-Z (clear 0.0 = the far plane, which
//     is also what the sky leaves behind -- see the accept window in SampleViewZ).
Texture2D                        DepthTex   : register(t0);
// t1: the CPU-uploaded per-view cull planes. Finalize carries slots [gPartitions, count) over
//     VERBATIM (spot + point + clipmap views, which the CPU still owns) and overwrites the
//     directional slots with the partition boxes. Copying rather than binding two buffers keeps
//     shadow_cull_cs.hlsl completely unaware that SDSM exists.
//     Read through SdsmViewFrustum (stride 256), NOT as a flat float4 array: the SRV over that
//     ring is created with StructureByteStride 256, and D3D12 requires the shader's element size
//     to match it.
StructuredBuffer<SdsmViewFrustum> SrcFrustums : register(t1);
// t2: LAST frame's partitions -- a copy taken at the head of this pass, before ClearBounds. The
// HZB occlusion test projects a caster box into the tile the pyramid was RENDERED with, and that
// is last frame's matrix; testing it with this frame's would compare a box against a depth buffer
// drawn from somewhere else.
StructuredBuffer<SdsmPartition>   PrevPartitions : register(t2);

RWStructuredBuffer<SdsmZBounds>    ZBounds    : register(u0);
RWStructuredBuffer<SdsmBoundsUint> Bounds     : register(u1);
RWStructuredBuffer<SdsmPartition>  Partitions : register(u2);
// One flat float4 per plane: view v's plane i is at v * 16 + i (render::kShadowViewPlanes).
RWStructuredBuffer<float4>         OutFrustums : register(u3);
// u4/u5: two CONSTANT BUFFERS the GPU writes and the CPU then binds by address -- the depth pass's
// per-partition PerView block (256 B each, render::sdsm::kViewCbStride) and the cull's
// CascadeHzbCB (560 B). Both are viewed as float4 arrays here and as cbuffers by their consumers;
// that is the whole reason SDSM needs no shader permutation in either of them.
RWStructuredBuffer<float4>         OutViewCBs : register(u4);
RWStructuredBuffer<float4>         OutCullCB  : register(u5);

static const uint kViewPlanes = 16u;   // render::kShadowViewPlanes
static const uint kViewCbFloat4s = 16u; // 256 B / 16 -- render::sdsm::kViewCbStride

// One partition's PerView constant block, in scene_internal::PerViewCB's layout EXACTLY:
//   0..63   viewProj            (the only matrix a depth-only shadow VS reads)
//   64..191 viewProjNoJitter / prevViewProjNoJitter -- unread here, zeroed
//   192..223 the wind tail, byte-identical to the gbuffer's (W5)
//   224..239 constBias, slopeBias, maxSlope, clampNear (S6/S7)
// A wrong offset here does not fail to compile; it silently feeds the depth pass garbage, which is
// why the layout is spelled out rather than referenced.
void SdsmWriteViewCB(uint partition, float4x4 viewProj, float4 bias)
{
    const uint b = partition * kViewCbFloat4s;
    OutViewCBs[b + 0u] = float4(viewProj._11, viewProj._12, viewProj._13, viewProj._14);
    OutViewCBs[b + 1u] = float4(viewProj._21, viewProj._22, viewProj._23, viewProj._24);
    OutViewCBs[b + 2u] = float4(viewProj._31, viewProj._32, viewProj._33, viewProj._34);
    OutViewCBs[b + 3u] = float4(viewProj._41, viewProj._42, viewProj._43, viewProj._44);
    [unroll] for (uint i = 4u; i < 12u; ++i) { OutViewCBs[b + i] = (float4)0.0f; }
    OutViewCBs[b + 12u] = float4(gWindTime, gWindPrevTime, gWindDirX, gWindDirZ);
    OutViewCBs[b + 13u] = float4(gWindSwayAmp, gWindSwayFreq, gWindGustMul, gWindPrevGustMul);
    OutViewCBs[b + 14u] = bias;
    OutViewCBs[b + 15u] = (float4)0.0f;
}

// ---------------------------------------------------------------------------------------------
// S15.4 -- the ACCURATE caster cull volume, Scene.cpp's BuildCascadeCullVolume transcribed (which
// is itself UE's ComputeShadowCullingVolume, DirectionalLightComponent.cpp:101).
//
// WHY IT IS HERE AND NOT A PLAIN BOX. A caster can only shadow the slice if some point of it lies
// on a light ray that enters the slice -- i.e. inside `slice + t * toSun, t >= 0`. That prism is
// dramatically tighter than the slice's light-space AABB whenever the sun is not straight down,
// and MEASURED on this engine it is worth 0.045 ms of Pass_CSM in Legacy (`csm.accurateCull:0`
// costs 0.363 against 0.318). SDSM shipped with the plain box first because the plan assumed a
// box fitted to SAMPLES made the prism unnecessary; the measurement said otherwise -- the box is
// tight in XY but still holds the whole wedge the camera slice occupies only a sliver of.
//
// The CPU version's two precision rules are transcribed WITH it, because they are not incidental:
//  * everything is computed RELATIVE TO THE CAMERA and only the plane offsets move back to world.
//    Cascade 0's near quad is 2 cm across while a corner sits hundreds of metres from the origin;
//    a direction taken across it in absolute world space is float noise.
//  * directions come from FAR-SCALE geometry only: side faces and side edges pass through the
//    apex and take their direction from the corner RAYS, near-quad edges borrow the parallel far
//    edge. Near corners are anchors only, where an absolute error is not amplified.
// ---------------------------------------------------------------------------------------------

// BuildFrustumSliceCornersWS, verbatim. Order is (2 * ndcCorner + {0 near, 1 far}) over the NDC
// corners BL/BR/TR/TL -- the layout the index tables below are written for.
void SdsmSliceCorners(float nearZ, float farZ, out float3 c[8])
{
    const float2 ndc[4] = { float2(-1.0f, -1.0f), float2(1.0f, -1.0f),
                            float2(1.0f, 1.0f), float2(-1.0f, 1.0f) };
    [unroll] for (uint i = 0u; i < 4u; ++i)
    {
        // The RAY through this frustum corner. Which clip-z it is unprojected at does not matter
        // for a perspective projection -- the (x, y) corner is the same ray -- which is why this
        // is correct under reverse-Z without a special case.
        float4 farVS = mul(float4(ndc[i], 1.0f, 1.0f), gInvProj);
        const float3 dirVS = farVS.xyz / farVS.w;
        const float nz = max(1e-6f, dirVS.z);
        c[2u * i + 0u] = mul(float4(dirVS * (nearZ / nz), 1.0f), gInvView).xyz;
        c[2u * i + 1u] = mul(float4(dirVS * (farZ / nz), 1.0f), gInvView).xyz;
    }
}

// Corner names of that layout.
#define SDSM_nBL 0
#define SDSM_fBL 1
#define SDSM_nBR 2
#define SDSM_fBR 3
#define SDSM_nTR 4
#define SDSM_fTR 5
#define SDSM_nTL 6
#define SDSM_fTL 7

// Up to 4 away-facing faces + 6 silhouette edges. Returns how many it wrote; 0 = degenerate slice,
// and the caller then keeps the plain box (always correct, just looser).
uint SdsmCullVolumePlanes(float nearZ, float farZ, float3 toSun, out float4 planes[10])
{
    [unroll] for (uint z = 0u; z < 10u; ++z) { planes[z] = float4(0.0f, 0.0f, 0.0f, 1.0f); }

    float3 cw[8];
    SdsmSliceCorners(nearZ, farZ, cw);
    const float3 origin = gCamPosWS.xyz;
    float3 c[8];
    [unroll] for (uint i = 0u; i < 8u; ++i) { c[i] = cw[i] - origin; }

    float3 sliceCentre = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (uint k = 0u; k < 8u; ++k) { sliceCentre += c[k]; }
    sliceCentre *= 0.125f;

    // Face normals from far-scale geometry; the side faces pass through the apex (the origin).
    const float3 farU = c[SDSM_fBR] - c[SDSM_fBL];
    const float3 farV = c[SDSM_fTL] - c[SDSM_fBL];
    const float3 viewN = cross(farU, farV);
    float3 faceN[6];
    float3 faceA[6];
    faceN[0] = viewN;                             faceA[0] = c[SDSM_nBL]; // Near
    faceN[1] = cross(c[SDSM_fTL], c[SDSM_fBL]);   faceA[1] = float3(0.0f, 0.0f, 0.0f); // Left
    faceN[2] = cross(c[SDSM_fTR], c[SDSM_fBR]);   faceA[2] = float3(0.0f, 0.0f, 0.0f); // Right
    faceN[3] = cross(c[SDSM_fTR], c[SDSM_fTL]);   faceA[3] = float3(0.0f, 0.0f, 0.0f); // Top
    faceN[4] = cross(c[SDSM_fBR], c[SDSM_fBL]);   faceA[4] = float3(0.0f, 0.0f, 0.0f); // Bottom
    faceN[5] = viewN;                             faceA[5] = c[SDSM_fBL]; // Far

    // OUTWARD unit normals (the sliceCentre must fall on the negative side).
    float3 n[6];
    float  d[6];
    [unroll] for (uint f = 0u; f < 6u; ++f)
    {
        const float len = length(faceN[f]);
        if (len < 1e-9f) { return 0u; }
        float3 nn = faceN[f] / len;
        float dd = -dot(nn, faceA[f]);
        if (dot(nn, sliceCentre) + dd > 0.0f) { nn = -nn; dd = -dd; }
        n[f] = nn;
        d[f] = dd;
    }

    uint count = 0u;
    // 1) Faces looking AWAY from the sun, stored inward. The sun-facing ones sweep off to
    //    infinity along the light and vanish from the boundary.
    [unroll] for (uint f2 = 0u; f2 < 6u; ++f2)
    {
        if (dot(n[f2], toSun) < 0.0f && count < 10u)
        {
            planes[count++] = float4(-n[f2], -d[f2] + dot(n[f2], origin));
        }
    }

    // 2) Silhouette edges (the two faces disagree about the sun), extruded along the light.
    //    (faceA, faceB, cornerA, cornerB) -- UE's AdjacentPlanePairs + LineVertexIndices,
    //    re-expressed for the corner layout above.
    const uint4 edges[12] = {
        uint4(0, 1, SDSM_nBL, SDSM_nTL), uint4(0, 2, SDSM_nBR, SDSM_nTR),
        uint4(0, 3, SDSM_nTL, SDSM_nTR), uint4(0, 4, SDSM_nBL, SDSM_nBR),
        uint4(5, 1, SDSM_fBL, SDSM_fTL), uint4(5, 2, SDSM_fBR, SDSM_fTR),
        uint4(5, 3, SDSM_fTL, SDSM_fTR), uint4(5, 4, SDSM_fBL, SDSM_fBR),
        uint4(1, 3, SDSM_nTL, SDSM_fTL), uint4(2, 3, SDSM_nTR, SDSM_fTR),
        uint4(1, 4, SDSM_nBL, SDSM_fBL), uint4(2, 4, SDSM_nBR, SDSM_fBR) };
    [unroll] for (uint e = 0u; e < 12u; ++e)
    {
        const uint4 ed = edges[e];
        if (dot(n[ed.x], toSun) * dot(n[ed.y], toSun) >= 0.0f) { continue; }

        // Anchor: the edge's own first corner, EXCEPT the side edges (8..11), which are anchored
        // at the apex -- they pass through it by definition, and anchoring them at a near corner
        // hands the far corner's distance to its own plane to the near corner's float error times
        // the lever arm (measured: 1.2 mm outside on cascade 3).
        const float3 a = (e >= 8u) ? float3(0.0f, 0.0f, 0.0f) : c[ed.z];
        float3 dir;
        if (e < 4u)       { dir = c[edges[e + 4u].w] - c[edges[e + 4u].z]; } // borrow the parallel far edge
        else if (e < 8u)  { dir = c[ed.w] - c[ed.z]; }
        else              { dir = c[ed.w]; }                                 // the corner ray
        const float dirLen = length(dir);
        if (dirLen < 1e-9f) { continue; }
        float3 pn = cross(dir, toSun);
        const float len = length(pn);
        // Ill-conditioned when the edge runs nearly along the light: the normal would be float
        // noise and could cut through the volume. Dropping a plane is always safe -- the volume
        // stays open along that edge, i.e. more casters, never fewer.
        if (len < 1e-4f * dirLen) { continue; }
        pn /= len;
        float pd = -dot(pn, a);
        if (dot(pn, sliceCentre) + pd < 0.0f) { pn = -pn; pd = -pd; }
        if (count < 10u) { planes[count++] = float4(pn, pd - dot(pn, origin)); }
    }
    return count;
}

// One partition's two matrix rows of CascadeHzbCB (render::CascadeHzb::GpuParams): prevViewProj[4]
// at float4 0..15, viewProj[4] at 16..31, then prevValid / viewRect / (size, on, pad).
void SdsmWriteCullCBMatrices(uint partition, float4x4 prevVp, float4x4 curVp)
{
    const float4x4 p = SdsmFlipZ(prevVp);
    const float4x4 c = SdsmFlipZ(curVp);
    const uint pb = partition * 4u;
    OutCullCB[pb + 0u] = float4(p._11, p._12, p._13, p._14);
    OutCullCB[pb + 1u] = float4(p._21, p._22, p._23, p._24);
    OutCullCB[pb + 2u] = float4(p._31, p._32, p._33, p._34);
    OutCullCB[pb + 3u] = float4(p._41, p._42, p._43, p._44);
    const uint cb = 16u + partition * 4u;
    OutCullCB[cb + 0u] = float4(c._11, c._12, c._13, c._14);
    OutCullCB[cb + 1u] = float4(c._21, c._22, c._23, c._24);
    OutCullCB[cb + 2u] = float4(c._31, c._32, c._33, c._34);
    OutCullCB[cb + 3u] = float4(c._41, c._42, c._43, c._44);
}

// ---------------------------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------------------------

// One depth pixel -> (world position, view-space Z). Returns false for a sample that is not
// geometry: the sample drop everything outside [near, far) for exactly this reason ("clear color,
// etc" -- CustomPartitions.hlsl:72). Under reverse-Z the sky is depth 0, which reconstructs to the
// far plane and fails the window without a special case.
bool SampleSurface(uint2 coord, out float3 posWS, out float viewZ)
{
    posWS = float3(0.0f, 0.0f, 0.0f);
    viewZ = 0.0f;
    if (coord.x >= gDepthWidth || coord.y >= gDepthHeight) { return false; }

    const float d = DepthTex.Load(int3(int2(coord), 0)).r;
    if (d <= 0.0f) { return false; }   // reverse-Z clear = sky

    const float2 uv = (float2(coord) + 0.5f) / float2(gDepthWidth, gDepthHeight);
    const float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 vpos = mul(float4(ndc, d, 1.0f), gInvProj);
    vpos.xyz /= max(1e-8f, vpos.w);
    posWS = mul(float4(vpos.xyz, 1.0f), gInvView).xyz;

    // The SAME view-Z the sampler will use to pick a partition (csm_sample's CsmChooseCascade):
    // the distance along the camera axis, not |P - camPos|. Two different definitions here and
    // there would put a pixel in a partition whose box was fitted without it.
    viewZ = dot(posWS - gCamPosWS.xyz, gCamDirWS.xyz);
    return (viewZ >= gNearZ && viewZ < gFarZ);
}

// ---------------------------------------------------------------------------------------------
// 1. ClearBounds -- LogPartitions.hlsl:71 + CustomPartitions.hlsl:38, merged into one dispatch
//    because both clear one element per partition and neither reads anything.
// ---------------------------------------------------------------------------------------------
[numthreads(SDSM_MAX_PARTITIONS, 1, 1)]
[RootSignature(SDSM_ANALYZE_RS)]
void ClearBounds(uint groupIndex : SV_GroupIndex)
{
    SdsmBoundsUint b;
    b.minCoord = uint3(SdsmFlippedPosInf(), SdsmFlippedPosInf(), SdsmFlippedPosInf());
    b.sampleCount = 0u;
    b.maxCoord = uint3(SdsmFlippedNegInf(), SdsmFlippedNegInf(), SdsmFlippedNegInf());
    b.pad = 0u;
    Bounds[groupIndex] = b;

    if (groupIndex == 0u)
    {
        SdsmZBounds z;
        z.minZ = SdsmFlippedPosInf();
        z.maxZ = SdsmFlippedNegInf();
        z.sampleCount = 0u;
        z.pad = 0u;
        ZBounds[0] = z;
    }
}

// ---------------------------------------------------------------------------------------------
// 2. ReduceZBounds -- LogPartitions.hlsl:79 ReduceZBoundsFromGBuffer.
//    Each group walks a gReduceTileDim square, keeps a per-thread min/max, reduces through LDS
//    and scatters ONE atomic pair per group. The sample's note applies unchanged: choosing a tile
//    size that "just" fills the machine keeps the scatter cheap.
// ---------------------------------------------------------------------------------------------
groupshared float sMinZ[SDSM_ZBOUNDS_BLOCK_SIZE];
groupshared float sMaxZ[SDSM_ZBOUNDS_BLOCK_SIZE];
groupshared uint  sZCount[SDSM_ZBOUNDS_BLOCK_SIZE];

[numthreads(SDSM_ZBOUNDS_BLOCK_DIM, SDSM_ZBOUNDS_BLOCK_DIM, 1)]
[RootSignature(SDSM_ANALYZE_RS)]
void ReduceZBounds(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID,
                   uint groupIndex : SV_GroupIndex)
{
    float minZ = gFarZ;
    float maxZ = gNearZ;
    uint  count = 0u;

    const uint2 tileStart = groupId.xy * gReduceTileDim.xx + groupThreadId.xy;
    for (uint tileY = 0u; tileY < gReduceTileDim; tileY += SDSM_ZBOUNDS_BLOCK_DIM)
    {
        for (uint tileX = 0u; tileX < gReduceTileDim; tileX += SDSM_ZBOUNDS_BLOCK_DIM)
        {
            float3 posWS;
            float  viewZ;
            if (SampleSurface(tileStart + uint2(tileX, tileY), posWS, viewZ))
            {
                minZ = min(minZ, viewZ);
                maxZ = max(maxZ, viewZ);
                ++count;
            }
        }
    }

    sMinZ[groupIndex] = minZ;
    sMaxZ[groupIndex] = maxZ;
    sZCount[groupIndex] = count;
    GroupMemoryBarrierWithGroupSync();

    for (uint offset = (SDSM_ZBOUNDS_BLOCK_SIZE >> 1); offset > 0u; offset >>= 1)
    {
        if (groupIndex < offset)
        {
            sMinZ[groupIndex] = min(sMinZ[groupIndex], sMinZ[offset + groupIndex]);
            sMaxZ[groupIndex] = max(sMaxZ[groupIndex], sMaxZ[offset + groupIndex]);
            sZCount[groupIndex] += sZCount[offset + groupIndex];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex == 0u && sZCount[0] > 0u)
    {
        // DEVIATION: through the order-preserving flip (see SdsmFloatFlip). View Z is positive
        // here, so the sample's raw asuint would work -- the flip is used anyway so that ONE
        // encoding covers both reductions and neither can be read with the wrong decoder.
        uint prev;
        InterlockedMin(ZBounds[0].minZ, SdsmFloatFlip(sMinZ[0]), prev);
        InterlockedMax(ZBounds[0].maxZ, SdsmFloatFlip(sMaxZ[0]), prev);
        InterlockedAdd(ZBounds[0].sampleCount, sZCount[0], prev);
    }
}

// ---------------------------------------------------------------------------------------------
// 3. LogPartitions -- LogPartitions.hlsl:132 ComputeLogPartitionsFromZBounds.
//    ALL the boundaries are logarithmic over the REDUCED range, ends included (S16.2). The sample
//    expand the first and last to the camera's own near/far and argue the expansion is free
//    because the BOXES come from the samples rather than from the partition frusta. It is free for
//    the boxes; it is not free for the CULL VOLUME and the slice-sphere ceiling, which Finalize
//    builds from the interval itself. See the block at the assignment for why nothing downstream
//    needs the padding.
// ---------------------------------------------------------------------------------------------
[numthreads(SDSM_MAX_PARTITIONS, 1, 1)]
[RootSignature(SDSM_ANALYZE_RS)]
void LogPartitions(uint groupIndex : SV_GroupIndex)
{
    const uint count = min(gPartitions, (uint)SDSM_MAX_PARTITIONS);
    const uint empty = ZBounds[0].sampleCount == 0u ? 1u : 0u;
    // Nothing visible at all (a frame that is pure sky): fall back to the camera range so the
    // partitions stay ordered and the sphere fallback in Finalize produces a valid, if useless,
    // box. Without this `minZ` is +inf and every interval collapses to NaN.
    const float rawMin = empty ? gNearZ : max(gNearZ, SdsmFloatUnflip(ZBounds[0].minZ));
    const float rawMax = empty ? gFarZ  : min(gFarZ,  SdsmFloatUnflip(ZBounds[0].maxZ));

    // S16.7 -- TEMPORAL SMOOTHING, APPLIED ONLY TO THE INTERIOR SPLIT POINTS.
    //
    // What actually rings, measured per frame on the reported viewpoint with a STATIC scene: it is
    // not minZ (15.23 m, rock steady) but maxZ, which walks 154 -> 166 -> 153 -> 154 m as ocean
    // waves open and close the furthest visible point. Every interior boundary is logarithmic over
    // [minZ, maxZ], so an 8 % swing at the far end moves all of them at once and the cascade edge
    // visibly jumps across the ground.
    //
    // The first attempt smoothed the RANGE and made expansion instant so the range stayed a
    // superset of the measurement. Safe, but useless here: the ringing IS an expansion every other
    // frame, so the instant side passed it straight through (measured 0.70 % of screen area per
    // frame against 0.64 % raw -- no better than nothing).
    //
    // So the ENDS STAY RAW and only the interior splits ride the smoothed range. Coverage and
    // culling cannot regress by construction: the cull volume and the sphere ceiling are built from
    // a partition's own interval ends, and partition 0's begin and the last one's end are still
    // exactly what the reduction measured. Contiguity survives because boundary i is computed once
    // and serves as both `intervalEnd[i-1]` and `intervalBegin[i]`.
    //
    // Freed of the superset obligation the average can be SYMMETRIC, which is what damps a
    // two-sided oscillation. In log space, because the split is logarithmic. Per FRAME, not per
    // second: the wind clock can be frozen and a dt-based decay stops converging when dt is 0.
    float splitMin = rawMin;
    float splitMax = rawMax;
    if (gStability > 0.0f && empty == 0u)
    {
        // Carried in partition 0's spare pair rather than read back off the interval ends -- those
        // are raw now, so they cannot continue an average.
        const float prevMin = PrevPartitions[0].smoothedRange.x;
        const float prevMax = PrevPartitions[0].smoothedRange.y;
        // A frame that never ran leaves zeroes, and log(0) is -inf. Positive and ordered is the
        // whole validity test: the buffer is cleared, not garbage.
        if (prevMin > 0.0f && prevMax > prevMin)
        {
            splitMin = exp(lerp(log(rawMin), log(prevMin), gStability));
            splitMax = exp(lerp(log(rawMax), log(prevMax), gStability));
            // The split divides by splitMin, so a denormal here would take every interval with it.
            splitMin = clamp(splitMin, gNearZ, gFarZ);
            splitMax = clamp(splitMax, splitMin * 1.0001f, gFarZ);
        }
    }
    // Partition 0 owns the state for the next frame, written by ITS thread only. Finalize must not
    // clear this field or the average would restart every frame.
    if (groupIndex == 0u)
    {
        Partitions[0].smoothedRange = float2(splitMin, splitMax);
    }

    if (groupIndex >= count) { return; }

    // S16.2 -- BOTH ENDS RIDE THE REDUCED RANGE NOW. They used to be pinned to the camera's own
    // near and to `gFarZ` (= the shadow distance), on the argument that widening an interval that
    // contains nothing is free because the BOXES come from the samples. That is true of the boxes
    // and of nothing else. `Finalize` builds this partition's CULL VOLUME and its slice-sphere
    // ceiling from `intervalBegin/intervalEnd`, so a last partition declared 35.85..1000 while the
    // depth buffer ends at ~120 m submits casters down an EIGHT TIMES longer prism than the
    // geometry occupies, and its empty-partition fallback box is the sphere of that whole prism.
    //
    // Nothing downstream needs the old padding, and that is the other half of this change rather
    // than a second edit: `CsmChooseCascade` counts how many of the three INTERIOR boundaries the
    // receiver is past (`splitsVS.yzw`), so it already returns the last partition for everything
    // beyond the last boundary -- the clamp is structural, not a comparison against `farSplit`.
    // `farSplit` itself only ever reaches the blend band's `sFar`, which the `idx < 3` guard drops
    // for the last partition. A receiver past maxZ (glass or water, which do not write the depth
    // the reduction reads) therefore still lands in the last partition and is still bounded by its
    // box test in `CsmSampleChain` -- exactly as before, because the BOX did not move: the samples
    // that define it are the same samples that define maxZ.
    Partitions[groupIndex].intervalBegin =
        (groupIndex == 0u) ? rawMin : SdsmLogPartitionFromRange(groupIndex, splitMin, splitMax, count);
    Partitions[groupIndex].intervalEnd =
        (groupIndex == (count - 1u)) ? rawMax : SdsmLogPartitionFromRange(groupIndex + 1u, splitMin, splitMax, count);
}

// ---------------------------------------------------------------------------------------------
// 4. ReduceBounds -- CustomPartitions.hlsl:45 ReduceBoundsFromGBuffer.
//    Per-thread bounds for EVERY partition (no atomics in the inner loop -- the thread owns its
//    copy, which is the sample's whole reason for the 16x8 block: PARTITIONS x blockSize float3
//    pairs have to fit in LDS), then a tree reduction over the partition-major array, then one
//    atomic per partition per group.
// ---------------------------------------------------------------------------------------------
#define SDSM_BOUNDS_SHARED (SDSM_MAX_PARTITIONS * SDSM_BOUNDS_BLOCK_SIZE)
groupshared float3 sBoundsMin[SDSM_BOUNDS_SHARED];
groupshared float3 sBoundsMax[SDSM_BOUNDS_SHARED];
groupshared uint   sBoundsCount[SDSM_BOUNDS_SHARED];

[numthreads(SDSM_BOUNDS_BLOCK_X, SDSM_BOUNDS_BLOCK_Y, 1)]
[RootSignature(SDSM_ANALYZE_RS)]
void ReduceBounds(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID,
                  uint groupIndex : SV_GroupIndex)
{
    float3 boundsMin[SDSM_MAX_PARTITIONS];
    float3 boundsMax[SDSM_MAX_PARTITIONS];
    uint   boundsCount[SDSM_MAX_PARTITIONS];
    [unroll] for (uint p = 0u; p < SDSM_MAX_PARTITIONS; ++p)
    {
        boundsMin[p] = float3(1.0e30f, 1.0e30f, 1.0e30f);
        boundsMax[p] = float3(-1.0e30f, -1.0e30f, -1.0e30f);
        boundsCount[p] = 0u;
    }

    const uint count = min(gPartitions, (uint)SDSM_MAX_PARTITIONS);
    const float nearZ = Partitions[0].intervalBegin;
    const float farZ = Partitions[count - 1u].intervalEnd;

    const uint2 tileStart = groupId.xy * gReduceTileDim.xx + groupThreadId.xy;
    for (uint tileY = 0u; tileY < gReduceTileDim; tileY += SDSM_BOUNDS_BLOCK_Y)
    {
        for (uint tileX = 0u; tileX < gReduceTileDim; tileX += SDSM_BOUNDS_BLOCK_X)
        {
            float3 posWS;
            float  viewZ;
            if (!SampleSurface(tileStart + uint2(tileX, tileY), posWS, viewZ)) { continue; }
            if (viewZ < nearZ || viewZ >= farZ) { continue; }

            uint part = 0u;
            [unroll] for (uint i = 0u; i < (SDSM_MAX_PARTITIONS - 1u); ++i)
            {
                [flatten] if (i < (count - 1u) && viewZ >= Partitions[i].intervalEnd) { ++part; }
            }

            // DEVIATION: light-space METRES from an orthonormal frame, not a normalized light
            // texcoord. The projection is built from these bounds afterwards, so there is no
            // global light matrix to normalise against -- and that is the point: the box is
            // anisotropic and a normalized coordinate would have thrown that away.
            const float3 ls = mul(float4(posWS, 1.0f), gLightView).xyz;
            boundsMin[part] = min(boundsMin[part], ls);
            boundsMax[part] = max(boundsMax[part], ls);
            ++boundsCount[part];
        }
    }

    [unroll] for (uint q = 0u; q < SDSM_MAX_PARTITIONS; ++q)
    {
        const uint index = groupIndex * SDSM_MAX_PARTITIONS + q;
        sBoundsMin[index] = boundsMin[q];
        sBoundsMax[index] = boundsMax[q];
        sBoundsCount[index] = boundsCount[q];
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint offset = (SDSM_BOUNDS_SHARED >> 1); offset >= SDSM_MAX_PARTITIONS; offset >>= 1)
    {
        for (uint i = groupIndex; i < offset; i += SDSM_BOUNDS_BLOCK_SIZE)
        {
            sBoundsMin[i] = min(sBoundsMin[i], sBoundsMin[offset + i]);
            sBoundsMax[i] = max(sBoundsMax[i], sBoundsMax[offset + i]);
            sBoundsCount[i] += sBoundsCount[offset + i];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex < count && sBoundsCount[groupIndex] > 0u)
    {
        uint prev;
        InterlockedMin(Bounds[groupIndex].minCoord.x, SdsmFloatFlip(sBoundsMin[groupIndex].x), prev);
        InterlockedMin(Bounds[groupIndex].minCoord.y, SdsmFloatFlip(sBoundsMin[groupIndex].y), prev);
        InterlockedMin(Bounds[groupIndex].minCoord.z, SdsmFloatFlip(sBoundsMin[groupIndex].z), prev);
        InterlockedMax(Bounds[groupIndex].maxCoord.x, SdsmFloatFlip(sBoundsMax[groupIndex].x), prev);
        InterlockedMax(Bounds[groupIndex].maxCoord.y, SdsmFloatFlip(sBoundsMax[groupIndex].y), prev);
        InterlockedMax(Bounds[groupIndex].maxCoord.z, SdsmFloatFlip(sBoundsMax[groupIndex].z), prev);
        InterlockedAdd(Bounds[groupIndex].sampleCount, sBoundsCount[groupIndex], prev);
    }
}

// ---------------------------------------------------------------------------------------------
// 5. Finalize -- SDSMPartitions.hlsl:141 ComputePartitionDataFromBounds, plus everything the
//    sample do on the CPU (their `ShadowVS` gets scale/bias in NDC; we hand the VS a matrix).
// ---------------------------------------------------------------------------------------------
[numthreads(SDSM_MAX_PARTITIONS, 1, 1)]
[RootSignature(SDSM_ANALYZE_RS)]
void Finalize(uint groupIndex : SV_GroupIndex)
{
    const uint count = min(gPartitions, (uint)SDSM_MAX_PARTITIONS);
    const uint p = groupIndex;

    // The CPU-known tail of CascadeHzbCB, written once. Thread 0 owns it because every other
    // thread writes only its own partition's rows.
    if (p == 0u)
    {
        OutCullCB[32u] = asfloat(gHzbPrevValid);
        OutCullCB[33u] = asfloat(uint4(asuint(gHzbViewRect.x), asuint(gHzbViewRect.y),
                                       asuint(gHzbViewRect.z), asuint(gHzbViewRect.w)));
        OutCullCB[34u] = asfloat(uint4(gHzbSize.x, gHzbSize.y, gHzbOn, 0u));
    }

    if (p >= count)
    {
        // An inactive slot must still reject every caster, or the cull would draw the whole scene
        // into a tile nothing samples. Same sentinel the CPU uses for an inactive view slot.
        OutFrustums[p * kViewPlanes] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        [unroll] for (uint k = 1u; k < kViewPlanes; ++k)
        {
            OutFrustums[p * kViewPlanes + k] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        // Identity matrices rather than stale ones: an inactive slot is never culled against, but
        // a NaN in a cbuffer row is a hazard for whoever reads the block next.
        SdsmWriteViewCB(p, (float4x4)0.0f, (float4)0.0f);
        SdsmWriteCullCBMatrices(p, (float4x4)0.0f, (float4x4)0.0f);
        return;
    }

    const float intervalBegin = Partitions[p].intervalBegin;
    const float intervalEnd = Partitions[p].intervalEnd;
    const uint  samples = Bounds[p].sampleCount;

    // The Legacy cascade of the same interval: the ceiling the measured box may not beat by more
    // than gMinScaleOverSphere, and the FALLBACK when this partition saw nothing at all.
    float3 sphereCentreWS;
    const float sphereRadius = SdsmSliceSphereRadius(intervalBegin, intervalEnd, sphereCentreWS);
    const float3 sphereLS = mul(float4(sphereCentreWS, 1.0f), gLightView).xyz;

    float3 bmin, bmax;
    uint flags = 0u;
    if (samples == 0u)
    {
        // Empty. The sample shrink such a partition to a degenerate region that nothing overlaps
        // (SDSMPartitions.hlsl:166) -- we take the Legacy sphere box instead, because our shadow
        // path still renders the tile and a receiver that appears mid-frame must land on
        // SOMETHING valid rather than on a 1-texel smear. Recorded in `flags` for the readout.
        bmin = sphereLS - float3(sphereRadius, sphereRadius, sphereRadius);
        bmax = sphereLS + float3(sphereRadius, sphereRadius, sphereRadius);
        flags |= SDSM_FLAG_EMPTY;
    }
    else
    {
        bmin = float3(SdsmFloatUnflip(Bounds[p].minCoord.x),
                      SdsmFloatUnflip(Bounds[p].minCoord.y),
                      SdsmFloatUnflip(Bounds[p].minCoord.z));
        bmax = float3(SdsmFloatUnflip(Bounds[p].maxCoord.x),
                      SdsmFloatUnflip(Bounds[p].maxCoord.y),
                      SdsmFloatUnflip(Bounds[p].maxCoord.z));
    }

    // `mDilationFactor` (SDSMPartitions.hlsl:157), as a fraction of the box's own extent. Covers
    // what a per-sample reduction cannot see: the caster whose shadow lands just past the last
    // visible sample, and the anisotropic reach of the filter kernel.
    {
        const float3 ext = max(bmax - bmin, float3(1e-3f, 1e-3f, 1e-3f));
        bmin -= ext * gDilation;
        bmax += ext * gDilation;
    }

    // The scale CEILING, expressed against the Legacy sphere (`mMaxScale`, SDSMPartitions.hlsl:163
    // -- "clamp scale but remain centered"). A partition holding a handful of pixels would
    // otherwise magnify without bound, and a single-pixel box makes the depth pass draw the whole
    // scene into a tile that covers a square metre.
    {
        const float minExtent = 2.0f * sphereRadius * gMinScaleOverSphere;
        const float3 centre = 0.5f * (bmin + bmax);
        const float3 halfExt = max(0.5f * (bmax - bmin), float3(0.5f * minExtent, 0.5f * minExtent, 0.5f * minExtent));
        if (any((bmax - bmin) < float3(minExtent, minExtent, minExtent))) { flags |= SDSM_FLAG_SCALE_CLAMPED; }
        bmin = centre - halfExt;
        bmax = centre + halfExt;
    }

    // ...and the FLOOR: never WIDER than the Legacy sphere box. The sphere encloses the whole
    // frustum slice by construction, so anything past it is a reduction that went wrong (a stray
    // sample, a NaN) rather than geometry, and letting it through would make SDSM quietly worse
    // than the mode it replaces.
    bmin.xy = max(bmin.xy, sphereLS.xy - float2(sphereRadius, sphereRadius));
    bmax.xy = min(bmax.xy, sphereLS.xy + float2(sphereRadius, sphereRadius));
    bmin.xy = min(bmin.xy, bmax.xy - float2(1e-2f, 1e-2f));

    // `mLightSpaceBorder` (SDSMPartitions.hlsl:150), in TEXELS of this partition's own tile. It
    // reserves room INSIDE the box for the filter kernel; the S5 atlas gutter is a separate ring
    // OUTSIDE the content rect and still exists. Solved rather than iterated: adding b texels on
    // each side of a box of extent E rendered into R texels gives E' = E * R / (R - 2b).
    {
        const float shrink = max(1.0f, gContentRes) / max(1.0f, gContentRes - 2.0f * gBorderTexels);
        const float3 centre = 0.5f * (bmin + bmax);
        const float3 halfExt = 0.5f * (bmax - bmin) * shrink;
        bmin = centre - halfExt;
        bmax = centre + halfExt;
    }

    // S7 pancaking, unchanged in meaning from the Legacy fit: the PROJECTION near plane hugs the
    // samples (that is the D16 range this step buys) and the pancake clamp in the depth VS presses
    // anything in front of it onto the plane instead of clipping it. The CULL box below keeps the
    // wide near so those casters are still submitted.
    const float nearProj = bmin.z - gPancakeSlack;
    const float farProj = bmax.z + gZMargin;
    const float nearCull = bmin.z - gCasterReach;

    const float4x4 proj = SdsmOrthoOffCenterLH(bmin.x, bmax.x, bmin.y, bmax.y, nearProj, farProj);
    const float4x4 viewProj = mul(gLightView, proj);

    const float2 texelWS = float2((bmax.x - bmin.x) / max(1.0f, gContentRes),
                                  (bmax.y - bmin.y) / max(1.0f, gContentRes));
    // The bias is quoted in texels, and the box is anisotropic, so "a texel" has to be the bigger
    // of the two -- the smaller one would under-bias along the wide axis, which is where the
    // acne would be.
    const float worstTexel = max(texelWS.x, texelWS.y);
    const float depthBias = (gDepthBiasTexels * worstTexel) / max(1e-4f, farProj - nearProj);

    // The atlas tile, identical in layout to Legacy: 2x2 grid, content rect inset by the gutter.
    const float tileOriginX = float((p % 2u) * gTileRes + gBorderRes);
    const float tileOriginY = float((p / 2u) * gTileRes + gBorderRes);
    const float atlasScale = gContentRes / max(1.0f, gAtlasRes);

    Partitions[p].depthBiasNDC = depthBias;
    Partitions[p].slopeBiasNDC = max(0.0f, depthBias * gSlopeScale);
    Partitions[p].boundsMin = float4(bmin, gMaxSlope);
    Partitions[p].boundsMax = float4(bmax, gClampNear);
    Partitions[p].lightViewProj = viewProj;
    Partitions[p].atlasScaleBias = float4(atlasScale, atlasScale,
                                          tileOriginX / max(1.0f, gAtlasRes),
                                          tileOriginY / max(1.0f, gAtlasRes));
    Partitions[p].texelWS = texelWS;
    Partitions[p].sampleCount = samples;
    Partitions[p].flags = flags;
    // S16: the warp exponents, clamped on the CPU, stored here so the converter and the sampler
    // read ONE value rather than two copies of a knob.
    Partitions[p].evsmExponents = float2(gEvsmPos, gEvsmNeg);

    // The two GPU-written constant blocks. The depth pass binds the first by address (b1) and the
    // caster cull the second (b1) -- neither knows SDSM exists.
    //
    // S16: with EVSM on the DEPTH-PASS BIAS IS ZEROED. The sample carry none, and they are right
    // to: a Chebyshev bound already has a minimum-variance floor that plays the part a constant
    // depth push plays for a binary compare, and stacking the two means the moments describe a
    // surface that is not where the geometry is -- peter-panning that no filter can take back.
    const float4 depthPassBias = (gEvsmOn != 0u)
        ? float4(0.0f, 0.0f, 0.0f, gClampNear)
        : float4(depthBias, max(0.0f, depthBias * gSlopeScale), gMaxSlope, gClampNear);
    SdsmWriteViewCB(p, viewProj, depthPassBias);
    SdsmWriteCullCBMatrices(p, PrevPartitions[p].lightViewProj, viewProj);

    // ---- the cull box, as six world-space inward planes -------------------------------------
    // gLightView maps a world ROW vector: pl.x = dot(p, column0) + gLightView._41, so the light
    // frame's world axes are the COLUMNS of the matrix. "pl.x >= bmin.x" is then the plane
    // (axisX, offsetX - bmin.x) and "pl.x <= bmax.x" its negation. Six planes; the remaining ten
    // slots carry the accept-all sentinel the cull's fixed 16-iteration loop expects.
    //
    // The Z pair uses nearCull, NOT nearProj: shrinking the cull near would delete exactly the
    // casters pancaking exists to save (the S7 trap, Scene.cpp's note).
    {
        const float3 axisX = float3(gLightView._11, gLightView._21, gLightView._31);
        const float3 axisY = float3(gLightView._12, gLightView._22, gLightView._32);
        const float3 axisZ = float3(gLightView._13, gLightView._23, gLightView._33);
        const float3 offs = float3(gLightView._41, gLightView._42, gLightView._43);
        const uint base = p * kViewPlanes;

        float4 box[6];
        box[0] = float4( axisX,  offs.x - bmin.x);
        box[1] = float4(-axisX, -offs.x + bmax.x);
        box[2] = float4( axisY,  offs.y - bmin.y);
        box[3] = float4(-axisY, -offs.y + bmax.y);
        box[4] = float4( axisZ,  offs.z - nearCull);
        box[5] = float4(-axisZ, -offs.z + farProj);

        // S15.4: the accurate volume = the camera slice extruded toward the sun, INTERSECTED with
        // the box. Both, not either: the box's four XY faces are geometrically implied by the
        // prism but NOT by the positive-vertex AABB test that consumes these planes -- that test
        // over-includes at the prism's acute corners, and Legacy measured 17 casters passing where
        // the box passed 13 without them. With both, the volume can never pass a box the plain box
        // rejects, which is exactly what the Legacy cross-check asserts on.
        //
        // The slice is extended toward the camera by the PREVIOUS partition's cross-fade band: a
        // receiver in that band samples THIS partition too (S10), so the casters over it have to
        // survive. Partition 0 has no previous one.
        uint written = 0u;
        if (gAccurateCull != 0u)
        {
            const float3 toSun = -axisZ; // axisZ is the direction the light TRAVELS
            const float bandPrev = (p > 0u)
                ? (Partitions[p - 1u].intervalEnd - Partitions[p - 1u].intervalBegin) * gBlendFraction
                : 0.0f;
            const float sliceNear = max(gNearZ, intervalBegin - bandPrev);
            float4 vol[10];
            const uint volCount = SdsmCullVolumePlanes(sliceNear, intervalEnd, toSun, vol);
            // 10 + 6 would not fit the 16 the cull loops; a degenerate slice returns 0. Either way
            // the box alone is the fallback, and it is always correct -- just looser.
            if (volCount > 0u && (volCount + 6u) <= kViewPlanes)
            {
                // SELF-CHECK, the GPU twin of the Debug asserts Legacy runs on this same
                // construction (Scene.cpp: "drops a slice corner" / "closed toward the sun"). A
                // cull volume that is too TIGHT deletes casters, and a deleted caster is a missing
                // shadow -- which reads as a content bug, not a cull bug, unless something says so.
                // The tolerance scales with the coordinates for the reason the CPU version spells
                // out: a plane test is float products of world coordinates, ~30 ulps at 500 m.
                float3 probe[8];
                SdsmSliceCorners(sliceNear, intervalEnd, probe);
                bool leak = false;
                [unroll] for (uint ci = 0u; ci < 8u; ++ci)
                {
                    const float tol = 1e-3f + 2e-6f * length(probe[ci] - gCamPosWS.xyz);
                    [unroll] for (uint pi = 0u; pi < 10u; ++pi)
                    {
                        if (pi >= volCount) { continue; }
                        if (dot(vol[pi].xyz, probe[ci]) + vol[pi].w < -tol) { leak = true; }
                    }
                }
                if (leak) { flags |= SDSM_FLAG_VOLUME_LEAK; }

                [unroll] for (uint i = 0u; i < 10u; ++i)
                {
                    if (i < volCount) { OutFrustums[base + written++] = vol[i]; }
                }
            }
        }
        [unroll] for (uint b = 0u; b < 6u; ++b) { OutFrustums[base + written++] = box[b]; }
        // The cull's loop is a fixed literal 16, so the tail carries the accept-all plane.
        [unroll] for (uint k = 0u; k < kViewPlanes; ++k)
        {
            if (k >= written) { OutFrustums[base + k] = float4(0.0f, 0.0f, 0.0f, 1.0f); }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// 6. CarryFrustums -- copy the non-directional view slots (spot, point faces, clipmap) from the
//    CPU-uploaded ring into the SDSM frustum buffer verbatim. A separate entry point rather than
//    part of Finalize because it is a different thread mapping (one per plane, not one per
//    partition), and because a copy loop inside a 4-thread group would serialise 42 views onto
//    four lanes.
// ---------------------------------------------------------------------------------------------
[numthreads(64, 1, 1)]
[RootSignature(SDSM_ANALYZE_RS)]
void CarryFrustums(uint3 dtid : SV_DispatchThreadID)
{
    // One thread per (view, plane) of the slots PAST the partitions. The source is indexed by
    // VIEW (stride 256) and the destination by PLANE (stride 16) -- two views of one layout, and
    // the reason the two descriptors are built differently in SdsmShadows::EnsureDescriptors.
    // Starts at the LITERAL partition cap, not at the active count: slots between the two are
    // Finalize's reject-all sentinels, and copying the CPU ring over them would resurrect a
    // directional view the analysis deliberately switched off. The CPU dispatches exactly this
    // many groups, from the same constant.
    const uint i = dtid.x;
    const uint view = (uint)SDSM_MAX_PARTITIONS + (i / kViewPlanes);
    const uint plane = i % kViewPlanes;
    if (view >= gViewFrustumCount) { return; }
    OutFrustums[view * kViewPlanes + plane] = SrcFrustums[view].planes[plane];
}
