// Tier-2 hardware ray-traced reflections (S7 + S9 + S10) -- the MONOLITHIC dispatch.
//
// cs_6_6 inline RayQuery + SM6.6 dynamic resources. One SHARP reflection ray per
// GBuffer surface against the TLAS; the hit is shaded to MATCH THE BASE PASS. All of
// that lives in rt_reflect_common.hlsli now (gather-then-shade split, async prep) --
// this file only combines the two phases inline: radiancePart + the lit-HDR screen
// sample when the hit reprojects on screen.
//
// The OPAQUE reflection runs the split (rt_trace_cs -> rt_resolve_cs); THIS shader
// serves the GLASS reflection dispatch, which runs after lighting anyway and is not
// worth two extra dispatches at its resolution.
//
// On miss: coverage 0 -> compose's skybox fallback. Writes premultiplied
// (rgb, coverage) into the reflection target; blur + compose unchanged.
//
// Glossy/rough blur is NOT done here: stochastic glossy needs a real denoiser
// (DLSS Ray Reconstruction) — see plan S16. Reflections are sharp + clean.
#include "rt_reflect_common.hlsli"

[numthreads(8, 8, 1)]
[RootSignature(RT_REFLECT_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }

    RWTexture2D<float4> outTex = ResourceDescriptorHeap[reflectionUavIndex];

    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f); // no surface / miss -> skybox via compose
    float3 origin, dir, camPos;
    float tMin;
    uint raySeed;
    if (RtSetupReflectionRay(dtid.xy, origin, dir, camPos, tMin, raySeed))
    {
        RtTraceResult tr;
        if (TraceReflectionCore(origin, dir, camPos, tMin, raySeed, tr))
        {
            float3 radiance = tr.radiancePart;
            if (tr.reuseScreen)
            {
                Texture2D lightT = ResourceDescriptorHeap[lightIndex];
                radiance += lightT.SampleLevel(gSmp, tr.reuseUv, 0).rgb; // exact lit HDR
            }
            result = float4(radiance, 1.0f); // premultiplied, full coverage
        }
    }

    outTex[dtid.xy] = result;
}
