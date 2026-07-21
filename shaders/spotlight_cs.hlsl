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

#include "utils.hlsl"
#include "vsm_sample.hlsli"

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
    float2 _vsmPad;
};

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
        float biasScale = clamp(length(P - camPosWS) / max(vsmRefDist, 1e-3f), 1.0f,
                                (float)(1u << VSM_MAX_LEVEL));
        float3 PoffV = P + N * (normalBias * biasScale);
        uint slot = (uint)light.shadowParams.y;
        return VsmSpotShadow(slot, light.viewProj, PoffV, camPosWS, vsmRefDist, depthBias,
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

        float x = saturate(1.0f - dist / light.positionRange.w);
        float distAtten = x * x;

        BRDFInput bi;
        bi.albedo = albedo;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        BRDFResult br = EvalBRDF(bi);
        if (br.NdotL <= 0.0f)
        {
            continue;
        }

        float shadow = ComputeSpotShadow(light, P, N, br.NdotL);
        float3 radiance = light.colorIntensity.xyz * light.colorIntensity.w * distAtten * angleAtten;
        accum += (br.diffBRDF + br.specBRDF) * br.NdotL * radiance * shadow;
    }

    LightTarget[dispatchThreadId.xy] = float4(accum, base.a);
}

