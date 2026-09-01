#define POINTLIGHT_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=10, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE))"
// t0..t3 : GBuffer (GB0, GB1, GB2, GBVelocity)
// t4     : Depth
// t5     : StructuredBuffer<PointLightData>
// t6     : TextureCubeArray point shadow depth cube (D16 -> R16), B2b
// t7     : StructuredBuffer<uint> VSM page table (Rung 2 / Step 21)
// t8     : Texture2D VSM physical page pool depth
// t9     : GBAux (AO, indirect specular scale, shading model)
// u0     : Light accumulation RWTexture2D
// s0     : LinearClamp
// s1     : PointClamp
// s2     : ComparisonLinearClamp (shadow compare)

#pragma pack_matrix(row_major)

#include "utils.hlsli"
#include "vsm_sample.hlsli"
#include "contact_shadow.hlsli"

struct PointLightData
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
    float4 shadowParams; // x = shadow slot (-1 = none), y = bias, z = nearPlane, w = farPlane (radius)
};

Texture2D GB0 : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D GBVelocity : register(t3);
Texture2D DepthT : register(t4);
StructuredBuffer<PointLightData> PointLights : register(t5);
TextureCubeArray PointShadowCube : register(t6);
StructuredBuffer<uint> VsmPageTable : register(t7); // Rung 2 / Step 21
Texture2D VsmPool : register(t8);
Texture2D GBAux : register(t9);
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpLinear : register(s0);
SamplerState gSmpPoint : register(s1);
SamplerComparisonState gSmpShadowCmp : register(s2);

cbuffer PointLightFrame : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float3 camPosWS;
    uint lightCount;
    float2 screenSize;
    float2 invScreenSize;
    float invPointShadowSize; // 1 / pointShadowRes (cube face texel, for PCF)
    uint   useVsm;      // Rung 2 / Step 21: sample the VSM page pool instead of the cube atlas
    float  vsmRefDist;  // VSM mip level-select reference distance
    // VSM local-light shadow bias in shadow-texel units (live-tunable, vsm::g_local*Texels) — same
    // model as the spot path. Lateral normal offset + slope-scaled along-ray depth push.
    float  localLateralTexels;
    float  localDepthPushTexels;
    float  _vsmPad0;
    float  _vsmPad1;
    float  _vsmPad2;
    // Contact shadows (docs/csm_improvement_plan.md S12). Same names in every light pass.
    float4x4 viewProj;           // camera world -> clip
    float4x4 projMatrix;         // camera view -> clip; the contact ray's compare tolerance
    float contactShadowLength;
    float contactShadowIntensity;
    uint  contactShadowSteps;
    uint  contactShadowLengthInWS;
    float contactShadowNormalOffset;
    float contactShadowGrazingFade;
    float contactShadowMinDist;
    float contactShadowMaxDist;
    float contactShadowFadeBand;
    float contactShadowThickness;
    uint  contactShadowFrameId;
    // LOCAL LIGHTS ONLY. 0 = shadow map, contacts off here. 1 = contacts INSTEAD of the shadow
    // map (the map is not even sampled). 2 = contacts only where the light has no shadow slot.
    // Stacking both on a small-range light darkens the same contact twice and buys nothing.
    uint  contactShadowLocalMode;
}
ContactShadowParams MakeContactParams()
{
    ContactShadowParams cp;
    cp.length = contactShadowLength;
    cp.intensity = contactShadowIntensity;
    cp.steps = contactShadowSteps;
    cp.lengthInWS = contactShadowLengthInWS;
    cp.normalOffset = contactShadowNormalOffset;
    cp.grazingFade = contactShadowGrazingFade;
    cp.minDist = contactShadowMinDist;
    cp.maxDist = contactShadowMaxDist;
    cp.fadeBand = contactShadowFadeBand;
    cp.thickness = contactShadowThickness;
    cp.frameId = contactShadowFrameId;
    return cp;
}

// Which of the two shadow sources this light uses, by contactShadowLocalMode (see the CB).
void LocalShadowSources(bool hasSlot, out bool useMap, out bool useContact)
{
    useMap = true; useContact = false;
    if (contactShadowLocalMode == 1u)      { useMap = false;   useContact = true; }
    else if (contactShadowLocalMode == 2u) { useMap = hasSlot; useContact = !hasSlot; }
}


// Omnidirectional (cube) point shadow, depth-cube approach (B3). Reconstructs the
// standard-projection compare depth from the world offset (matches the render:
// PerspectiveFovLH(90,1,near,far), LESS_EQUAL, clear 1.0 = far — NOT reverse-Z), then
// SampleCmpLevelZero on the cube-array slice. Returns 1 (lit) .. 0 (fully shadowed).
static const float kPointNormalBias = 0.05f; // world units, along the surface normal (B4)
// PCF mirrors the spot path (spotlight_cs.hlsl SampleShadowPCF): a 3x3 texel grid, /9. Spot
// offsets the UV by `invShadowSize`; here the coord is a direction, so we step in the tangent
// plane of d by one texel = 2*m*invRes world units — a 90-deg cube face spans 2*m at view
// depth m across `res` texels, and UV[0,1] <-> tangent[-1,1] gives the 2x. invRes = 1/pointShadowRes.
float PointShadowFactor(PointLightData Ld, float3 P, float3 N, float invRes)
{
    if (Ld.shadowParams.x < 0.0f) { return 1.0f; } // this light has no shadow slot this frame

    float3 Poff = P + N * kPointNormalBias; // normal-offset receiver
    // Rung 2 / Step 21: sample the VSM page pool. Reconstructs the cube face's view-proj + looks up
    // the page table. World-space bias = pull the receiver toward the light before projecting (like
    // the atlas path, avoiding the crushed-perspective-depth bias problem).
    if (useVsm != 0u)
    {
        // Same texel-sized, slope-scaled, world-space bias as the spot path (ZERO NDC bias). Size
        // the offset to the ACTUAL shadow texel at the receiver so acne clears with the minimum
        // push at every distance/level -> minimal Peter-panning. A cube face is a fixed 90deg
        // frustum, so tan(halfFov)=1 and texelWorld = 2*distToLight/VSM_VIRTUAL_RES at the
        // camera-selected level. Push ALONG the light ray (depth-only, crush-immune) slope-scaled
        // by 1/N.L, plus a ~1-texel normal offset.
        const uint  lvl        = VsmSelectLevel(length(P - camPosWS), vsmRefDist, VSM_MAX_LEVEL);
        const float distLight  = length(Ld.position - P);
        const float texelWorld = (2.0f * distLight / VSM_VIRTUAL_RES) * exp2((float)lvl);
        const float3 toL       = normalize(Ld.position - P);
        const float  ndl       = saturate(dot(N, toL));
        const float  slope     = clamp(1.0f / max(ndl, 0.15f), 1.0f, 6.0f);
        float3 PoffV = P + N * (texelWorld * localLateralTexels)
                         + toL * (texelWorld * slope * localDepthPushTexels);
        return VsmPointShadow((uint)Ld.shadowParams.x, PoffV, Ld.position, Ld.shadowParams.z,
                              Ld.shadowParams.w, camPosWS, vsmRefDist, 0.0f,
                              VsmPageTable, VsmPool, gSmpShadowCmp);
    }

    float3 d = Poff - Ld.position; // HW picks the face from d
    float m = max(abs(d.x), max(abs(d.y), abs(d.z)));     // view-space Z on the selected face
    float nearP = Ld.shadowParams.z;
    float farP = max(Ld.shadowParams.w, nearP + 1e-3f);
    // WORLD-space depth bias: pull the compare distance toward the light BEFORE projecting.
    // A constant NDC bias is unusable — perspective depth is crushed into [~0.95,1] at any
    // real distance, so a fixed NDC bias is huge up close (no shadow) and nil far away.
    float mBiased = max(m - Ld.shadowParams.y, nearP);
    float zc = (farP / (farP - nearP)) * (1.0f - nearP / mBiased); // standard LH NDC depth

    // Tangent basis perpendicular to d (seed with the smallest-magnitude axis to avoid a
    // degenerate cross product), then a 3x3 grid stepped by one texel in world units.
    float3 ad = abs(d);
    float3 seed = (ad.x < ad.y && ad.x < ad.z) ? float3(1, 0, 0)
                : (ad.y < ad.z)                ? float3(0, 1, 0) : float3(0, 0, 1);
    float3 t1 = normalize(cross(seed, d));
    float3 t2 = normalize(cross(d, t1));
    float texel = 2.0f * m * invRes;

    float shadow = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float3 sd = d + (t1 * x + t2 * y) * texel;
            shadow += PointShadowCube.SampleCmpLevelZero(gSmpShadowCmp,
                float4(sd, Ld.shadowParams.x), zc); // .w = CUBE INDEX (slot)
        }
    }
    return shadow / 9.0f;
}

[numthreads(8,8,1)]
[RootSignature(POINTLIGHT_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = (uint)screenSize.x;
    uint height = (uint)screenSize.y;
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) * invScreenSize;

    float4 g0 = GB0.SampleLevel(gSmpLinear, uv, 0);
    float4 g1 = GB1.SampleLevel(gSmpLinear, uv, 0);
    float z = DepthT.SampleLevel(gSmpPoint, uv, 0).r;
    if (z <= kEpsilon)
    {
        return;
    }

    float3 albedo = g0.rgb;
    float2 rm = UnpackRM(g0.a);
    float rough = rm.x;
    float metal = rm.y;
    float3 N = normalize(g1.rgb * 2.0 - 1.0);
    float3 P = ReconstructPosWS(uv, z, invProj, invView);
    float3 V = normalize(camPosWS - P);

    // F5: foliage shading model (GBAux.b is an ID -> point sample) + subsurface payload (GB2).
    const float4 gbAux = GBAux.SampleLevel(gSmpPoint, uv, 0);
    uint shadingModel = DecodeShadingModel(gbAux.b);
    bool isFoliage = (shadingModel == kShadingModelTwoSidedFoliage);
    const float transmissionNormalWeight = saturate(gbAux.a);
    float3 subsurface = 0.0f.xxx;
    if (isFoliage)
    {
        subsurface = GB2.SampleLevel(gSmpPoint, uv, 0).rgb;
    }

    float4 base = LightTarget[dispatchThreadId.xy];
    float3 accum = base.rgb;

    uint count = (uint)lightCount;
    for (uint i = 0; i < count; ++i)
    {
        PointLightData Ld = PointLights[i];
        float3 Lvec = Ld.position - P;
        float dist = length(Lvec);
        if (dist > Ld.radius || Ld.radius <= kEpsilon)
        {
            continue;
        }
        float3 L = Lvec / max(dist, kEpsilon);

        const float atten = LightDistanceAttenuation(dist, Ld.radius); // P16.5

        BRDFInput bi;
        bi.albedo = albedo;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        const float3 radiance = Ld.color * Ld.intensity * atten;

        if (isFoliage)
        {
            // F5: reuse the shared F4 foliage helper so point lights match the directional model
            // exactly. Radius attenuation + shadow apply to both the front lobe and transmission.
            FoliageResult fr = EvalFoliageBRDF(
                bi, subsurface, 0.0f, transmissionNormalWeight);
            if (fr.NdotL > 0.0)
            {
                bool useMap, useContact;
                LocalShadowSources(Ld.shadowParams.x >= 0.0f, useMap, useContact);
                float shadow = useMap ? PointShadowFactor(Ld, P, N, invPointShadowSize) : 1.0f;
                if (useContact)
                {
                    shadow = ApplyContactShadow(shadow, P, N, L, fr.NdotL, dispatchThreadId.xy, camPosWS,
                                                viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                                                MakeContactParams());
                }
                accum += (fr.diffBRDF + fr.specBRDF) * fr.NdotL * radiance * shadow;
            }
            // Transmission: flipped-normal shadow sample so the leaf's light-facing face does not
            // self-shadow the light passing through it (mirrors lighting_cs F4).
            if (any(fr.transBRDF > 0.0))
            {
                bool useMapT, useContactT;
                LocalShadowSources(Ld.shadowParams.x >= 0.0f, useMapT, useContactT);
                float shadowT = useMapT ? PointShadowFactor(Ld, P, -N, invPointShadowSize) : 1.0f;
                if (useContactT)
                {
                    shadowT = ApplyContactShadow(shadowT, P, -N, L, saturate(dot(-N, L)),
                                                 dispatchThreadId.xy, camPosWS,
                                                 viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                                                 MakeContactParams());
                }
                accum += fr.transBRDF * radiance * shadowT;
            }
        }
        else
        {
            BRDFResult br = EvalBRDF(bi);
            if (br.NdotL <= 0.0)
            {
                continue;
            }

            bool useMap, useContact;
            LocalShadowSources(Ld.shadowParams.x >= 0.0f, useMap, useContact);
            float shadow = useMap ? PointShadowFactor(Ld, P, N, invPointShadowSize) : 1.0f;
            if (useContact)
            {
                shadow = ApplyContactShadow(shadow, P, N, L, br.NdotL, dispatchThreadId.xy, camPosWS,
                                            viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                                            MakeContactParams());
            }
            if (shadow <= 0.0f)
            {
                continue;
            }

            accum += (br.diffBRDF + br.specBRDF) * br.NdotL * radiance * shadow;
        }
    }

    LightTarget[dispatchThreadId.xy] = float4(accum, base.a);
}
