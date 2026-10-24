#pragma pack_matrix(row_major)

#ifndef UTILS_HLSLI
#define UTILS_HLSLI

// ================== constants ==================
static const float kEpsilon = 1e-6f;
static const float kPi = 3.14159265359f;
static const float kInvPi = 1.0f / kPi;
static const float kMinRoughness = 0.02f; // UE's SUBSTRATE_MIN_GGX_ROUGHNESS (SubstrateDefinitions.h:52)
static const float kMinAlpha = kMinRoughness * kMinRoughness;
static const float3 kF0Dielectric = float3(0.04f, 0.04f, 0.04f); // IOR ~1.5 for dielectrics

// GBAux.b carries a four-bit shading-model contract in an R8 UNORM channel. Values 0..15 are
// encoded exactly at n/15; the remaining channel precision is intentionally unused for now.
// Keep these IDs in sync with the C++ ShadingModel enum in MaterialData.h.
static const uint kShadingModelDefaultLit = 0u;
static const uint kShadingModelTwoSidedFoliage = 1u;
static const uint kShadingModelTerrain = 2u;
// The ocean surface, written by its own forward draw AFTER compose (UE's SingleLayerWater), never by a
// material: lighting and compose never see it. docs/ocean_gbuffer_ssr_unification_plan.md S1.
static const uint kShadingModelWater = 3u;
static const uint kShadingModelMask = 15u;

inline float EncodeShadingModel(uint id)
{
	return float(id & kShadingModelMask) / float(kShadingModelMask);
}

inline uint DecodeShadingModel(float encoded)
{
	return (uint)round(saturate(encoded) * float(kShadingModelMask));
}

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
inline float4 TransformPositionH(float3 p, float4x4 world, float4x4 viewProj)
{
        return mul(mul(float4(p, 1.0f), world), viewProj);
}

inline float3 TransformDirectionWS(float3 n, float3x3 world3x3)
{
	return mul(n, world3x3);
}

inline float2 UVtoNDC(float2 uv)
{
        return uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
}

inline float2 NDCToUV(float2 ndc)
{
        return ndc * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
}

inline float2 ClipToUV(float4 clip)
{
        float w = clip.w;
        //float clampedW = (w >= 0.0f) ? max(kEpsilon, w) : min(-kEpsilon, w);
        float2 ndc = clip.xy * rcp(w);
        return NDCToUV(ndc);
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
	// UE's form (BRDF.ush D_GGX) has no floor: d >= a2 > 0 here (a >= kMinAlpha). An ABSOLUTE floor
	// of kEpsilon (1e-6) on pi*d^2 capped the peak of every alpha below ~0.024 -- a chrome glint at a
	// third of its value and a smooth surface under a point light at 0.8 instead of ~4e5 (2026-10-15).
	return a2 / max(kPi * d * d, 1.0e-30f);
}

// UE's SPHERE-LIGHT specular for a light of angular radius `sinAlpha` (BRDF.ush SphereMaxNoH, after
// de Carpentier 2017 "Decima Engine: Advances in Lighting and AA", with the Newton step): N.H and V.H
// taken at the light's point that maximises N.H, so inside the reflected disc the lobe's peak is flat.
// Inputs are the plain dots N.L, N.V, V.L; NoH/VoH are replaced.
inline void SphereMaxNoH(float NoL, float NoV, float VoL, float sinAlpha, inout float NoH, inout float VoH)
{
	if (sinAlpha <= 0.0f) { return; }
	const float cosAlpha = sqrt(1.0f - sinAlpha * sinAlpha);
	const float RoL = 2.0f * NoL * NoV - VoL;
	if (RoL >= cosAlpha)
	{
		NoH = 1.0f;
		VoH = abs(NoV);
		return;
	}
	const float rInvLengthT = sinAlpha * rsqrt(max(1.0f - RoL * RoL, 1.0e-12f));
	float NoTr = rInvLengthT * (NoV - RoL * NoL);
	float VoTr = rInvLengthT * (2.0f * NoV * NoV - 1.0f - RoL * VoL);
	// dot(cross(N, L), V)
	const float NxLoV = sqrt(saturate(1.0f - NoL * NoL - NoV * NoV - VoL * VoL + 2.0f * NoL * NoV * VoL));
	const float NoBr = rInvLengthT * NxLoV;
	const float VoBr = rInvLengthT * NxLoV * 2.0f * NoV;
	const float NoLVTr = NoL * cosAlpha + NoV + NoTr;
	const float VoLVTr = VoL * cosAlpha + 1.0f + VoTr;
	const float p = NoBr * VoLVTr;
	const float q = NoLVTr * VoLVTr;
	const float s = VoBr * NoLVTr;
	const float xNum = q * (-0.5f * p + 0.25f * VoBr * NoLVTr);
	const float xDenom = p * p + s * (s - 2.0f * p) +
		NoLVTr * ((NoL * cosAlpha + NoV) * VoLVTr * VoLVTr + q * (-0.5f * (VoLVTr + VoL * cosAlpha) - 0.5f));
	const float twoX1 = 2.0f * xNum / max(xDenom * xDenom + xNum * xNum, 1.0e-30f);
	const float sinTheta = twoX1 * xDenom;
	const float cosTheta = 1.0f - twoX1 * xNum;
	NoTr = cosTheta * NoTr + sinTheta * NoBr;
	VoTr = cosTheta * VoTr + sinTheta * VoBr;
	const float sNoL = NoL * cosAlpha + NoTr;
	const float sVoL = VoL * cosAlpha + VoTr;
	const float invLenH = rsqrt(max(2.0f + 2.0f * sVoL, 1.0e-12f));
	NoH = saturate((sNoL + NoV) * invLenH);
	VoH = saturate(invLenH + invLenH * sVoL);
}

// UE's energy normalisation of that widened lobe (ShadingModels.ush New_a2 / EnergyNormalization):
// D is evaluated with the surface's own a2 at the maximised N.H and scaled by a2 / Sphere_a2, so the
// reflected disc comes out at the light's own radiance x F, neither brighter nor dimmer.
inline float SphereEnergyNormalization(float a2, float VoH, float sinAlpha)
{
	if (sinAlpha <= 0.0f) { return 1.0f; }
	const float sphereA2 = a2 + 0.25f * sinAlpha * (3.0f * sqrt(a2) + sinAlpha) / (VoH + 0.001f);
	return a2 / sphereA2;
}
// UE's visibility term, G / (4 N.L N.V) in one: the exact joint Smith (BRDF.ush Vis_SmithJoint, Heitz
// 2014) -- what a Substrate slab uses at r.Substrate.ShadingQuality 1 (SubstrateEvaluation.ush
// Substrate_Vis_GGX), the owner's UE 5.8 project's setting; DefaultLit and quality 0 take its linear
// approximation, Vis_SmithJointApprox. Takes a2. It REPLACED Schlick-GGX with k = (a+1)^2 / 8
// (2026-10-15), Karis 2013's remap for analytic lights, which keeps k >= 1/8 however smooth the surface:
// a near-mirror lost ~17 % at 60 degrees and ~3x at grazing (the sea's glints with a low sun).
inline float Vis_SmithJoint(float a2, float NdotV, float NdotL)
{
	const float visV = NdotL * sqrt(NdotV * (NdotV - NdotV * a2) + a2);
	const float visL = NdotV * sqrt(NdotL * (NdotL - NdotL * a2) + a2);
	return 0.5f / max(visV + visL, 1.0e-12f);
}
inline float3 F_Schlick(float cosT, float3 F0)
{
	float m = pow(1.0f - cosT, 5.0f);
	return F0 + (1.0f - F0) * m;
}

// ===== Unified BRDF =====
// P16.5 -- the shared local-light distance falloff, in the same photometric units as the sun.
//
// `intensity` at the call sites is CANDELA, so the product is the illuminance reaching the surface,
// in lux -- the same unit `sunIlluminanceLux` is in, which is the entire point of having it here
// rather than seven times over.
//
// The shape is Unreal's `saturate(1 - (d/r)^4)^2 / (d^2 + 1)`:
//   * inverse-square in the middle, which is what makes the unit mean anything;
//   * a window that reaches exactly ZERO at the range, so a light does not clip off while still
//     bright the way a hard `d > range` cut does;
//   * a bias in the denominator so d -> 0 is finite ("technically just 1/d2 but this avoids inf",
//     DeferredLightingCommon.ush:482). UE's 1 is in ITS units, cm: 1 cm^2 = 1e-4 m^2 here. Transcribed
//     as 1 m^2 it dimmed every lamp near its source -- 2x at 1 m, 5x at 0.5 m, 17x at 0.25 m (owner,
//     2026-10-24: the candles had no bright pool on the wall behind them).
//
// It REPLACED `(1 - d/r)^2`, which was a window pretending to be a falloff -- no unit, no
// inverse-square, and a brightness that moved whenever someone dragged the radius.
static const float kLightDistanceBiasSqr = 1.0e-4f; // UE's +1 cm^2, in m^2
float LightDistanceAttenuation(float dist, float range)
{
    const float invRange = (range > 1e-6f) ? (1.0f / range) : 0.0f;
    const float t = saturate(dist * dist * invRange * invRange); // (d/r)^2
    const float w = saturate(1.0f - t * t);                      // 1 - (d/r)^4
    return (w * w) / (dist * dist + kLightDistanceBiasSqr);
}

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

// lightAngularSize: the analytic light's angular RADIUS in radians (0 = punctual). A finite light
// is UE's sphere light (2026-10-15, replacing an alpha widening without renormalisation that put a
// mirror's glint at a quarter of the disc's radiance and twice its size): SphereSinAlpha =
// sin(radius) * (1 - alpha) (CapsuleLightIntegrate.ush:98), N.H and V.H at the disc's best point
// (SphereMaxNoH) and D scaled by a2 / Sphere_a2 (EnergyNormalization) -- a near-mirror shows the
// reflected disc at its own radiance x F, sample-able because it has the disc's size. Vis takes UE's
// N.V = saturate(|N.V| + 1e-5) (ShadingModels.ush:252): a normal-mapped texel turned away from the eye
// gets a finite term rather than a 1/0, the way a 0 * G used to hide it.
//
// lightSoftRadius: UE's SoftSourceRadius, sin(half of LightSourceSoftAngle) (DirectionalLight). It
// widens the surface's a2 BEFORE the sphere light acts, and D and Vis both take the widened one
// (ShadingModels.ush EnergyNormalization, `inout a2`).
//
// The whole term is a Substrate slab as the owner's UE 5.8 project renders one (2026-10-15, read off its
// cvars): r.Substrate 1 with the blendable GBuffer, so the energy conservation is COMPILED OUT
// (ShadingEnergyConservation.ush:20-30) -- diffuse is plain Lambert of the diffuse colour, no (1 - F)
// (SubstrateEvaluation.ush:565, its transmission weight is 1); r.Substrate.ShadingQuality 1 -> the
// exact Vis_SmithJoint; F = F_Schlick(F0, F90) with F90 = saturate(50 max3(F0)), the micro-occlusion
// that only touches an F0 under 2 %; roughness floored at SUBSTRATE_MIN_GGX_ROUGHNESS 0.02.
inline BRDFResult EvalBRDF(BRDFInput bi, float lightAngularSize = 0.0f, float lightSoftRadius = 0.0f)
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
	const float F90 = saturate(50.0f * max(F0.r, max(F0.g, F0.b))); // ShadingCommon.ush F0RGBToMicroOcclusion
	float a = max(kMinAlpha, bi.rough * bi.rough);
	float a2 = a * a;

	// The specular's N.H / V.H: at the light's best point when it has a size (UE sphere light).
	float specNdotH = NdotH;
	float specVdotH = VdotH;
	float sphereEnergy = 1.0f;
	if (lightAngularSize > 0.0f || lightSoftRadius > 0.0f)
	{
		const float sinAlpha = saturate(sin(lightAngularSize) * (1.0f - a));
		SphereMaxNoH(dot(bi.N, bi.L), dot(bi.N, bi.V), dot(bi.V, bi.L), sinAlpha, specNdotH, specVdotH);
		if (lightSoftRadius > 0.0f)
		{
			a2 = saturate(a2 + lightSoftRadius * lightSoftRadius / (specVdotH * 3.6f + 0.4f));
		}
		sphereEnergy = SphereEnergyNormalization(a2, specVdotH, sinAlpha);
	}

	const float Fc = pow(1.0f - specVdotH, 5.0f);
	float3 F = F90 * Fc + (1.0f - Fc) * F0;
	float D = D_GGX(specNdotH, sqrt(a2)) * sphereEnergy;
	float Vis = Vis_SmithJoint(a2, saturate(abs(dot(bi.N, bi.V)) + 1.0e-5f), o.NdotL);

	o.diffBRDF = (1.0f - bi.metal) * bi.albedo * kInvPi;
	o.specBRDF = D * Vis * F;

	return o;
}

// ===== Two-sided thin-foliage direct lighting =====
// Adds a view-opposed transmission lobe on top of the standard front-side GGX/Lambert
// response so a thin leaf viewed against a light glows with a tinted subsurface color — matching the
// intent of Unreal's legacy "Two Sided Foliage" model (a thin-sheet wrap/transmission
// approximation, not volumetric subsurface scattering).
//
// N is the VISIBLE-side normal (already SV_IsFrontFace-corrected when the GBuffer is written),
// so front lighting uses dot(N,L) exactly like DefaultLit. Transmission treats both botanical
// sides identically through abs(N.L); view/light opposition makes a leaf glow when it is viewed
// against the source instead of tinting foliage from every camera direction.
static const float kFoliageTransmissionViewPower = 2.0f;
static const float kFoliageTransmissionNormalWeight = 0.35f;

struct FoliageResult
{
	float3 diffBRDF;  // front Lambert BRDF; caller multiplies by NdotL * radiance
	float3 specBRDF;  // front GGX BRDF;     caller multiplies by NdotL * radiance
	float3 transBRDF; // two-sided transmission weight (Fresnel + wrap + payload); caller multiplies by radiance only
	float  NdotL;     // front cosine, weights diffBRDF + specBRDF
};

// subsurfacePayload = GB2.rgb = subsurfaceColor * transmissionStrength * albedoTransmission.
// lightAngularSize / lightSoftRadius: the light's size and soft radius (see EvalBRDF). normalWeight
// blends broad thin-sheet wrap with projected-area weighting without assigning a privileged side to the leaf.
inline FoliageResult EvalFoliageBRDF(BRDFInput bi, float3 subsurfacePayload,
	float lightAngularSize = 0.0f,
	float normalWeight = kFoliageTransmissionNormalWeight,
	float lightSoftRadius = 0.0f)
{
	FoliageResult o;

	// Front side is byte-for-byte the DefaultLit response (Lambert + GGX). This keeps a
	// front-lit leaf indistinguishable from DefaultLit and lets local lights / RT hits reuse it.
	BRDFResult br = EvalBRDF(bi, lightAngularSize, lightSoftRadius);
	o.diffBRDF = br.diffBRDF;
	o.specBRDF = br.specBRDF;
	o.NdotL = br.NdotL;

	// The sheet is two-sided: abs(N.L) measures projected area without privileging either
	// botanical side. A material weight keeps some broad wrap while retaining angle variation.
	float sheetNdotL = saturate(abs(dot(bi.N, bi.L)));
	float incidence = lerp(1.0f, sheetNdotL, saturate(normalWeight));

	// Account for reflection at both interfaces. For a dielectric leaf this retains about 92%
	// at normal incidence and smoothly suppresses impossible grazing-angle transmission.
	float sheetNdotV = saturate(abs(dot(bi.N, bi.V)));
	float3 FIn = F_Schlick(sheetNdotL, kF0Dielectric);
	float3 FOut = F_Schlick(sheetNdotV, kF0Dielectric);
	float3 interfaceTransmission = (1.0f - FIn) * (1.0f - FOut) * (1.0f - bi.metal);

	// The view gate peaks when the camera and light are on opposite sides of the leaf. Squaring
	// it gives a broad but directional forward-scattering lobe without an all-around glow.
	float viewLightOpposition = saturate(dot(bi.V, -bi.L));
	float viewScatter = pow(viewLightOpposition, kFoliageTransmissionViewPower);
	float transmit = incidence * viewScatter;
	o.transBRDF = subsurfacePayload * interfaceTransmission * transmit * kInvPi;

	return o;
}

#endif // UTILS_HLSLI
