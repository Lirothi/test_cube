#pragma pack_matrix(row_major)

#ifndef UTILS_HLSL
#define UTILS_HLSL

// ================== constants ==================
static const float kEpsilon = 1e-6;

// ============ normalize helpers ============
inline float3 NormalizeSafe(float3 v, float3 fallback)
{
    float l2 = dot(v, v);
    if (l2 > kEpsilon)
    {
        return v * rsqrt(l2);
    }
    else
    {
        return fallback;
    }
}

// =============== transforms (row-vector) ===============
inline float4 TransformPositionH(float3 p, float4x4 world, float4x4 view, float4x4 proj)
{
    return mul(mul(mul(float4(p, 1.0), world), view), proj);
}

inline float3 TransformDirectionWS(float3 n, float3x3 world3x3)
{
    return mul(n, world3x3);
}

// =============== normal remap [-1..1] <-> [0..1] ===============
inline float3 NrmTo01(float3 n)
{
    return n * 0.5 + 0.5;
}
inline float3 NrmFrom01(float3 n01)
{
    return n01 * 2.0 - 1.0;
}

// =============== pack/unpack Roughness+Metallic в A8_UNORM ===============
// по умолчанию 5/3 бита (rough/metal). меняется одной парой констант.
static const uint kRM_RBits = 5u; // roughness
static const uint kRM_MBits = 3u; // metallic
static const uint kRM_MaxU8 = 255u;
static const uint kRM_MMask = (1u << kRM_MBits) - 1u;
static const float kRM_RScale = float((1u << kRM_RBits) - 1u);
static const float kRM_MScale = float((1u << kRM_MBits) - 1u);

// [0..1]x[0..1] -> A8_UNORM
inline float PackRM(float rough, float metal)
{
    uint r = (uint) round(saturate(rough) * kRM_RScale);
    uint m = (uint) round(saturate(metal) * kRM_MScale);
    uint packed = (r << kRM_MBits) | m; // [rrrrr][mmm]
    return float(packed) / float(kRM_MaxU8);
}

// A8_UNORM -> (rough, metal) в [0..1]
inline float2 UnpackRM(float a8)
{
    uint v = (uint) round(saturate(a8) * float(kRM_MaxU8));
    uint m = v & kRM_MMask;
    uint r = (v >> kRM_MBits);
    return float2(float(r) / kRM_RScale, float(m) / kRM_MScale);
}

#endif // UTILS_HLSL