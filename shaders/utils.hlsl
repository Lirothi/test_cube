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

// --- Derivative/cotangent frame (без TBN) ---
inline float3 PerturbNormal_Deriv(float3 nTS, float3 Nws, float3 Pvs, float2 uv)
{
    //float3 N0 = Nws; //normalize(Nws);

    //// БЕРЁМ FINE-деривативы — устойчиво на границах примитива
    //float3 dpdx = ddx_fine(Pws);
    //float3 dpdy = ddy_fine(Pws);
    //float2 duvdx = ddx_fine(uv);
    //float2 duvdy = ddy_fine(uv);

    //// детерминант параметризации; адаптивный порог
    //float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    //float eps = 1e-12;
    //if (abs(det) < eps)
    //{
    //    return N0; // плохо обусловлено — вернём базовую нормаль
    //}

    //float3 r1 = cross(dpdy, N0);
    //float3 r2 = cross(N0, dpdx);

    //float3 T = r1 * duvdx.x + r2 * duvdy.x;
    //float3 B = r1 * duvdx.y + r2 * duvdy.y;

    //return normalize(T * nTS.x + B * nTS.y + N0 * nTS.z);
    
    
    // Базовая нормаль (world)
    float3 N = Nws;

    // Fine-деривативы (устойчиво у края треугольников)
    float3 dp1 = ddx_fine(Pvs);
    float3 dp2 = ddy_fine(Pvs);
    float2 du1 = ddx_fine(uv);
    float2 du2 = ddy_fine(uv);

    // Cotangent frame (Mikk's trick) без деления на det
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * du1.x + dp1perp * du2.x;
    float3 B = dp2perp * du1.y + dp1perp * du2.y;

    // Баланс масштаба T/B → сила нормалки независима от масштаба UV/проекции
    float len2 = max(dot(T, T), dot(B, B));
    if (len2 < 1e-18)
    {
        return N; // вырожденная параметризация
    }
    float invMax = rsqrt(len2);
    T *= invMax;
    B *= invMax;

    // Применяем tangent-space нормаль
    return normalize(T * nTS.x + B * nTS.y + N * nTS.z);
}

#endif // UTILS_HLSL