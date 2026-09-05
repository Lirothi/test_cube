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
// t5 HzbFurthest (this frame)  t6 PrevHzbFurthest (last frame) -- conservative depth (plan A3)
// t7 SpotLights  t8 PointLights  t9 SpotShadowAtlas  t10 PointShadowCube -- local lights (plan A4)
// u0 FogScatter (3D)
// s0 point clamp  s1 comparison linear  s2 linear wrap  s3 linear clamp (the lighting pass's set)
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "vsm_sample.hlsli"
#include "csm_sample.hlsli"
#include "lighting_cb.hlsli"
#include "fog_common.hlsli"
#include "rt_lights.hlsli" // SpotLightData / PointLightData (the light passes' layout)

#define FOG_SCATTER_RS \
    "CBV(b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=11, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE))"

FOG_CB(b1)

Texture2D               ShadowAtlas   : register(t0);
StructuredBuffer<uint>  VsmPageTable  : register(t1);
Texture2D               VsmPool       : register(t2);
TextureCube             SkyIrradiance : register(t3);
Texture3D<float4>       FogHistory    : register(t4);
Texture2D<float>        HzbFurthest   : register(t5); // this frame's furthest depth pyramid (min reverse-Z)
Texture2D<float>        PrevHzbFurthest : register(t6); // last frame's (the history's conservative depth)
StructuredBuffer<SpotLightData>  SpotLights      : register(t7);  // plan A4: the light passes' buffers
StructuredBuffer<PointLightData> PointLights     : register(t8);
Texture2DArray                   SpotShadowAtlas : register(t9);  // Legacy local shadows
TextureCubeArray                 PointShadowCube : register(t10);
RWTexture3D<float4>     FogScatter    : register(u0);

SamplerState            gSmpPoint       : register(s0);
SamplerComparisonState  gSmpLinear      : register(s1);
SamplerState            gSmpLinearWrap  : register(s2);
SamplerState            gSmpLinearClamp : register(s3);

static const uint kFogHistoryValid = 1u;
static const uint kFogJitterOn = 2u;
static const uint kFogConservativeDepth = 4u;
static const uint kFogTemporalOn = 8u;
// R3 low-discrepancy sequence (Roberts): the extra cell offsets of a history-miss supersample.
static const float3 kFogR3 = float3(0.8191725134f, 0.6710436067f, 0.5497004779f);

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

// The light passes' LightDistanceAttenuation (utils.hlsli) with UE's volumetric distance bias in
// place of its fixed +1: the same smooth range window, 1 / (d^2 + bias^2) inside it.
float FogLocalDistanceAttenuation(float dist, float range, float distanceBiasSqr)
{
    const float invRange = (range > 1e-6f) ? (1.0f / range) : 0.0f;
    const float t = saturate(dist * dist * invRange * invRange);
    const float w = saturate(1.0f - t * t);
    return (w * w) / (dist * dist + distanceBiasSqr);
}

// Local-light shadows at a point IN THE AIR (plan A4): the light passes' samplers minus the
// receiver-normal terms (a volume has no surface). The sample point is pushed ALONG the light ray
// by two shadow texels at the camera-selected VSM level (the light passes' slope-scaled depth push
// with the slope at 1), which is what keeps a cell next to a caster from shadowing itself.
float FogSpotShadow(SpotLightData light, float3 P, float3 toL, float distToLight)
{
    if (light.shadowParams.y < 0.0f) { return 1.0f; } // no shadow slot this frame
    if (fogLocal.z != 0u)
    {
        const uint  lvl        = VsmSelectLevel(length(P - camPosWS), fogLocalParams.w, VSM_MAX_LEVEL);
        const float cosOuter   = max(light.directionCosOuter.w, 1e-3f);
        const float tanOuter   = sqrt(saturate(1.0f - cosOuter * cosOuter)) / cosOuter;
        const float texelWorld = (2.0f * distToLight * tanOuter / VSM_VIRTUAL_RES) * exp2((float)lvl);
        const float3 Pv = P + toL * (texelWorld * 2.0f);
        return VsmSpotShadow((uint)light.shadowParams.y, light.viewProj, Pv, camPosWS, fogLocalParams.w, 0.0f,
                             VsmPageTable, VsmPool, gSmpLinear);
    }
    const float4 clip = mul(float4(P, 1.0f), light.viewProj);
    const float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    if (any(uv < 0.0f) || any(uv > 1.0f) || ndc.z <= 0.0f || ndc.z >= 1.0f) { return 1.0f; }
    return SpotShadowAtlas.SampleCmpLevelZero(gSmpLinear, float3(uv, light.shadowParams.y), ndc.z - light.shadowParams.w);
}

float FogPointShadow(PointLightData light, float3 P, float3 toL, float distToLight)
{
    if (light.shadowParams.x < 0.0f) { return 1.0f; }
    if (fogLocal.z != 0u)
    {
        const uint  lvl        = VsmSelectLevel(length(P - camPosWS), fogLocalParams.w, VSM_MAX_LEVEL);
        const float texelWorld = (2.0f * distToLight / VSM_VIRTUAL_RES) * exp2((float)lvl);
        const float3 Pv = P + toL * (texelWorld * 2.0f);
        return VsmPointShadow((uint)light.shadowParams.x, Pv, light.position, light.shadowParams.z,
                              light.shadowParams.w, camPosWS, fogLocalParams.w, 0.0f,
                              VsmPageTable, VsmPool, gSmpLinear);
    }
    // pointlight_cs.hlsl's cube compare: the face's view-space depth with a WORLD-space bias.
    const float3 d = P - light.position;
    const float m = max(abs(d.x), max(abs(d.y), abs(d.z)));
    const float nearP = light.shadowParams.z;
    const float farP = max(light.shadowParams.w, nearP + 1e-3f);
    const float mBiased = max(m - light.shadowParams.y, nearP);
    const float zc = (farP / (farP - nearP)) * (1.0f - nearP / mBiased);
    return PointShadowCube.SampleCmpLevelZero(gSmpLinear, float4(d, light.shadowParams.x), zc);
}

// One lighting sample of a cell at `cellOffset` inside it: (preExposure * L * scattering, sigma).
float4 FogSampleCell(uint3 coord, float3 cellOffset)
{
    float viewDepth;
    const float3 P = FogCellWorldPosition(coord, cellOffset, fogGridSize, fogGridZParams,
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
    // Local lights (plan A4, UE :944-1030): every spot / point whose range reaches the cell, with
    // its shadow map. No light grid: the scene has tens of lights, not thousands, and the range
    // test is the cull (a cell outside every range costs the two loops' distance tests). The +1 in
    // LightDistanceAttenuation's denominator is the singularity bias next to the source (UE
    // InverseSquaredLightDistanceBiasScale); the cone's soft edge is the light passes' squared
    // ramp (UE additionally soften it by the cell radius, LightSoftFading -- not done). Literal
    // loop bounds; the counts only shorten them.
    if (fogLocalParams.x > 0.0f)
    {
        // UE :951-956: the cell radii (to the diagonal neighbour in 3D and within the slice) size the
        // cone soft edge (LightSoftFading * the 2D radius) and the inverse-square bias
        // (max(radius * scale, 1)^2 added to d^2), so a cell straddling the cone edge or containing
        // the source is a blend, not a coin flip -- the stepped, flickering edge the user saw.
        const float3 Pn3 = FogCellWorldPosition(coord + uint3(1u, 1u, 1u), cellOffset, fogGridSize, fogGridZParams, fogProjZ, fogInvViewProjNoJitter);
        const float3 Pn2 = FogCellWorldPosition(coord + uint3(1u, 1u, 0u), cellOffset, fogGridSize, fogGridZParams, fogProjZ, fogInvViewProjNoJitter);
        const float cellRadius = length(P - Pn3);
        const float softFadeDistance = fogLocalParams2.x * length(P - Pn2);
        float distanceBiasSqr = max(cellRadius * fogLocalParams2.y, 1.0f);
        distanceBiasSqr *= distanceBiasSqr;
        [loop]
        for (uint si = 0u; si < 256u; ++si)
        {
            if (si >= fogLocal.x) { break; }
            const SpotLightData light = SpotLights[si];
            const float3 Lvec = light.positionRange.xyz - P;
            const float d = length(Lvec);
            if (d >= light.positionRange.w || light.positionRange.w <= 1e-6f) { continue; }
            const float3 Ldir = Lvec / max(d, 1e-6f);
            const float spotCos = dot(-Ldir, light.directionCosOuter.xyz);
            if (spotCos <= light.directionCosOuter.w) { continue; }
            float angleAtten = saturate((spotCos - light.directionCosOuter.w) * light.shadowParams.z);
            angleAtten *= angleAtten;
            // UE GetSpotLightVolumetricSoftFading (DeferredLightingCommon.ush:583): how far inside the
            // cone aperture the cell sits, faded over the soft distance.
            float softFade = 1.0f;
            if (softFadeDistance > 0.0f)
            {
                const float cosOuter = light.directionCosOuter.w;
                const float tanOuter = sqrt(saturate(1.0f - cosOuter * cosOuter)) / max(cosOuter, 1e-3f);
                const float3 toCell = -Lvec; // light -> cell
                const float along = dot(light.directionCosOuter.xyz, toCell);
                const float aperture = along * tanOuter;
                const float fromAxis = length(toCell - light.directionCosOuter.xyz * along);
                softFade = saturate((aperture - fromAxis) / softFadeDistance);
            }
            const float atten = FogLocalDistanceAttenuation(d, light.positionRange.w, distanceBiasSqr) * angleAtten * softFade;
            const float shadow = FogSpotShadow(light, P, Ldir, d);
            L += light.colorIntensity.xyz * light.colorIntensity.w * atten * shadow * fogLocalParams.x
                 * FogPhaseHG(g, dot(Ldir, V));
        }
        [loop]
        for (uint pi = 0u; pi < 256u; ++pi)
        {
            if (pi >= fogLocal.y) { break; }
            const PointLightData light = PointLights[pi];
            const float3 Lvec = light.position - P;
            const float d = length(Lvec);
            if (d >= light.radius || light.radius <= 1e-6f) { continue; }
            const float3 Ldir = Lvec / max(d, 1e-6f);
            const float atten = FogLocalDistanceAttenuation(d, light.radius, distanceBiasSqr);
            const float shadow = FogPointShadow(light, P, Ldir, d);
            L += light.color * light.intensity * atten * shadow * fogLocalParams.x * FogPhaseHG(g, dot(Ldir, V));
        }
    }
    return float4(fogMedium2.y * L * scattering, sigma); // pre-exposed, UE :1024
}

[numthreads(4, 4, 4)]
[RootSignature(FOG_SCATTER_RS)]
void CSMain(uint3 coord : SV_DispatchThreadID)
{
    if (any(coord >= fogGridSize)) { return; }

    // Conservative depth (UE :785-800): the furthest pyramid's texel over this cell's 16x16 pixels is
    // the FARTHEST surface any of them sees (min reverse-Z). If even that is in front of the cell's
    // near face -- half a voxel towards the camera, so a bilinear read of the cell still lands on
    // lit data -- nothing on screen looks through this cell: it stores nothing and the sampling
    // (the shadow lookups above all) is skipped. Beyond the pyramid (a grid column past its last
    // texel) Load returns 0 = the far plane, which never culls.
    const bool conservative = (fogFlags & kFogConservativeDepth) != 0u;
    if (conservative)
    {
        const float tileFar = HzbFurthest.Load(int3(coord.xy, fogMisc.x)).r;
        const float nearFaceDepth = FogDepthFromSlice(fogGridZParams, max((float)coord.z - 0.5f, 0.0f));
        const float cellNearZ = FogDeviceZFromViewDepth(fogProjZ, nearFaceDepth);
        if (tileFar > cellNearZ)
        {
            FogScatter[coord] = 0.0f.xxxx;
            return;
        }
    }

    // Temporal history (UE :1027-1033): the previous slot's volume at this cell's position under
    // last frame's camera, re-exposed to this frame; off screen -> no history. With conservative
    // depth the history cell must also have been in FRONT of last frame's geometry (UE
    // FixupHistoryUV, :699-765, reduced to the one tap the grid is read at): a cell that was
    // behind a surface last frame stored nothing and must not be blended from.
    float historyWeight = 0.0f;
    float3 historyUv = 0.0f.xxx;
    if ((fogFlags & kFogHistoryValid) != 0u && fogJitter.w > 0.0f)
    {
        const float3 Pc = FogCellWorldPosition(coord, 0.5f.xxx, fogGridSize, fogGridZParams,
                                               fogProjZ, fogInvViewProjNoJitter);
        const float4 prevClip = mul(float4(Pc, 1.0f), fogPrevViewProjNoJitter);
        if (prevClip.w > 1.0e-4f)
        {
            const float2 prevUv = (prevClip.xy / prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
            historyUv = FogVolumeUV(prevUv, prevClip.w, fogGridZParams, fogGridSize.z);
            if (all(historyUv.xy >= 0.0f) && all(historyUv.xy < 1.0f))
            {
                historyWeight = fogJitter.w;
                if (conservative)
                {
                    const float3 Pn = FogCellWorldPosition(coord, float3(0.5f, 0.5f, -0.5f), fogGridSize,
                                                           fogGridZParams, fogProjZ, fogInvViewProjNoJitter);
                    const float4 prevNearClip = mul(float4(Pn, 1.0f), fogPrevViewProjNoJitter);
                    const float prevFar = PrevHzbFurthest.Load(int3((int2)(historyUv.xy * (float2)fogGridSize.xy), fogMisc.x)).r;
                    if (prevNearClip.w > 1.0e-4f && prevFar > prevNearClip.z / prevNearClip.w)
                    {
                        historyWeight = 0.0f;
                    }
                }
            }
        }
    }

    // History-miss supersampling (UE :822-829, HistoryMissSupersampleCount): a cell that has nothing
    // to accumulate into takes several jittered samples at once, so the first frame after a cut or
    // the frame's newly uncovered edge is not a single noisy sample. Literal loop bound, the count
    // from the CB only shortens it (engine rule).
    const bool temporalOn = (fogFlags & kFogTemporalOn) != 0u;
    const uint superCount = (temporalOn && historyWeight <= 0.001f) ? max(fogMisc.y, 1u) : 1u;
    float4 result = 0.0f.xxxx;
    [loop]
    for (uint si = 0u; si < 4u; ++si)
    {
        if (si >= superCount) { break; }
        const float3 cellOffset = (si == 0u) ? fogJitter.xyz : frac(fogJitter.xyz + kFogR3 * (float)si);
        result += FogSampleCell(coord, cellOffset);
    }
    result /= (float)superCount;

    if (historyWeight > 0.0f)
    {
        float4 history = FogHistory.SampleLevel(gSmpLinearClamp, historyUv, 0);
        history.rgb *= fogMedium2.z * fogMedium2.y; // previous pre-exposure -> this frame's
        result = lerp(result, history, historyWeight);
    }

    // UE MakePositiveFinite: a NaN in one cell would smear through the integration and the history.
    result = max(result, 0.0f.xxxx);
    if (!all(isfinite(result))) { result = 0.0f.xxxx; }
    FogScatter[coord] = result;
}
