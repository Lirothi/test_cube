// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3) SRV(t4)) TABLE(UAV(u0)) TABLE(SAMPLER(s0) SAMPLER(s1))
// t0..t3 : GBuffer (GB0, GB1, GB2, Depth)
// t4     : StructuredBuffer<PointLightData>
// u0     : Light accumulation RWTexture2D
// s0     : LinearClamp
// s1     : PointClamp

#pragma pack_matrix(row_major)

#include "utils.hlsl"

Texture2D GB0 : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D DepthT : register(t3);
StructuredBuffer<PointLightData> PointLights : register(t4);
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpLinear : register(s0);
SamplerState gSmpPoint : register(s1);

struct PointLightData
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

cbuffer PointLightFrame : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float3 camPosWS;
    uint lightCount;
    float2 screenSize;
    float2 invScreenSize;
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

        float x = saturate(1.0 - dist / Ld.radius);
        float atten = x * x;

        BRDFInput bi;
        bi.albedo = albedo;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        BRDFResult br = EvalBRDF(bi);
        if (br.NdotL <= 0.0)
        {
            continue;
        }

        float3 radiance = Ld.color * Ld.intensity * atten;
        accum += (br.diffBRDF + br.specBRDF) * br.NdotL * radiance;
    }

    LightTarget[dispatchThreadId.xy] = float4(accum, base.a);
}
