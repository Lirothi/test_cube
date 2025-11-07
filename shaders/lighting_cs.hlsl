// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3) SRV(t4) SRV(t5)) TABLE(UAV(u0)) TABLE(SAMPLER(s0) SAMPLER(s1))
// t0..t3 : GBuffer textures (GB0, GB1, GB2, GBVelocity)
// t4     : Depth (R32F)
// t5     : Shadow atlas
// u0     : Light accumulation target (RWTexture2D)
// s0     : PointClamp
// s1     : ComparisonLinearClamp

#pragma pack_matrix(row_major)

#include "utils.hlsl"

Texture2D GB0 : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D GBVelocity : register(t3);
Texture2D DepthT : register(t4);
Texture2D ShadowAtlas : register(t5);
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpPoint : register(s0);
SamplerComparisonState gSmpLinear : register(s1);

cbuffer PerFrame : register(b0)
{
    float3 sunDirWS;
    float ambientIntensity;
    float3 lightRgb;
    float exposure;
    float3 camPosWS;
    float3 camDirWS;

    float4x4 view;
    float4x4 invView;
    float4x4 invProj;

    float4x4 lightViewProj[4];
    float4 cascadeScaleBias[4];
    float4 cascadeSplitsVS;
    float2 shadowAtlasSize;
    float4 shadowBiasNDC;
    float4 normalBiasWS;
    float2 screenSize;
    float2 invScreenSize;
}

static const float shadowBias = 0.0015f;
static const float pcfRadius = 1.0f;

int ChooseCascadeIndex(float3 Pws)
{
    float z = dot(Pws - camPosWS, camDirWS);
    float3 gt = saturate(sign(z.xxx - cascadeSplitsVS.yzw));
    return (int)(gt.x + gt.y + gt.z);
}

float ShadowPCF(float2 uv, float zRef)
{
    return ShadowAtlas.SampleCmpLevelZero(gSmpLinear, uv, zRef).r;
}

float ShadowPCF3x3(float2 uv, float zRef, float2 texel, float radiusPx)
{
    float s = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 off = float2(x, y) * texel * radiusPx;
            s += ShadowAtlas.SampleCmpLevelZero(gSmpLinear, uv + off, zRef).r;
        }
    }
    return s / 9.0;
}

float SampleShadowCSM(float3 Pws, float NdotL, float3 Nws)
{
    int idx = ChooseCascadeIndex(Pws);
    float4x4 LVP = lightViewProj[idx];
    float4 sb = cascadeScaleBias[idx];
    float2 scale = sb.xy;
    float2 bias = sb.zw;

    float3 Poff = Pws + Nws * normalBiasWS[idx];

    float4 lc = mul(float4(Poff, 1), LVP);
    float2 uv = (lc.xy / max(1e-6, lc.w)) * float2(0.5, -0.5) + float2(0.5, 0.5);
    float z = lc.z / max(1e-6, lc.w);

    uv = uv * scale + bias;

    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1.0;
    }

    float2 texel = 1.0 / shadowAtlasSize;
    float bBase = shadowBiasNDC[idx];
    float b = bBase + (1.0 - saturate(NdotL)) * bBase;

    if (idx < 3)
    {
        return ShadowPCF3x3(uv, z - b, texel, pcfRadius);
    }
    return ShadowPCF(uv, z - b);
}

[numthreads(8,8,1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = (uint)screenSize.x;
    uint height = (uint)screenSize.y;
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) * invScreenSize;

    float4 gb0 = GB0.SampleLevel(gSmpPoint, uv, 0);
    float4 gb1 = GB1.SampleLevel(gSmpPoint, uv, 0);

    float3 albedo = gb0.rgb;
    float2 rm = UnpackRM(gb0.a);
    float rough = rm.x;
    float metal = rm.y;

    float3 N = normalize(gb1.rgb * 2.0 - 1.0);
    float z = DepthT.SampleLevel(gSmpPoint, uv, 0).r;
    if (z <= kEpsilon)
    {
        LightTarget[dispatchThreadId.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float3 P = ReconstructPosWS(uv, z, invProj, invView);

    const float3 V = normalize(camPosWS - P);
    const float3 L = normalize(-sunDirWS);

    const float3 ambient = albedo * ambientIntensity;

    BRDFInput bi;
    bi.albedo = albedo;
    bi.rough = rough;
    bi.metal = metal;
    bi.N = N;
    bi.V = V;
    bi.L = L;

    BRDFResult br = EvalBRDF(bi);
    float3 color = ambient * lightRgb;
    if (br.NdotL > 0.0)
    {
        float shadow = SampleShadowCSM(P, br.NdotL, N);
        float3 direct = (br.diffBRDF + br.specBRDF) * br.NdotL * lightRgb * shadow;
        color += direct;
    }

    LightTarget[dispatchThreadId.xy] = float4(color * exposure, 1.0);
}
