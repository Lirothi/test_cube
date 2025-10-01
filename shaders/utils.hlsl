#pragma pack_matrix(row_major)

#ifndef UTILS_HLSL
#define UTILS_HLSL

// ================== constants ==================
static const float kEpsilon = 1e-6f;
static const float kPi = 3.14159265359f;
static const float kInvPi = 1.0f / kPi;
static const float kMinRoughness = 0.03f;
static const float kMinAlpha = kMinRoughness * kMinRoughness;
static const float3 kF0Dielectric = float3(0.04f, 0.04f, 0.04f); // IOR ~1.5 for dielectrics

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
	return mul(mul(mul(float4(p, 1.0f), world), view), proj);
}

inline float3 TransformDirectionWS(float3 n, float3x3 world3x3)
{
	return mul(n, world3x3);
}

inline float2 UVtoNDC(float2 uv)
{
	return uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
}

inline float3 ReconstructPosWS(float2 uv, float depth, float4x4 invProj, float4x4 invView)
{
	const float2 ndc = UVtoNDC(uv);
	float4 clip = float4(ndc, depth, 1.0f);
	float4 vpos = mul(clip, invProj); // → view
	vpos.xyz /= max(kEpsilon, vpos.w);
	return mul(float4(vpos.xyz, 1.0f), invView).xyz; // → world
}

// =============== normal remap [-1..1] <-> [0..1] ===============
inline float3 NrmTo01(float3 n)
{
	return n * 0.5f + 0.5f;
}
inline float3 NrmFrom01(float3 n01)
{
	return n01 * 2.0f - 1.0f;
}

// =============== pack/unpack Roughness+Metallic into A8_UNORM ===============
static const uint kRM_RBits = 5u; // roughness
static const uint kRM_MBits = 3u; // metallic
static const uint kRM_MaxU8 = 255u;
static const uint kRM_MMask = (1u << kRM_MBits) - 1u;
static const float kRM_RScale = (float) ((1u << kRM_RBits) - 1u);
static const float kRM_MScale = (float) ((1u << kRM_MBits) - 1u);

// [0..1]x[0..1] -> A8_UNORM
inline float PackRM(float rough, float metal)
{
	uint r = (uint) round(saturate(rough) * kRM_RScale);
	uint m = (uint) round(saturate(metal) * kRM_MScale);
	uint packed = (r << kRM_MBits) | m; // [rrrrr][mmm]
	return (float) packed / (float) kRM_MaxU8;
}

// A8_UNORM -> (rough, metal) in [0..1]
inline float2 UnpackRM(float a8)
{
	uint v = (uint) round(saturate(a8) * (float) kRM_MaxU8);
	uint m = v & kRM_MMask;
	uint r = (v >> kRM_MBits);
	return float2((float) r / kRM_RScale, (float) m / kRM_MScale);
}

// --- Derivative/cotangent frame (without TBN) ---
inline float3 PerturbNormal_Deriv(float3 nTS, float3 Nws, float3 Pvs, float2 uv)
{
    // Base normal (world)
	float3 N = Nws;

    // Fine derivatives (stable near triangle edges)
	float3 dp1 = ddx_fine(Pvs);
	float3 dp2 = ddy_fine(Pvs);
	float2 du1 = ddx_fine(uv);
	float2 du2 = ddy_fine(uv);

    // Cotangent frame (Mikk's trick) without dividing by det
	float3 dp2perp = cross(dp2, N);
	float3 dp1perp = cross(N, dp1);
	float3 T = dp2perp * du1.x + dp1perp * du2.x;
	float3 B = dp2perp * du1.y + dp1perp * du2.y;

    // Balance the scale of T/B so normal strength is independent of UV/projection scale
	float len2 = max(dot(T, T), dot(B, B));
	if (len2 < 1e-18f)
	{
            return N; // Degenerate parameterization
	}
	float invMax = rsqrt(len2);
	T *= invMax;
	B *= invMax;

    // Apply tangent-space normal
	return normalize(T * nTS.x + B * nTS.y + N * nTS.z);
}

// sRGB <-> Linear (precise piecewise functions, alpha untouched)
// Formula source: IEC 61966-2-1
static const float SRGB_EPS_INV = 0.04045f; // sRGB -> Linear threshold
static const float SRGB_EPS_FWD = 0.0031308f; // Linear -> sRGB threshold
static const float SRGB_A = 0.055f;
static const float SRGB_GAMMA = 2.4f;
static const float SRGB_IGAMMA = 1.0f / 2.4f;

inline float3 SRGBToLinear(float3 c)
{
	float3 low = c / 12.92f;
	float3 high = pow((c + SRGB_A) / (1.0f + SRGB_A), SRGB_GAMMA);
	return lerp(low, high, step(SRGB_EPS_INV, c));
}

inline float4 SRGBToLinear(float4 c)
{
	return float4(SRGBToLinear(c.rgb), c.a);
}

inline float3 LinearToSRGB(float3 c)
{
    c = max(c, 0.0f); // clamp negatives that may appear after tonemapping
	float3 low = c * 12.92f;
	float3 high = (1.0f + SRGB_A) * pow(c, SRGB_IGAMMA) - SRGB_A;
	return lerp(low, high, step(SRGB_EPS_FWD, c));
}

inline float4 LinearToSRGB(float4 c)
{
	return float4(LinearToSRGB(c.rgb), c.a);
}

// Fast approximations if needed: pow-based, but not piecewise accurate
inline float3 SRGBToLinear_Fast(float3 c)
{
	return pow(saturate(c), 2.2f);
}
inline float3 LinearToSRGB_Fast(float3 c)
{
	return pow(max(c, 0.0f), 1.0f / 2.2f);
}

// Arbitrary “gamma”: linear <-> gamma encode (generic pow, not sRGB)
inline float3 LinearToGamma(float3 c, float gammaOut)
{
	return pow(max(c, 0.0f), 1.0f / max(1e-6f, gammaOut));
}
inline float3 GammaToLinear(float3 c, float gammaIn)
{
	return pow(saturate(c), max(1e-6f, gammaIn));
}
inline float4 LinearToGamma(float4 c, float g)
{
	return float4(LinearToGamma(c.rgb, g), c.a);
}
inline float4 GammaToLinear(float4 c, float g)
{
	return float4(GammaToLinear(c.rgb, g), c.a);
}

// GGX + Schlick
inline float D_GGX(float NdotH, float a)
{
	float a2 = a * a;
	float d = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
	return a2 / max(kEpsilon, kPi * d * d);
}
inline float G_SchlickGGX(float NdotX, float k)
{
	return NdotX / max(kEpsilon, NdotX * (1.0f - k) + k);
}
inline float3 F_Schlick(float cosT, float3 F0)
{
	float m = pow(1.0f - cosT, 5.0f);
	return F0 + (1.0f - F0) * m;
}

// ===== Unified BRDF =====
struct BRDFInput
{
	float3 albedo;
	float rough;
	float metal;
	float3 N;
	float3 V;
	float3 L;
};

struct BRDFResult
{
    float3 diffBRDF; // Lambert with energy compensation
	float3 specBRDF; // GGX Cook-Torrance
	float NdotL;
	float NdotV;
};

inline BRDFResult EvalBRDF(BRDFInput bi)
{
	BRDFResult o;
	o.NdotL = saturate(dot(bi.N, bi.L));
	o.NdotV = saturate(dot(bi.N, bi.V));
	o.diffBRDF = 0.0f.xxx;
	o.specBRDF = 0.0f.xxx;

	float3 H = normalize(bi.L + bi.V);
	float NdotH = saturate(dot(bi.N, H));
	float VdotH = saturate(dot(bi.V, H));

	float3 F0 = lerp(kF0Dielectric, bi.albedo, bi.metal);
	float a = max(kMinAlpha, bi.rough * bi.rough);
	float kv = (a + 1.0f) * (a + 1.0f) * 0.125f; // (a+1)^2 / 8

	float3 F = F_Schlick(VdotH, F0);
	float D = D_GGX(NdotH, a);
	float G = G_SchlickGGX(o.NdotV, kv) * G_SchlickGGX(o.NdotL, kv);

	float3 kd = (1.0f - F) * (1.0f - bi.metal);
	o.diffBRDF = kd * bi.albedo * kInvPi;
	o.specBRDF = (D * G * F) / max(kEpsilon, 4.0f * o.NdotL * o.NdotV);

	return o;
}

#endif // UTILS_HLSL
