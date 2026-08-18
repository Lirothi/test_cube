#define OCEAN_REFLECTION_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

#pragma pack_matrix(row_major)

#include "utils.hlsli"

Texture2D SceneColorTexture : register(t0);
Texture2D SceneDepthTexture : register(t1);
// P13: the FURTHEST depth pyramid, the same chain the deferred SSR pass traces. `HzbFurthest` is
// the name ssr_trace_ue.hlsli's host contract asks for.
Texture2D HzbFurthest : register(t2);
RWTexture2D<float4> OceanReflectionOut : register(u0);
SamplerState LinearClampSampler : register(s0);
// Named to the trace contract as well: the UE march point-samples the pyramid through it.
SamplerState gSmpPoint : register(s1);

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
    // P13. This block MIRRORS OceanReflectionConstants in SceneRenderer.cpp, which is uploaded by
    // raw memcpy -- offsets here and there must agree, so keep every row 16-byte tidy.
    // `tech`, not `technique`: the latter is still a reserved effects-framework keyword in DXC.
    // ssr_cs.hlsl names its field the same way for the same reason.
    uint tech;             // SSR_TECHNIQUE_*, from the same `ssr.technique` setting as the SSR pass
    uint useHzb;           // 0 = no pyramid built yet, march the log tracer instead
    uint hzbMipCount;
    uint frameIndexMod8;   // UE's StateFrameIndexMod8: the eight-frame sample-phase cycle
    float2 hzbSize;        // pyramid mip 0 in texels (HALF the render resolution)
    float2 hzbInvSize;
    float ueStartMipLevel;
    float ueSlopeCompareToleranceScale;
    uint ueConfirmRetries;
    uint ueRefineSteps;
    // Already collapsed to UE's mirror case on the CPU: a plane is always their Roughness < 0.1
    // branch, so the ray count never reaches this shader.
    uint ueNumSteps;
    uint _oceanUePad0;
    uint _oceanUePad1;
    uint _oceanUePad2;
};

static const float kEps = 1e-6f;
static const float kUnderwaterSsrHitBias = -0.05f;

float DepthToViewZ_Fast(float d)
{
    return depthB / (d - depthA);
}

float ReadDepth(float2 uv)
{
    return SceneDepthTexture.SampleLevel(gSmpPoint, uv, 0).r;
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
#include "ssr_trace_ue.hlsli" // Unreal's own SSR ray cast; reuses SSRHit / BuildSsrHit above

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

    SSRHit ssr;
    if (tech == SSR_TECHNIQUE_UE && useHzb != 0u)
    {
        // The reflector here is the water PLANE, not the displaced surface -- the wave normal is
        // applied later, when the ocean shader samples this buffer. So this is always UE's mirror
        // case: one ray along the plane's reflection, with the collapsed step budget.
        const float3 unitPositionFrom = normalize(Pv);
        const float3 pivot = normalize(reflect(unitPositionFrom, Nv));
        // Integer coordinates of THIS dispatch, not uv*screenSize. The reflection target can be
        // scaled below render resolution, and a stride above one texel re-correlates UE's
        // interleaved-gradient phase across neighbours -- which is the exact defect P13 fixed.
        ssr = TraceSSR_UeHzbRay(Pv, unitPositionFrom, pivot, 0.0f, uv,
                                float2(dispatchThreadId.xy), frameIndexMod8, ueNumSteps);
    }
    else
    {
        // LogMarch, and the safety net for a frame with no pyramid yet.
        ssr = TraceSSR_LogMarch(Pv, Nv, uv * screenSize);
    }

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
