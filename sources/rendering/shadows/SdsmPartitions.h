#pragma once

#include <cstdint>
#include <DirectXMath.h>

#include "rendering/shadows/ShadowSettings.h" // render::sdsm::kMaxPartitions

// ---------------------------------------------------------------------------------------------
// SDSM partition contract (docs/csm_improvement_plan.md S15). THE MIRROR of
// shaders/sdsm_partitions.hlsli -- every struct here has a byte-identical twin there, and the
// static_asserts below are the only thing that catches a member added on one side only.
//
// Transcribed from the Intel Sample Distribution Shadow Maps DX11 sample
// (D:\Programming\sdsm\sdsm_dx11, Intel Sample Source Code License -- permissive with notice):
// Partitions.h `Partition`, SDSMPartitions.hlsl `ComputePartitionDataFromBounds`,
// CustomPartitions.hlsl `ReduceBoundsFromGBuffer`, LogPartitions.hlsl `ReduceZBoundsFromGBuffer`
// / `LogPartitionFromRange` / `ComputeLogPartitionsFromZBounds`.
//
// OUR DEVIATIONS FROM THE SAMPLE, all deliberate:
//  * MATRICES, not NDC scale/bias. The sample keep one global light projection and store a
//    per-partition zoom (scale/bias in NDC) that both the shadow VS and the lighting PS apply.
//    We store the finished world->clip matrix per partition instead, because our consumers
//    (csm_sample.hlsli, shadow_indirect_csm.hlsl) already speak matrices and a second convention
//    would be a second place for the two to disagree.
//  * BOUNDS IN METRES, not in [0,1] light texcoords. Same reason: there is no global light
//    projection here to normalise against. The consequence is that the reduction's atomics cannot
//    use the sample's raw `asuint` trick (it is monotonic only for non-negative floats) -- see
//    SdsmFloatFlip in the HLSL header.
//  * ONE DEPTH TEXTURE, not a G-buffer decode: we reduce the camera depth buffer directly
//    (reverse-Z) and reconstruct world position, so a sample costs one fetch.
//  * A CEILING, not just a floor. The sample clamp the zoom with `mMaxScale` to stop a
//    near-empty partition magnifying without bound; ours is expressed against the LEGACY
//    bounding sphere of the same interval (render::sdsm::g_minScaleOverSphere), which doubles
//    as the fallback for a partition with zero samples.
// ---------------------------------------------------------------------------------------------

namespace render::sdsm
{

// One partition: what the analysis produced and what every consumer reads. ONE PRODUCER
// (Main_SdsmAnalyze's finalize dispatch); consumers are the cull (via the frustum buffer it also
// writes), the shadow depth VS, csm_sample.hlsli and the dev readout.
struct alignas(16) Partition
{
    // --- 0 --- the slice of VIEW-SPACE Z this partition owns. `intervalBegin` of partition 0 is
    // the camera near plane and `intervalEnd` of the last is the camera far plane, so the set
    // covers the whole range BY CONSTRUCTION -- exactly the sample's expansion of the first and
    // last (LogPartitions.hlsl:139-143). The interior boundaries are logarithmic over the
    // REDUCED [minZ, maxZ], which is where the "near being tightened" win comes from.
    float intervalBegin = 0.0f;
    float intervalEnd = 0.0f;
    // The depth-pass bias, already in NDC, computed from THIS partition's world texel and depth
    // range (the Legacy twin is CascadeData::depthBiasNDC). It has to be computed here because
    // both inputs are results of the reduction.
    float depthBiasNDC = 0.0f;
    float slopeBiasNDC = 0.0f; // depthBiasNDC * CascadeShadowConfig::slopeScale

    // --- 16 --- the light-space AABB of this partition's samples, in METRES, after the filter
    // border, the dilation and the scale clamp. xyz = the box; w carries the two remaining
    // depth-pass parameters so the VS needs no second constant buffer.
    DirectX::XMFLOAT4 boundsMin{};  // w = maxSlope (tangent cap on the slope-scaled bias)
    DirectX::XMFLOAT4 boundsMax{};  // w = clampNear (S7 pancaking flag: 1 = clamp, 0 = clip)

    // --- 48 --- world -> partition clip. lightView * OrthoOffCenterLH(box, nearProj, far).
    DirectX::XMFLOAT4X4 lightViewProj{};

    // --- 112 --- where the partition lives in the atlas: xy = scale, zw = bias, mapping the
    // partition's [0,1] local UV onto its CONTENT rect (the S5 gutter is already excluded).
    DirectX::XMFLOAT4 atlasScaleBias{};

    // --- 128 --- world metres per texel, PER AXIS. The box is anisotropic (that is half the win:
    // a beach vista is far wider than it is tall in light space), so one number cannot describe it
    // and every texel-derived quantity downstream -- the filter ramp, the normal offset, the
    // gutter inset -- has to pick the axis it means.
    float texelWS[2] = { 0.0f, 0.0f };
    // Samples this partition's reduction saw. 0 = nothing visible fell in the interval, and the
    // box below is then the Legacy bounding-sphere fallback, not a measurement.
    std::uint32_t sampleCount = 0u;
    // bit 0: the scale clamp fired (the box was widened to the sphere ceiling).
    // bit 1: the partition is empty (sampleCount == 0) and got the sphere fallback.
    std::uint32_t flags = 0u;
};
static_assert(sizeof(Partition) == 144, "sdsm::Partition must match SdsmPartition in shaders/sdsm_partitions.hlsli");

// Flags of Partition::flags.
inline constexpr std::uint32_t kPartitionFlagScaleClamped = 1u << 0;
inline constexpr std::uint32_t kPartitionFlagEmpty        = 1u << 1;
// The accurate cull volume dropped one of its own slice corners: too tight, casters deleted.
inline constexpr std::uint32_t kPartitionFlagVolumeLeak   = 1u << 2;

// The reduction scratch: ONE element, written by the Z-bounds pass and consumed by the log-split
// pass. Separate from the partition buffer rather than aliased over it (the sample alias a
// `PartitionUint` view over `gPartitions`), because our partition struct has no pair of adjacent
// uints at the right offset and an alias that is "almost" right is a silent corruption.
struct alignas(16) ZBoundsScratch
{
    std::uint32_t minZ = 0u;   // SdsmFloatFlip(view-space Z)
    std::uint32_t maxZ = 0u;
    std::uint32_t sampleCount = 0u;
    std::uint32_t pad = 0u;
};
static_assert(sizeof(ZBoundsScratch) == 16, "sdsm::ZBoundsScratch must match SdsmZBounds in shaders/sdsm_partitions.hlsli");

// Per-partition light-space bounds accumulator, in SdsmFloatFlip space so InterlockedMin/Max
// order correctly across negative coordinates. CustomPartitions.hlsl `BoundsUint`, with the flip.
struct alignas(16) BoundsUint
{
    std::uint32_t minCoord[3] = { 0u, 0u, 0u };
    std::uint32_t sampleCount = 0u;
    std::uint32_t maxCoord[3] = { 0u, 0u, 0u };
    std::uint32_t pad = 0u;
};
static_assert(sizeof(BoundsUint) == 32, "sdsm::BoundsUint must match SdsmBoundsUint in shaders/sdsm_partitions.hlsli");

// The constant buffer every analyze dispatch binds at b0. MIRRORS `SdsmAnalyzeCB` in
// shaders/sdsm_partitions.hlsli; uploaded by raw memcpy, so the offsets have to agree.
struct alignas(16) AnalyzeConstants
{
    DirectX::XMFLOAT4X4 invProj{};       // camera clip -> view (non-jittered: see the fit note in S1)
    DirectX::XMFLOAT4X4 invView{};       // camera view -> world
    DirectX::XMFLOAT4X4 lightView{};     // world -> light space (orthonormal, camera-anchored)

    DirectX::XMFLOAT4 camPosWS{};        // xyz; w = tan(hfovX/2)
    DirectX::XMFLOAT4 camDirWS{};        // xyz normalized; w = tan(vfovY/2)

    float nearZ = 0.0f;                  // camera near/far in VIEW space -- the reduction's accept window
    float farZ = 0.0f;
    std::uint32_t partitions = 4u;       // active partitions; <= kMaxPartitions
    std::uint32_t depthWidth = 0u;

    std::uint32_t depthHeight = 0u;
    std::uint32_t reduceTileDim = 0u;    // `mReduceTileDim`: pixels per thread-group side
    float borderTexels = 0.0f;
    float dilation = 0.0f;

    float minScaleOverSphere = 0.0f;     // the ceiling, as a fraction of the Legacy sphere box
    float zMargin = 0.0f;
    float pancakeSlack = 0.0f;           // S7: how far the PROJECTION near plane sits ahead of the samples
    float casterReach = 0.0f;            // S7: how far toward the sun the CULL box stays open

    float depthBiasTexels = 0.0f;        // CascadeShadowConfig::depthBiasInTexels
    float slopeScale = 0.0f;             // CascadeShadowConfig::slopeScale
    float maxSlope = 0.0f;               // CascadeShadowConfig::maxSlope
    float clampNear = 0.0f;              // CascadeShadowConfig::pancakeCasters ? 1 : 0

    float atlasRes = 0.0f;               // the whole CSM atlas edge, in texels
    float contentRes = 0.0f;             // one tile's edge MINUS twice the S5 gutter
    std::uint32_t tileRes = 0u;
    std::uint32_t borderRes = 0u;        // the S5 gutter, in texels

    std::uint32_t viewFrustumCount = 0u; // how many slots of the frustum buffer to carry over
    // S15.4: the cull volume needs the SAME slice extension Legacy gives it -- a receiver in the
    // previous partition's cross-fade band samples THIS partition too (S10), so the casters over
    // it must survive this cull. CascadeShadowConfig::blendFraction.
    float blendFraction = 0.0f;
    // 1 = the accurate volume (the camera slice extruded toward the sun, S14); 0 = the plain
    // light-space box. The A/B, one flag inside one binary -- CascadeShadowConfig::accurateCasterCull.
    std::uint32_t accurateCull = 0u;
    std::uint32_t pad2 = 0u;

    // ---- the wind tail of PerViewCB, passed through so the analyze can write a COMPLETE
    // per-partition constant block. It is the gbuffer's wind, verbatim: a shadow swaying to
    // different numbers than its own tree detaches from it (W5), so this travels rather than
    // being re-derived.
    float windTime = 0.0f;
    float windPrevTime = 0.0f;
    float windDirX = 1.0f;
    float windDirZ = 0.0f;
    float windSwayAmp = 0.0f;
    float windSwayFreq = 0.0f;
    float windGustMul = 1.0f;
    float windPrevGustMul = 1.0f;

    // ---- the parts of CascadeHzb::GpuParams only the CPU knows. The MATRICES are GPU results
    // (this frame's partitions and last frame's), so the whole 560-byte block is assembled by the
    // analyze; these four rows are what it cannot derive.
    std::uint32_t hzbPrevValid[4] = { 0u, 0u, 0u, 0u }; // pyramid c holds LAST frame's tile
    std::int32_t  hzbViewRect[4] = { 0, 0, 0, 0 };      // (0, 0, content/2, content/2)
    std::uint32_t hzbSize[2] = { 0u, 0u };              // pyramid mip 0
    std::uint32_t hzbOn = 0u;                           // 0 = no occlusion test this frame
    std::uint32_t hzbPad = 0u;
};
static_assert(sizeof(AnalyzeConstants) == 400,
              "sdsm::AnalyzeConstants must stay byte-identical to SdsmAnalyzeCB in "
              "shaders/sdsm_partitions.hlsli -- it is uploaded by raw memcpy");

// `mReduceTileDim` of the sample, fixed here rather than derived per frame: the group counts are
// what keeps the machine full, and at 1920x1080 a 32-pixel tile is 60x34 groups, which is already
// past the point where more groups stop helping. The Z reduction runs 16x16 threads over it and
// the bounds reduction 16x8 (CustomPartitions.hlsl's shape, so PARTITIONS bounds fit in LDS).
// One per-partition PerViewCB block, and the size of the whole CascadeHzbCB block, both written
// by the analyze into DEFAULT-heap buffers whose ADDRESSES the CPU then binds as root CBVs. This
// is the one trick the whole mode leans on: a root CBV takes a GPU virtual address, so "the GPU
// computed this constant buffer" needs no shader permutation anywhere downstream -- not in the
// shadow depth VS, not in the caster cull, not in the post cull.
inline constexpr std::uint32_t kViewCbStride = 256u;   // == scene_internal::kSdsmViewCbStride
inline constexpr std::uint32_t kCullCbBytes = 560u;    // == sizeof(render::CascadeHzb::GpuParams)

inline constexpr std::uint32_t kReduceTileDim = 32u;
inline constexpr std::uint32_t kZBoundsBlockDim = 16u;   // REDUCE_ZBOUNDS_BLOCK_DIM
inline constexpr std::uint32_t kBoundsBlockX = 16u;      // REDUCE_BOUNDS_BLOCK_X
inline constexpr std::uint32_t kBoundsBlockY = 8u;       // REDUCE_BOUNDS_BLOCK_Y

} // namespace render::sdsm
