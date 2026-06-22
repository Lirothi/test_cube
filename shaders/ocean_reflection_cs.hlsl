#define OCEAN_REFLECTION_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

#pragma pack_matrix(row_major)

#include "utils.hlsl"

Texture2D SceneColorTexture : register(t0);
Texture2D SceneDepthTexture : register(t1);
RWTexture2D<float4> OceanReflectionOut : register(u0);
SamplerState LinearClampSampler : register(s0);
SamplerState PointSampler : register(s1);

cbuffer OceanReflectionCB : register(b0)
{
    float4x4 view;
    float4x4 proj;
    float4x4 invView;
    float4x4 invProj;
    float depthA;
    float depthB;
    float2 screenSize;
    float2 invScreenSize;
    float2 outputSize;
    float3 camPosWS;
    float waterHeight;
};

static const float kEps = 1e-6f;
static const float kUnderwaterSsrHitBias = -0.05f;

float DepthToViewZ_Fast(float d)
{
    return depthB / (d - depthA);
}

float ReadDepth(float2 uv)
{
    return SceneDepthTexture.SampleLevel(PointSampler, uv, 0).r;
}

float3 ReconstructPosVS(float2 uv, float d)
{
    float2 ndc = UVtoNDC(uv);
    float4 clip = float4(ndc, d, 1.0f);
    float4 v = mul(clip, invProj);
    return v.xyz / max(v.w, kEps);
}

float3 ReconstructPosWS(float2 uv, float d)
{
    float3 viewPos = ReconstructPosVS(uv, d);
    float4 worldPos = mul(float4(viewPos, 1.0f), invView);
    return worldPos.xyz / max(worldPos.w, kEps);
}

float3 ViewRayWS(float2 uv)
{
    float2 ndc = UVtoNDC(uv);
    float4 clip = float4(ndc, 0.0f, 1.0f);
    float4 viewPos = mul(clip, invProj);
    float3 viewDir = normalize(viewPos.xyz / max(viewPos.w, kEps));
    return normalize(mul(float4(viewDir, 0.0f), invView).xyz);
}

#define SSR_TRACE_PROJ proj
#define SSR_TRACE_INV_SCREEN_SIZE invScreenSize
#define SSR_TRACE_SCREEN_SIZE screenSize
#define SSR_TRACE_READ_DEPTH(uv) ReadDepth(uv)
#define SSR_TRACE_DEPTH_TO_VIEW_Z(depthRaw) DepthToViewZ_Fast(depthRaw)
#define SSR_TRACE_RECONSTRUCT_POS_VS(uv, depthRaw) ReconstructPosVS(uv, depthRaw)
#include "ssr_trace_logmarch.hlsli"

[numthreads(8, 8, 1)]
[RootSignature(OCEAN_REFLECTION_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint outWidth, outHeight;
    OceanReflectionOut.GetDimensions(outWidth, outHeight);
    if (dispatchThreadId.x >= outWidth || dispatchThreadId.y >= outHeight)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / max(outputSize, float2(1.0f, 1.0f));
    float3 rayDirWS = ViewRayWS(uv);
    float denom = rayDirWS.y;
    if (abs(denom) < 1e-4f)
    {
        OceanReflectionOut[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float t = (waterHeight - camPosWS.y) / denom;
    if (t <= 0.0f)
    {
        OceanReflectionOut[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 waterPosWS = camPosWS + rayDirWS * t;
    float3 Pv = mul(float4(waterPosWS, 1.0f), view).xyz;
    if (Pv.z <= 0.0f)
    {
        OceanReflectionOut[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 Nv = normalize(mul(float3(0.0f, 1.0f, 0.0f), (float3x3)view));
    SSRHit ssr = TraceSSR_LogMarch(Pv, Nv, uv * screenSize);
    if (ssr.hit == 0)
    {
        OceanReflectionOut[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float hitDepth = ReadDepth(ssr.uv);
    float3 hitWS = ReconstructPosWS(ssr.uv, hitDepth);
    if (hitWS.y < waterHeight + kUnderwaterSsrHitBias)
    {
        OceanReflectionOut[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 reflectedScene = SceneColorTexture.SampleLevel(LinearClampSampler, saturate(ssr.uv), 0).rgb;
    OceanReflectionOut[dispatchThreadId.xy] = float4(reflectedScene * ssr.visibility, ssr.visibility);
}
