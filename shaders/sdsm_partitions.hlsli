// SDSM partition contract (docs/csm_improvement_plan.md S15) -- the HLSL half of the mirror pair
// with sources/rendering/shadows/SdsmPartitions.h. Every struct below is byte-identical to its
// twin there; the C++ side carries the static_asserts.
//
// Transcribed from the Intel Sample Distribution Shadow Maps DX11 sample
// (D:\Programming\sdsm\sdsm_dx11, Intel Sample Source Code License -- permissive with notice):
//   LogPartitions.hlsl      ReduceZBoundsFromGBuffer / LogPartitionFromRange /
//                           ComputeLogPartitionsFromZBounds
//   CustomPartitions.hlsl   ReduceBoundsFromGBuffer
//   SDSMPartitions.hlsl     ComputePartitionDataFromBounds
// Our deviations are listed at the head of the C++ header and marked "DEVIATION" below.
#ifndef SDSM_PARTITIONS_HLSLI
#define SDSM_PARTITIONS_HLSLI

// Mirrors render::sdsm::kMaxPartitions. A LITERAL, never a constant-buffer value: it is the bound
// of every [unroll]ed partition loop and of the groupshared arrays, and a loop bound read from a
// CB is exactly the shape that produces silent GPU undefined behaviour.
#define SDSM_MAX_PARTITIONS 4

// `mReduceTileDim` and the two block shapes (render::sdsm::kReduceTileDim / kZBoundsBlockDim /
// kBoundsBlockX / kBoundsBlockY). Literals for the same reason.
#define SDSM_REDUCE_TILE_DIM 32
#define SDSM_ZBOUNDS_BLOCK_DIM 16
#define SDSM_ZBOUNDS_BLOCK_SIZE (SDSM_ZBOUNDS_BLOCK_DIM * SDSM_ZBOUNDS_BLOCK_DIM)
#define SDSM_BOUNDS_BLOCK_X 16
#define SDSM_BOUNDS_BLOCK_Y 8
#define SDSM_BOUNDS_BLOCK_SIZE (SDSM_BOUNDS_BLOCK_X * SDSM_BOUNDS_BLOCK_Y)

struct SdsmPartition
{
    float  intervalBegin;     // 0   view-space Z
    float  intervalEnd;       // 4
    float  depthBiasNDC;      // 8
    float  slopeBiasNDC;      // 12
    float4 boundsMin;         // 16  xyz light-space metres; w = maxSlope
    float4 boundsMax;         // 32  xyz light-space metres; w = clampNear
    float4x4 lightViewProj;   // 48  world -> partition clip
    float4 atlasScaleBias;    // 112 xy scale, zw bias (CONTENT rect)
    float2 texelWS;           // 128 world metres per texel, per axis
    uint   sampleCount;       // 136
    uint   flags;             // 140
};                            // 144

#define SDSM_FLAG_SCALE_CLAMPED 1u
#define SDSM_FLAG_EMPTY         2u
// S15.4: the accurate cull volume dropped one of its OWN slice corners -- the volume is too
// tight, which deletes casters and reads as a missing shadow. Must be 0.
#define SDSM_FLAG_VOLUME_LEAK   4u

struct SdsmZBounds
{
    uint minZ;          // SdsmFloatFlip(view Z)
    uint maxZ;
    uint sampleCount;
    uint pad;
};

// render::ShadowViewFrustum -- one cull view's 16 inward planes, 256 bytes. Declared here because
// the analyze pass READS ShadowGpuData's own frustum ring through it, and a StructuredBuffer's
// element stride must match the SRV's StructureByteStride EXACTLY: reading that buffer as a flat
// float4 array (stride 16 against the descriptor's 256) is GBV id=1387, and the access it
// generates is 16x out of bounds.
struct SdsmViewFrustum
{
    float4 planes[16];
};

struct SdsmBoundsUint
{
    uint3 minCoord;     // SdsmFloatFlip of light-space metres
    uint  sampleCount;
    uint3 maxCoord;
    uint  pad;
};

// CONSUMERS include this header for the STRUCT ALONE -- the shadow depth VS, the lighting/fog/glass
// samplers. They must not also inherit a `cbuffer ... : register(b0)` that their own root signature
// never declares, so define SDSM_PARTITIONS_STRUCTS_ONLY before including and everything below that
// touches the analyze constants disappears.
#ifndef SDSM_PARTITIONS_STRUCTS_ONLY

// Mirrors render::sdsm::AnalyzeConstants (uploaded by raw memcpy -- offsets must agree).
cbuffer SdsmAnalyzeCB : register(b0)
{
    row_major float4x4 gInvProj;
    row_major float4x4 gInvView;
    row_major float4x4 gLightView;

    float4 gCamPosWS;      // xyz; w = tan(hfovX/2)
    float4 gCamDirWS;      // xyz; w = tan(vfovY/2)

    float  gNearZ;
    float  gFarZ;
    uint   gPartitions;
    uint   gDepthWidth;

    uint   gDepthHeight;
    uint   gReduceTileDim;
    float  gBorderTexels;
    float  gDilation;

    float  gMinScaleOverSphere;
    float  gZMargin;
    float  gPancakeSlack;
    float  gCasterReach;

    float  gDepthBiasTexels;
    float  gSlopeScale;
    float  gMaxSlope;
    float  gClampNear;

    float  gAtlasRes;
    float  gContentRes;
    uint   gTileRes;
    uint   gBorderRes;

    uint   gViewFrustumCount;
    float  gBlendFraction;
    uint   gAccurateCull;
    uint   gSdsmPad2;

    // The gbuffer's wind, passed through so Finalize can write a COMPLETE PerView block.
    float  gWindTime;
    float  gWindPrevTime;
    float  gWindDirX;
    float  gWindDirZ;
    float  gWindSwayAmp;
    float  gWindSwayFreq;
    float  gWindGustMul;
    float  gWindPrevGustMul;

    // The CPU-known rows of CascadeHzbCB (the matrices are GPU results -- see SdsmWriteCullCB).
    uint4  gHzbPrevValid;
    int4   gHzbViewRect;
    uint2  gHzbSize;
    uint   gHzbOn;
    uint   gHzbPad;
};

// Row-vector z flip, CascadeHzb.cpp's `FlipZ` transcribed: clip.z' = w - z. The atlas is
// forward-Z and hzb_cull.hlsli is reverse-Z, so the pyramid stores 1 - z and the matrices the cull
// tests boxes with must carry the same flip. `R = M * FlipZ` touches only column 2:
// R[i][2] = M[i][3] - M[i][2].
float4x4 SdsmFlipZ(float4x4 vp)
{
    float4x4 r = vp;
    r._13 = vp._14 - vp._13;
    r._23 = vp._24 - vp._23;
    r._33 = vp._34 - vp._33;
    r._43 = vp._44 - vp._43;
    return r;
}

#endif // SDSM_PARTITIONS_STRUCTS_ONLY

// DEVIATION from the sample. Theirs reduce light TEXCOORDS in [0,1], where `asuint` happens to be
// a monotonic map and a raw InterlockedMin/Max on it is correct. Ours are light-space METRES and
// routinely negative, where `asuint` is NOT monotonic (the sign bit dominates and the magnitude
// then orders backwards). This is the standard order-preserving flip used by float radix sorts:
// a non-negative float gets its sign bit set, a negative one is fully inverted. Both directions
// are exact -- it is a bijection on the bit pattern, not a quantisation.
uint SdsmFloatFlip(float f)
{
    const uint i = asuint(f);
    const uint mask = (i >> 31u) ? 0xFFFFFFFFu : 0x80000000u;
    return i ^ mask;
}

float SdsmFloatUnflip(uint u)
{
    const uint mask = (u >> 31u) ? 0x80000000u : 0xFFFFFFFFu;
    return asfloat(u ^ mask);
}

// The empty accumulator: min starts at +inf, max at -inf, both flipped.
uint SdsmFlippedPosInf() { return SdsmFloatFlip(1.0e30f); }
uint SdsmFlippedNegInf() { return SdsmFloatFlip(-1.0e30f); }

// LogPartitions.hlsl:24 `LogPartitionFromRange`, verbatim. `partition` in [0, PARTITIONS].
float SdsmLogPartitionFromRange(uint partition, float minZ, float maxZ, uint count)
{
    float z = maxZ;   // exclusive on this end
    if (partition < count)
    {
        const float ratio = maxZ / max(1e-4f, minZ);
        const float power = float(partition) * (1.0f / float(count));
        z = minZ * pow(ratio, power);
    }
    return z;
}

#ifndef SDSM_PARTITIONS_STRUCTS_ONLY

// S1's ComputeCascadeSphere (Scene.cpp:117), transcribed -- the minimal enclosing sphere of the
// camera frustum slice [splitNear, splitFar]. It is what a LEGACY cascade of the same interval
// would have covered, and here it serves two purposes at once: the CEILING the partition box may
// never be tighter than (the sample's `mMaxScale`, expressed against something meaningful instead
// of a magic constant) and the FALLBACK box when a partition saw no samples at all.
//
// Centre depends only on (splitNear, splitFar, FOV) and lies on the view axis; the closed form is
// the same equal-distance solve, clamped onto the far plane when the slice is wide relative to
// its length.
float SdsmSliceSphereRadius(float splitNear, float splitFar, out float3 centreWS)
{
    const float tanHalfX = gCamPosWS.w;
    const float tanHalfY = gCamDirWS.w;

    const float farX = tanHalfX * splitFar;
    const float farY = tanHalfY * splitFar;
    const float nearX = tanHalfX * splitNear;
    const float nearY = tanHalfY * splitNear;

    const float diagFarSq = farX * farX + farY * farY;
    const float diagNearSq = nearX * nearX + nearY * nearY;
    const float sliceLen = max(1e-4f, splitFar - splitNear);

    const float offset = (diagNearSq - diagFarSq) / (2.0f * sliceLen) + sliceLen * 0.5f;
    const float centreZ = clamp(splitFar - offset, splitNear, splitFar);
    centreWS = gCamPosWS.xyz + gCamDirWS.xyz * centreZ;

    // The far rectangle's corner is the farthest point of the slice from a centre on the axis
    // once the centre is at or before the far plane, which the clamp guarantees.
    const float dz = splitFar - centreZ;
    const float rFar = sqrt(diagFarSq + dz * dz);
    const float dzn = centreZ - splitNear;
    const float rNear = sqrt(diagNearSq + dzn * dzn);
    return max(1.0f, max(rFar, rNear));
}

#endif // SDSM_PARTITIONS_STRUCTS_ONLY

// XMMatrixOrthographicOffCenterLH, row-major / row-vector (the convention mat4 uses throughout).
float4x4 SdsmOrthoOffCenterLH(float l, float r, float b, float t, float zn, float zf)
{
    const float rw = 1.0f / max(1e-6f, r - l);
    const float rh = 1.0f / max(1e-6f, t - b);
    const float rz = 1.0f / max(1e-6f, zf - zn);
    return float4x4(
        2.0f * rw,        0.0f,             0.0f,      0.0f,
        0.0f,             2.0f * rh,        0.0f,      0.0f,
        0.0f,             0.0f,             rz,        0.0f,
        -(l + r) * rw,    -(t + b) * rh,    -zn * rz,  1.0f);
}

#endif // SDSM_PARTITIONS_HLSLI
