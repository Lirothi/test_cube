// RT reflections, GATHER phase (async-compute prep): traversal + full hit shading EXCEPT the
// lit-HDR screen sample. Everything this dispatch reads -- TLAS, depth, gb1, the bindless
// geometry/material table, the skybox/irradiance cubes and the spot/point light buffers (filled
// on the CPU in EnsureFrameResources) -- exists before the shadow/lighting passes run, so this
// pass is the one that later moves to the async compute queue and overlaps them. Only
// rt_resolve_cs touches the lighting output.
//
// Payload (reflection res):
//   payloadRadiance rgba16f: rgb = radiancePart (env + analytic direct when off-screen),
//                            a   = mode code (kRtPayloadMiss/Complete/Reuse -- exact in fp16);
//   payloadUv       rg16unorm: the lit-HDR sample position when mode == Reuse. UNORM16 keeps the
//                            fetch within ~0.04 px at 2560 wide; fp16 would wobble it by >1 px.
#include "rt_reflect_common.hlsli"

[numthreads(8, 8, 1)]
[RootSignature(RT_REFLECT_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }

    RWTexture2D<float4> payloadRadiance = ResourceDescriptorHeap[payloadRadianceIndex];
    RWTexture2D<float2> payloadUv = ResourceDescriptorHeap[payloadUvIndex];

    float4 payload = float4(0.0f, 0.0f, 0.0f, kRtPayloadMiss);
    float2 reuseUv = float2(0.0f, 0.0f);
    float3 origin, dir, camPos;
    float tMin;
    uint raySeed;
    if (RtSetupReflectionRay(dtid.xy, origin, dir, camPos, tMin, raySeed))
    {
        RtTraceResult tr;
        if (TraceReflectionCore(origin, dir, camPos, tMin, raySeed, tr))
        {
            payload = float4(tr.radiancePart,
                             tr.reuseScreen ? kRtPayloadReuse : kRtPayloadComplete);
            reuseUv = tr.reuseUv;
        }
    }

    payloadRadiance[dtid.xy] = payload;
    payloadUv[dtid.xy] = reuseUv;
}
