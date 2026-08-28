// RT reflections, SHADE phase: payload + lit HDR -> the premultiplied reflection target.
// The cheap half of the gather-then-shade split (see rt_trace_cs.hlsl) and the ONLY RT
// reflection dispatch that consumes the lighting pass's output. Bit-parity with the old
// monolithic pass: radiancePart carries `direct + env` already summed in trace order, and
// float addition commutes, so `+ lightT` here lands on the identical value.
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
        if (payload.a == kRtPayloadReuse)
        {
            Texture2D lightT = ResourceDescriptorHeap[lightIndex];
            radiance += lightT.SampleLevel(gSmp, payloadUv[dtid.xy], 0).rgb; // exact lit HDR
        }
        result = float4(radiance, 1.0f); // premultiplied, full coverage
    }

    outTex[dtid.xy] = result;
}
