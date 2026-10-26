// RT reflections, SHADE phase: payload + lit HDR -> the premultiplied reflection target.
// The cheap half of the gather-then-shade split (see rt_trace_cs.hlsl) and the ONLY RT
// reflection dispatch that consumes the lighting pass's output.
//
// An off-screen hit's SKY lighting arrives apart (payloadSky, RtTraceResult.skyPart) and is scaled
// here by the sky visibility of the REFLECTOR (rt_reflect_common.hlsli RtReflectorSkyVisibility).
// Without it every off-screen hit was lit as if under the open sky (2026-10-26: the bathroom's walls
// behind the camera came back daylit and blue in the panelling and the brass; SSR, reading the
// screen, did not).
#include "rt_reflect_common.hlsli"

[numthreads(8, 8, 1)]
[RootSignature(RT_REFLECT_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }

    Texture2D payloadRadiance = ResourceDescriptorHeap[payloadRadianceIndex];
    Texture2D<float2> payloadUv = ResourceDescriptorHeap[payloadUvIndex];
    RWTexture2D<float4> outTex = ResourceDescriptorHeap[reflectionUavIndex];

    const float4 payload = payloadRadiance[dtid.xy];
    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f); // miss -> skybox via compose
    if (payload.a != kRtPayloadMiss)
    {
        float3 radiance = payload.rgb;
        if (payload.a < 0.0f) // the hit is on screen: finish with its lit HDR (RtPayloadAlpha)
        {
            Texture2D lightT = ResourceDescriptorHeap[lightIndex];
            // exact lit HDR; the light target is pre-exposed, the reflection buffer holds radiance
            radiance += lightT.SampleLevel(gSmp, payloadUv[dtid.xy], 0).rgb * lightInvPreExposure;
        }
        else
        {
            Texture2D payloadSky = ResourceDescriptorHeap[payloadSkyIndex];
            radiance += payloadSky[dtid.xy].rgb * RtReflectorSkyVisibility(dtid.xy);
        }
        result = float4(radiance, 1.0f); // premultiplied, full coverage
    }

    outTex[dtid.xy] = result;
}
