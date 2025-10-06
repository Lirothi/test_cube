// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3) SRV(t4) SRV(t5)) TABLE(UAV(u0)) TABLE(SAMPLER(s0) SAMPLER(s1) SAMPLER(s2))
// t0..t3 : GBuffer (GB0, GB1, GB2, Depth)
// t4     : Texture2DArray Spot Shadow Atlas
// t5     : StructuredBuffer<SpotLightData>
// u0     : Light accumulation RWTexture2D
// s0     : LinearClamp
// s1     : PointClamp
// s2     : ComparisonLinearClamp

#pragma pack_matrix(row_major)

#include "utils.hlsl"

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
Texture2D DepthT : register(t3);
Texture2DArray SpotShadowAtlas : register(t4);
StructuredBuffer<SpotLightData> SpotLights : register(t5);
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
    float2 shadowSize;
    float2 invShadowSize;
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
    float normalBias = light.shadowParams2.x;
    float depthBias = light.shadowParams.w;

    float3 Poff = P + N * normalBias;
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
    if (z >= 1.0 - kEpsilon)
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

    uint count = min(lightCount, (uint)SpotLights.Length);
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

