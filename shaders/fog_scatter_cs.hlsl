// Volumetric fog, pass 1 of 2: per froxel cell, the medium and the light scattered towards the
// camera -- UE's MaterialSetupCS folded into LightScatteringCS (VolumetricFog.usf:122-140 and
// :766-1000). Plan part A (docs/volumetric_fog_sky_clouds_ssgi_plan.md).
//
// Per cell (jittered inside it, UE's Halton offsets + optional history):
//   sigma  = extinction of the analytic height fog at the cell's height (fog_common.hlsli)
//   S      = albedo * sigma
//   L      = sunColor * shadow(P) * HG(g, dot(toSun, -V)) * sunScatter          (:888)
//          + skyIrradiance(-V) * skyScatter                                     (:936-942, SH -> our cube)
//   out    = (preExposure * L * S, sigma), lerped with the reprojected history (:1027-1033)
// The sun's shadow is the lighting pass's own sampler (lighting_cb.hlsli maps the same b0): CSM
// atlas or the VSM clipmap, single tap, no receiver biases beyond the normal offset towards the
// sun and no contact shadows -- a volume has no surface.
//
// b0 = the lighting cbuffer (lighting_cb.hlsli, the same allocation the lighting pass reads)
// b1 = FogCB (fog_common.hlsli)
// t0 ShadowAtlas  t1 VsmPageTable  t2 VsmPool  t3 SkyIrradiance (cube)  t4 FogHistory (3D, prev slot)
// u0 FogScatter (3D)
// s0 point clamp  s1 comparison linear  s2 linear wrap  s3 linear clamp (the lighting pass's set)
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "vsm_sample.hlsli"
#include "csm_sample.hlsli"
#include "lighting_cb.hlsli"
#include "fog_common.hlsli"

#define FOG_SCATTER_RS \
    "CBV(b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE))"

FOG_CB(b1)

Texture2D               ShadowAtlas   : register(t0);
StructuredBuffer<uint>  VsmPageTable  : register(t1);
Texture2D               VsmPool       : register(t2);
TextureCube             SkyIrradiance : register(t3);
Texture3D<float4>       FogHistory    : register(t4);
RWTexture3D<float4>     FogScatter    : register(u0);

SamplerState            gSmpPoint       : register(s0);
SamplerComparisonState  gSmpLinear      : register(s1);
SamplerState            gSmpLinearWrap  : register(s2);
SamplerState            gSmpLinearClamp : register(s3);

static const uint kFogHistoryValid = 1u;

// Sun visibility at a point IN THE AIR: the lighting pass's CSM / VSM path, minus everything that
// belongs to a receiver surface (receiver-plane bias, contact shadows, SMRT). N = the direction
// to the sun and ndl = 1 make the normal-offset terms a small lift towards the light -- harmless
// in a volume, and it keeps one sampler for both consumers.
float FogSunShadow(float3 P)
{
    const float3 dirToLight = normalize(-sunDirWS);
    if (useVsm != 0u)
    {
        VsmSmrtParams smrt = (VsmSmrtParams)0; // rayCount 0 = the single-tap SampleCmp path
        return VsmClipmapShadow(P, dirToLight, camPosWS, clipmapNormalBias, vsmDepthBias,
                                clipmapDepthBiasDecay, clipmapDepthBiasFloorNdc, clipmapBlendWidth,
                                invProj._11, clipmapUvNormal, dirToLight, smrt, 0.0f,
                                clipmapViewProj, VsmPageTable, VsmPool, gSmpLinear);
    }
    int cascade;
    return CsmSampleShadow(MakeCsmParams(), ShadowAtlas, gSmpLinear, gSmpPoint, P, dirToLight, 1.0f, cascade);
}

[numthreads(4, 4, 4)]
[RootSignature(FOG_SCATTER_RS)]
void CSMain(uint3 coord : SV_DispatchThreadID)
{
    if (any(coord >= fogGridSize)) { return; }

    // The sample point inside the cell: UE's per-frame Halton offset (0.5 = the centre when the
    // jitter is off).
    float viewDepth;
    const float3 P = FogCellWorldPosition(coord, fogJitter.xyz, fogGridSize, fogGridZParams,
                                          fogProjZ, fogInvViewProjNoJitter, viewDepth);
    const float3 toCell = P - camPosWS;
    const float dist = length(toCell);
    const float3 V = dist > 1.0e-4f ? toCell / dist : float3(0.0f, 0.0f, 1.0f); // camera -> cell

    // The medium (UE MaterialSetupCS): extinction from the analytic height fog at this height,
    // scattering = albedo * extinction.
    const float sigma = FogExtinctionAt(P.y, fogMedium0, fogMedium1.y);
    const float3 scattering = fogMedium1.x * sigma;
    const float g = fogMedium1.z;

    float3 L = 0.0f.xxx;
    // Sun: colour * shadow * phase (UE :888, PhaseFunction(g, dot(L, -CameraVector)) with L TO the
    // light and CameraVector = camera -> cell). Their HenyeyGreensteinPhase is written with
    // +2g*cos (ParticipatingMediaCommon.ush:96), so their forward peak sits at cos = -1; ours is
    // the textbook -2g*cos form (peak at +1), hence the argument is negated: dot(toSun, V) is +1
    // when the camera looks INTO the sun, which is where forward scattering (g > 0) must peak.
    if (fogMedium1.w > 0.0f)
    {
        const float3 toSun = normalize(-sunDirWS);
        const float shadow = FogSunShadow(P);
        L += lightRgb * fogMedium1.w * shadow * FogPhaseHG(g, dot(toSun, V));
    }
    // Sky: UE evaluate the skylight SH at -V * g; the irradiance cube in the direction the air
    // is looked THROUGH is the same term without the SH's directional damping (delta, plan §3).
    if (fogMedium2.x > 0.0f && skyIrradianceEnabled != 0u)
    {
        L += SkyIrradiance.SampleLevel(gSmpLinearClamp, -V, 0).rgb * skyIrradianceScale * fogMedium2.x;
    }

    float4 result = float4(fogMedium2.y * L * scattering, sigma); // pre-exposed, UE :1024

    // Temporal history (UE :1027-1033): the previous slot's volume at this cell's position under
    // last frame's camera, re-exposed to this frame; off screen -> no history.
    if ((fogFlags & kFogHistoryValid) != 0u && fogJitter.w > 0.0f)
    {
        const float3 Pc = FogCellWorldPosition(coord, 0.5f.xxx, fogGridSize, fogGridZParams,
                                               fogProjZ, fogInvViewProjNoJitter);
        const float4 prevClip = mul(float4(Pc, 1.0f), fogPrevViewProjNoJitter);
        if (prevClip.w > 1.0e-4f)
        {
            const float2 prevUv = (prevClip.xy / prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
            const float3 historyUv = FogVolumeUV(prevUv, prevClip.w, fogGridZParams, fogGridSize.z);
            if (all(historyUv.xy >= 0.0f) && all(historyUv.xy < 1.0f))
            {
                float4 history = FogHistory.SampleLevel(gSmpLinearClamp, historyUv, 0);
                history.rgb *= fogMedium2.z * fogMedium2.y; // previous pre-exposure -> this frame's
                result = lerp(result, history, fogJitter.w);
            }
        }
    }

    // UE MakePositiveFinite: a NaN in one cell would smear through the integration and the history.
    result = max(result, 0.0f.xxxx);
    if (!all(isfinite(result))) { result = 0.0f.xxxx; }
    FogScatter[coord] = result;
}
