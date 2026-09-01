#define SPOTLIGHT_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=10, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE))"
// t0..t3 : GBuffer (GB0, GB1, GB2, GBVelocity)
// t4     : Depth
// t5     : Texture2DArray Spot Shadow Atlas
// t6     : StructuredBuffer<SpotLightData>
// t7     : StructuredBuffer<uint> VSM page table (Rung 2 / Step 21)
// t8     : Texture2D VSM physical page pool depth
// t9     : GBAux (AO, indirect specular scale, shading model)
// u0     : Light accumulation RWTexture2D
// s0     : LinearClamp
// s1     : PointClamp
// s2     : ComparisonLinearClamp

#pragma pack_matrix(row_major)

#include "utils.hlsli"
#include "vsm_sample.hlsli"
#include "contact_shadow.hlsli"

struct SpotLightData
{
    float4 positionRange;      // xyz = position, w = range
    float4 directionCosOuter;  // xyz = direction, w = cos(outer)
    float4 colorIntensity;     // xyz = color, w = intensity
    float4 shadowParams;       // x = cos(inner), y = shadow index, z = invAngleRange, w = depth bias
    float4 shadowParams2;      // x = normal bias (world units)
    float4x4 viewProj;
};

Texture2D GB0 : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D GBVelocity : register(t3);
Texture2D DepthT : register(t4);
Texture2DArray SpotShadowAtlas : register(t5);
StructuredBuffer<SpotLightData> SpotLights : register(t6);
StructuredBuffer<uint> VsmPageTable : register(t7); // Rung 2 / Step 21
Texture2D VsmPool : register(t8);
Texture2D GBAux : register(t9);
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpLinear : register(s0);
SamplerState gSmpPoint : register(s1);
SamplerComparisonState gSmpShadow : register(s2);

cbuffer SpotLightFrame : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float3 camPosWS;
    uint   lightCount;
    float2 screenSize;
    float2 invScreenSize;
    float2 invShadowSize;
    uint   useVsm;      // Rung 2 / Step 21: sample the VSM page pool instead of the atlas
    float  vsmRefDist;  // VSM mip level-select reference distance
    // VSM local-light shadow bias in shadow-texel units (live-tunable, vsm::g_local*Texels). Lateral =
    // surface-normal offset (~1 texel => Peter-panning at most a texel); depth push = along-the-light-
    // ray, slope-scaled, so grazing acne clears without moving flat lit ground. Per-light
    // shadowNormalBias/shadowDepthBias now only drive the Legacy (atlas) path.
    float  localLateralTexels;
    float  localDepthPushTexels;
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
};
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


float SampleShadowPCF(float3 uvw, float depth, float2 texel)
{
    float shadow = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texel;
            shadow += SpotShadowAtlas.SampleCmpLevelZero(gSmpShadow, float3(uvw.xy + offset, uvw.z), depth);
        }
    }
    return shadow / 9.0f;
}

float ComputeSpotShadow(const SpotLightData light, float3 P, float3 N, float NdotL)
{
    // shadowParams.y < 0 => this spot has no shadow slot this frame (unshadowed).
    if (light.shadowParams.y < 0.0f)
    {
        return 1.0f;
    }

    float normalBias = light.shadowParams2.x;
    float depthBias = light.shadowParams.w;
    float3 Poff = P + N * normalBias;

    // Rung 2 / Step 21: sample through the VSM page table + physical pool instead of the atlas.
    if (useVsm != 0u)
    {
        // Coarse VSM mip levels (far from the camera) cover 2-4x more world per texel than the 512^2
        // atlas the constant normalBias was tuned for, so that fixed offset no longer clears the
        // texel -> the receiver self-shadows into acne STRIPES on grazing ground. Scale the normal
        // offset by the LOD coarsening (continuous ~distCam/refDist, the SAME quantity that selects
        // the mip level), clamped to the coarsest level. Continuous (no 2x step) => no ring at a
        // level boundary. Mirrors the directional clipmap's distance-scaled offset.
        // Bias in WORLD space with ZERO NDC depth bias. An NDC bias is crush-sensitive: a spot's
        // perspective depth compresses to ~[0.95,1] and the crush scales with far/near, so a fixed
        // NDC bias becomes a huge world-space Peter-pan for wide/long spots (shadow detaches metres
        // off the base). Instead size the bias to the ACTUAL shadow texel at the receiver -- the
        // perspective half-width (distToLight * tan(outer)) over the virtual resolution at the
        // camera-selected level. That captures the spot's cone width, which the old camera-distance
        // biasScale ignored (wide spots were under-biased -> the acne 'stripes'). The push is ALONG
        // the light ray (depth-only, no lateral texel shift) and slope-scaled by 1/N.L so grazing
        // surfaces (where acne lives) get the depth while flat lit ground (where Peter-panning shows)
        // stays put. A small ~1-texel normal offset handles the rest.
        const uint  lvl        = VsmSelectLevel(length(P - camPosWS), vsmRefDist, VSM_MAX_LEVEL);
        const float cosOuter   = max(light.directionCosOuter.w, 1e-3f);
        const float tanOuter   = sqrt(saturate(1.0f - cosOuter * cosOuter)) / cosOuter;
        const float distLight  = length(light.positionRange.xyz - P);
        const float texelWorld = (2.0f * distLight * tanOuter / VSM_VIRTUAL_RES) * exp2((float)lvl);
        const float3 toL       = normalize(light.positionRange.xyz - P);
        const float  ndl       = saturate(dot(N, toL));
        const float  slope     = clamp(1.0f / max(ndl, 0.15f), 1.0f, 6.0f);
        float3 PoffV = P + N * (texelWorld * localLateralTexels)
                         + toL * (texelWorld * slope * localDepthPushTexels);
        uint slot = (uint)light.shadowParams.y;
        return VsmSpotShadow(slot, light.viewProj, PoffV, camPosWS, vsmRefDist, 0.0f,
                             VsmPageTable, VsmPool, gSmpShadow);
    }

    float4 clip = mul(float4(Poff, 1.0f), light.viewProj);
    float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    float2 uv = ndc.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    float depth = ndc.z;

    if (any(uv < 0.0f) || any(uv > 1.0f) || depth <= 0.0f || depth >= 1.0f)
    {
        return 1.0f;
    }

    float3 uvw = float3(uv, light.shadowParams.y);
    return SampleShadowPCF(uvw, depth - depthBias, invShadowSize);
}

[numthreads(8, 8, 1)]
[RootSignature(SPOTLIGHT_CS_RS)]
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
        SpotLightData light = SpotLights[i];
        float3 Lvec = light.positionRange.xyz - P;
        float dist = length(Lvec);
        if (dist >= light.positionRange.w || light.positionRange.w <= kEpsilon)
        {
            continue;
        }

        float3 L = Lvec / max(dist, kEpsilon);
        float spotCos = dot(-L, light.directionCosOuter.xyz);
        if (spotCos <= light.directionCosOuter.w)
        {
            continue;
        }

        float angleAtten = saturate((spotCos - light.directionCosOuter.w) * light.shadowParams.z);
        angleAtten = angleAtten * angleAtten;

        const float distAtten = LightDistanceAttenuation(dist, light.positionRange.w); // P16.5

        BRDFInput bi;
        bi.albedo = albedo;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        const float3 radiance = light.colorIntensity.xyz * light.colorIntensity.w * distAtten * angleAtten;

        if (isFoliage)
        {
            // F5: shared F4 foliage helper -> spot lights match the directional/point model.
            // Cone + distance attenuation and shadow apply to the front lobe and transmission.
            FoliageResult fr = EvalFoliageBRDF(
                bi, subsurface, 0.0f, transmissionNormalWeight);
            if (fr.NdotL > 0.0f)
            {
                bool useMap, useContact;
                LocalShadowSources(light.shadowParams.y >= 0.0f, useMap, useContact);
                float shadow = useMap ? ComputeSpotShadow(light, P, N, fr.NdotL) : 1.0f;
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
            if (any(fr.transBRDF > 0.0f))
            {
                bool useMapT, useContactT;
                LocalShadowSources(light.shadowParams.y >= 0.0f, useMapT, useContactT);
                float shadowT = useMapT ? ComputeSpotShadow(light, P, -N, saturate(dot(-N, L))) : 1.0f;
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
            if (br.NdotL <= 0.0f)
            {
                continue;
            }

            bool useMap, useContact;
            LocalShadowSources(light.shadowParams.y >= 0.0f, useMap, useContact);
            float shadow = useMap ? ComputeSpotShadow(light, P, N, br.NdotL) : 1.0f;
            if (useContact)
            {
                shadow = ApplyContactShadow(shadow, P, N, L, br.NdotL, dispatchThreadId.xy, camPosWS,
                                            viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                                            MakeContactParams());
            }
            accum += (br.diffBRDF + br.specBRDF) * br.NdotL * radiance * shadow;
        }
    }

    LightTarget[dispatchThreadId.xy] = float4(accum, base.a);
}

