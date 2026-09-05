// Volumetric fog, pass 2 of 2: front-to-back integration of the scattering volume into
// (accumulated in-scattered light, accumulated transmittance) per slice -- UE's
// FinalIntegrationCS (VolumetricFog.usf:1075-1120), including Frostbite's energy-conserving
// per-slice integral and the near fade-in. Every consumer samples the result by view depth.
//
// b0 = FogCB (fog_common.hlsli)   t0 FogScatter (3D)   u0 FogIntegrated (3D)
#pragma pack_matrix(row_major)
#include "fog_common.hlsli"

#define FOG_INTEGRATE_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

FOG_CB(b0)

Texture3D<float4>   FogScatter    : register(t0);
RWTexture3D<float4> FogIntegrated : register(u0);

[numthreads(8, 8, 1)]
[RootSignature(FOG_INTEGRATE_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (any(dtid.xy >= fogGridSize.xy)) { return; }

    float3 accumulatedLight = 0.0f.xxx;
    float accumulatedTransmittance = 1.0f;
    float accumulatedDepth = 0.0f;
    float3 previousSliceWorld = FogCellWorldPosition(uint3(dtid.xy, 0u), float3(0.5f, 0.5f, 0.0f),
                                                     fogGridSize, fogGridZParams, fogProjZ, fogInvViewProjNoJitter);
    // The slice count is the buffer's, a literal at the top of the dispatch: the loop bound is
    // never read from the constant buffer (engine rule).
    [loop]
    for (uint layer = 0u; layer < 64u; ++layer)
    {
        if (layer >= fogGridSize.z) { break; }
        const uint3 cell = uint3(dtid.xy, layer);
        const float4 scatteringAndExtinction = FogScatter[cell];

        const float3 sliceWorld = FogCellWorldPosition(cell, 0.5f.xxx, fogGridSize, fogGridZParams,
                                                       fogProjZ, fogInvViewProjNoJitter);
        const float stepLength = length(sliceWorld - previousSliceWorld);
        previousSliceWorld = sliceWorld;

        const float transmittance = exp(-scatteringAndExtinction.w * stepLength);
        accumulatedDepth += stepLength;
        const float fadeIn = saturate(accumulatedDepth * fogNearFadeInInv);

        // "Physically Based and Unified Volumetric Rendering in Frostbite": the analytic integral
        // of the scattering over the slice under its own extinction.
        const float3 sliceIntegral = fadeIn * (scatteringAndExtinction.rgb - scatteringAndExtinction.rgb * transmittance)
                                     / max(scatteringAndExtinction.w, 0.00001f);
        accumulatedLight += sliceIntegral * accumulatedTransmittance;
        accumulatedTransmittance *= lerp(1.0f, transmittance, fadeIn);

        FogIntegrated[cell] = float4(accumulatedLight, accumulatedTransmittance);
    }
}
