#pragma once

#include <cstdint>

#include "core/math/Math.h"
#include "core/threading/Knob.h"

// Moved from app/scene/SceneFrameData.h (architecture plan R1.5): the settings, their knobs, the packing.

// P7 -- global analytic aerial perspective. SHIPS DISABLED: it is a real image change, and this
// plan's own rule (the one GTAO and the P3B local exposure already follow) is that such a change
// earns its default with an explicit A/B rather than arriving switched on. With `enabled` false the
// density reaching the shader is 0 and compose skips the whole block, which is the interface
// contract's "screenshot-equivalent to M2".
struct HeightFogSettings
{
    bool enabled = false;
    // Extinction per world unit AT `referenceHeight`. The defaults below are a starting point for
    // tuning on the atoll, NOT a measured result -- they have never been looked at with fog on.
    float density = 0.004f;
    // e-folding rate with world height. 0 makes the medium height-invariant (uniform distance fog).
    float heightFalloff = 0.02f;
    float referenceHeight = 0.0f;   // sea level in this project's scenes
    float startDistance = 25.0f;    // fog-free air, so near beach contrast survives
    float maxOpacity = 0.9f;        // distance never fully flattens shape
    float sunScatterStrength = 0.35f;
    // UE's DirectionalInscatteringExponent default, which is dimensionless and so transfers
    // unchanged (their density/falloff do not -- see the units note in height_fog.hlsli).
    float sunScatterExponent = 4.0f;
    // UE keep the sun lobe out of the near field with a distance of its own
    // (DirectionalInscatteringStartDistance). Theirs is 10000 in a centimetre world.
    float sunScatterStartDistance = 100.0f;
    // How blurred the sky is when it is read as the FOG'S COLOUR, as a roughness fed to the same
    // IblSkyRadiance everything else uses. Only the LIGHTLY fogged end is blurred by it -- see
    // HeightFogSkyRoughness in height_fog.hlsli. 0 restores the original mip-0 read, which prints
    // the clouds onto whatever stands in front of them.
    float skyBlur = 0.5f;
    // The phase function, as "how bright the haze is with the sun BEHIND you" relative to looking
    // into it. Ships at 1.0 = flat = the pre-phase image, because it is a real look change and this
    // plan's rule is that those earn their default with an A/B. Below 1 the same air glows backlit
    // and stays dim front-lit, which is what real haze does -- and it is the RIGHT knob for that,
    // because a directional DENSITY would change extinction and make distant shapes fade in and out
    // as the camera pans.
    // WHERE the sky sample stops being directional. UE blend their fog cubemap from the top mip
    // (NonDirectionalColor, the sphere average) into mip 0 (DirectionalColor) across
    // [NonDirectionalInscatteringColorDistance, FullyDirectionalInscatteringColorDistance]
    // (HeightFogCommon.ush:148-171, FogRendering.cpp:273,284). Their defaults are 1000 and 100000
    // in a centimetre world = 10 m and 1000 m, which transfer unchanged because they are lengths.
    //
    // SHIPS OFF (both 0 => the fade is a no-op and the sample is always directional), and it has to,
    // because their structure is not ours. UE MULTIPLY the cubemap into an authored fog colour
    // (Inscattering = FogColor * lerp(NonDir, Dir, t)); we feed the sample straight into
    // HeightFogColor as the base. So their safe near-field average -- the whole sphere,
    // sun included -- becomes an injection of energy here, and with UE's own 10 m / 1000 m the open
    // water inside a kilometre went milk-white. Measured on the owner's HDRI camera, 2026-09-09.
    //
    // The knob stays because the reasoning behind it is sound: the fog's ray points at the SURFACE,
    // so up close it points steeply DOWN, where "the sky in that direction" means little. But the
    // near-field blur we already had (HeightFogSkyRoughness, bounded by HeightFogClampSkySample)
    // covers that, and turning this on is a look change that has to earn its default with an A/B --
    // the same rule every look change here ships under. It is NOT what removed the horizon band: a control
    // run with the fade off showed the band gone either way. That was the cube, not this.
    float nonDirectionalDistance = 0.0f;
    float fullyDirectionalDistance = 0.0f;
    // THE FOG'S COLOUR UNDER THE PROCEDURAL SKY IS UE 5.8's (2026-10-31, owner after a side-by-side with UE on
    // wind_test: "do it as in UE"). ComputeInscatteringColor (HeightFogCommon.ush:154-198) = FogInscatteringLuminance
    // + SkyAtmosphereAmbientContributionColorScale x the atmosphere's distant sky light (GetViewDistanceSkyLightColor,
    // SkyAtmosphereCommon.ush:356): ONE isotropic colour for every direction, on geometry and on the sky alike, so the
    // air reads as a veil over the horizon instead of converging on whatever sky stands behind each pixel (the sky
    // along the view, which made a fogged sky an identity: -1 / -3 / -8 % at the horizon where UE's frame has -9 /
    // -27 / -35 % at densities 0.002 / 0.01 / 0.04). The HDRI sky keeps the sky along the view: there is no distant
    // light, and that is UE's InscatteringColorCubemap option. cd/m2, absolute; UE's defaults (black, white).
    float3 inscatteringLuminance{0.0f, 0.0f, 0.0f};
    float3 skyAmbientContribution{1.0f, 1.0f, 1.0f};
    // UE's DirectionalInscatteringLuminance is a COLOUR; `sunScatterStrength` is its grey and this its tint (white =
    // the grey alone). Ultra Dynamic Sky's is warm by day, (0.79, 0.64, 0.56), reddening to 0 as the sun sets
    // (timeOfDay.fog's UDS curves).
    float3 sunScatterColor{1.0f, 1.0f, 1.0f};

    // ---- FRAME-ONLY, never serialized (HeightFogSettingsJson does not know them): the scene writes them on the FRAME's
    // copy every frame, beside the density's time-of-day scale (Scene::Tick), for PackHeightFog.
    // UE's SkyAtmosphere HeightFogContribution (SkyAtmosphereSettings::heightFogContribution) x timeOfDay.fog's UDS
    // curve: it scales the sky ambient in the fog's colour and the ground-illuminance half of the sun lobe.
    float frameSkyContribution = 1.0f;
    // timeOfDay.fog's UDS directional-inscattering curve, multiplying sunScatterStrength x sunScatterColor.
    float3 frameSunScatterTint{1.0f, 1.0f, 1.0f};
    // timeOfDay.fog's UDS twilight floor, cd/m2, added to inscatteringLuminance.
    float3 frameInscatterFloor{0.0f, 0.0f, 0.0f};

    // ---- Volumetric (froxel) fog, docs/volumetric_fog_sky_clouds_ssgi_plan.md part A. UE's
    // VolumetricFog on top of the analytic model above: inside `volumetricDistance` the air is a
    // lit, shadowed volume (light shafts through the palms); beyond it the analytic fog continues.
    // Off = the pre-plan image exactly (no pass, no volume). Defaults are UE's where dimensionless.
    bool volumetric = false;
    // m: the froxel volume's far plane (UE VolumetricFogDistance). NOT UE's 60 m, measured 2026-10-26: beyond the
    // far plane the analytic model fogs the SKY with the sky itself (an identity, compose_cs), where UE's authored fog
    // colour keeps hazing it -- so this distance is how much air stands between the camera and the sky. At 60 m a
    // dense fog (density 0.02 on wind_test's grove) left the sky clear behind 60 m of haze; at the level's 0.0015 the
    // two are the same picture (mean +0.2 of 255, the noise). UE's 60 would give the near field 5x thinner slices
    // (the first 0.44 m deep instead of 2.2) for +0.02 ms.
    float volumetricDistance = 300.0f;
    // UE VolumetricFogStartDistance (default 0): the volume's own near plane, NOT the analytic `startDistance` --
    // that one only keeps the analytic model off the near field, which the volume already excludes up to its far
    // plane (HeightFogCommon.ush:449, :288). Tied together, wind_test's 10 m start emptied a 4 m room of air: no
    // candle glow, no shafts nearer than 10 m, and the 64 slices spread over 10-300 m instead of 0-300.
    float volumetricStartDistance = 0.0f;
    // UE VolumetricFogNearFadeInDistance (default 0): the volume fades in over this much of every ray from its start
    // (VolumetricFog.usf FinalIntegrationCS, the accumulated step length x 1 / this).
    float volumetricNearFadeInDistance = 0.0f;
    float albedo = 1.0f;                // UE VolumetricFogAlbedo: scattering / extinction (white)
    float extinctionScale = 1.0f;       // UE VolumetricFogExtinctionScale
    float phaseG = 0.2f;                // UE VolumetricFogScatteringDistribution (Henyey-Greenstein g)
    float sunScatter = 1.0f;            // UE DirectionalLight VolumetricScatteringIntensity
    float skyScatter = 1.0f;            // UE SkyLight VolumetricScatteringIntensity
    float historyWeight = 0.9f;         // UE r.VolumetricFog.HistoryWeight
    bool temporal = true;               // UE r.VolumetricFog.TemporalReprojection (+ jitter)
    bool jitter = true;                 // UE r.VolumetricFog.Jitter (only with the history, else the cell centre)
    // Jittered samples per cell EVERY frame (1-4; UE take one and lean on TAA). 2 = the antithetic pair, which
    // cancels a linear gradient across the cell and stops a spot's tip flickering after the 0.9 history.
    int samplesPerCell = 2;
    bool conservativeDepth = true;      // UE r.VolumetricFog.ConservativeDepth: cells behind the tile's farthest surface are skipped
    bool localLights = true;            // spots and points light the volume (UE: every local light with VolumetricScatteringIntensity > 0)
    float localLightScatter = 1.0f;     // UE per-light VolumetricScatteringIntensity, one value for all local lights here
    // UE r.VolumetricFog.LightSoftFading: a spot cone edge fades over this many froxels (their default 0 = hard;
    // 1 is their own good starting point and the cure for a stepped, flickering cone edge -- ours ships at 1).
    float localSoftFading = 1.0f;
    // UE r.VolumetricFog.InverseSquaredLightDistanceBiasScale: the inverse-square falloff is biased by the cell
    // radius times this, so a cell that contains the source does not blow up (aliasing at the light).
    float localDistanceBias = 1.0f;
};

// P7 item 8. Deliberately NOT part of HeightFogSettings: that struct is serialized into the level,
// and a debug view saved into a level is a trap -- the same reasoning that keeps
// ocean::g_foamDebugView out of OceanRenderConfig. 0 = normal, 1 = transmittance, 2 = in-scattering.
inline uint32_t g_fogDebugView = 0u;
inline uint32_t g_fogDebugView_game = 0u;
inline const bool g_fogDebugView_knob = ::knob::Register(g_fogDebugView_game, g_fogDebugView, "g_fogDebugView");
// The atmosphere's distant sky light for this frame's fog colour (HeightFogSettings::skyAmbientContribution): rgb =
// SkyAtmosphere's DistantSkyLight read back to the CPU, times UE's SkyLuminanceFactor (UE puts it in that LUT,
// SkyAtmosphere.usf:1466; ours leaves it out of the light), raw cd/m2; w = 1 while the procedural sky has one. Set by
// the scene renderer once a frame before any pass packs the fog. A few frames old: it is rebuilt only as the sun moves.
inline float4 g_fogDistantSkyLight{};
// A/B, transient (--set fog.skyAlongView:1): the fog coloured by the sky along the view again, as before 2026-10-31.
inline bool g_fogSkyAlongView = false;
inline bool g_fogSkyAlongView_game = false;
inline const bool g_fogSkyAlongView_knob = ::knob::Register(g_fogSkyAlongView_game, g_fogSkyAlongView, "g_fogSkyAlongView");
// A/B, transient (--set fog.inSkyCapture:0): the analytic height fog stays out of the procedural sky's capture (the
// probes: the water's sky reflection, the sky fill), as before 2026-11-03. UE's ExponentialHeightFog
// bVisibleInRealTimeSkyCaptures, default true (ReflectionEnvironmentRealTimeCapture.cpp:919-962).
inline bool g_fogInSkyCapture = true;
inline bool g_fogInSkyCapture_game = true;
inline const bool g_fogInSkyCapture_knob = ::knob::Register(g_fogInSkyCapture_game, g_fogInSkyCapture, "g_fogInSkyCapture");

// The exact numbers the shaders receive. TWO passes apply aerial perspective -- compose, for opaque
// geometry, and the ocean's forward surface -- and the plan's own warning about this feature is that
// duplicated fog terms drift apart. So the packing lives here, once, and both callers use it.
// `hasSun` false zeroes the density: with no directional light there is nothing to colour the
// in-scattering with, and a fog that ignores that would tint the world with a stale sun.
struct HeightFogPacked
{
    float4 params0{}; // density, height falloff, reference height, start distance
    float4 params1{}; // max opacity, the sun lobe's ground-illuminance factor, sun scatter exponent, sun scatter start
    float4 params2{}; // sky blur, RESERVED (0; was the lobe's grey term), directional-fade inverse range, its bias
    float4 inscatter{}; // UE's fog colour (raw cd/m2), w = 1: use it on geometry and sky; w = 0: the sky along the view
    // rgb: UE's DirectionalInscatteringColor -- DirectionalInscatteringLuminance (colour) x the sun's outer-space
    // luminance -- / (4 pi), cd/m2: the lobe's own term beside the ground illuminance's (params1.y). w reserved.
    float4 sunDirectional{};
};

// `sunOuterLuminance`: the sun's outer-space illuminance as a luminance (DirectionalLight::GetOuterSpaceLuminance), 0
// without a sun.
inline HeightFogPacked PackHeightFog(const HeightFogSettings& a, bool hasSun, float sunOuterLuminance)
{
    const bool on = a.enabled && hasSun && a.density > 0.0f;
    HeightFogPacked p{};
    p.params0 = float4(on ? std::max(a.density, 0.0f) : 0.0f, std::max(a.heightFalloff, 0.0f),
                       a.referenceHeight, std::max(a.startDistance, 0.0f));
    // THE SUN LOBE IS UE's (2026-11-03, owner: "the same settings must work the same as in UE"), with a SkyAtmosphere
    // affecting the fog (HeightFogCommon.ush:328-340, FogRendering.cpp:431-438): (DirectionalInscatteringLuminance x the
    // sun's outer-space luminance + the SkyAtmosphere's HeightFogContribution x the sun's illuminance on the ground after
    // the atmosphere) x pow(V.L, exponent) x the uniform phase 1 / (4 pi). `sunScatterStrength` is that
    // DirectionalInscatteringLuminance (grey; UE's default black, 0.5 in the owner's UE wind_test) and the contribution
    // is UE's default 1. Ours was strength x the ground illuminance with no 1 / (4 pi) and no outer-space term: the same
    // 0.5 drew the lobe ~3-4x UE's. y of params1 = the ground illuminance's factor, y of params2 = the grey term in cd/m2;
    // the shader adds them over the sun colour its caller already has (height_fog.hlsli HeightFogColor).
    constexpr float kInv4Pi = 0.0795774715f;
    // UE's SkyAtmosphere HeightFogContribution (default 1; Ultra Dynamic Sky drives it by the sun's height, 2 by day):
    // the sky's light in the fog -- the ambient colour (:190) and the ground-illuminance half of the lobe (:334).
    const float skyContribution = std::max(a.frameSkyContribution, 0.0f);
    p.params1 = float4(std::clamp(a.maxOpacity, 0.0f, 1.0f), skyContribution * kInv4Pi,
                       std::max(a.sunScatterExponent, 1.0f), std::max(a.sunScatterStartDistance, 0.0f));
    // UE FogRendering.cpp:273,284: InvRange over the gap, and the bias that makes
    // saturate(rayLength * zw.x + zw.y) reach 0 at nonDirectional and 1 at fullyDirectional.
    const float nonDir = std::max(a.nonDirectionalDistance, 0.0f);
    const float invRange = 1.0f / std::max(a.fullyDirectionalDistance - nonDir, 1.0e-5f);
    // y carried `skyBackScatter` until the phase was found to be a dimmer (height_fog.hlsli), then the lobe's grey term,
    // which is the coloured `sunDirectional` since 2026-11-04 (UE's DirectionalInscatteringLuminance is a colour).
    p.params2 = float4(std::clamp(a.skyBlur, 0.0f, 1.0f), 0.0f, invRange, -nonDir * invRange);
    const float grey = std::max(a.sunScatterStrength, 0.0f) * std::max(sunOuterLuminance, 0.0f) * kInv4Pi;
    p.sunDirectional = float4(grey * std::max(a.sunScatterColor.x * a.frameSunScatterTint.x, 0.0f),
                              grey * std::max(a.sunScatterColor.y * a.frameSunScatterTint.y, 0.0f),
                              grey * std::max(a.sunScatterColor.z * a.frameSunScatterTint.z, 0.0f), 0.0f);
    if (g_fogDistantSkyLight.w > 0.0f && !g_fogSkyAlongView)
    {
        const float4& d = g_fogDistantSkyLight;
        const float3 own = a.inscatteringLuminance + a.frameInscatterFloor;
        p.inscatter = float4(std::max(own.x + skyContribution * a.skyAmbientContribution.x * d.x, 0.0f),
                             std::max(own.y + skyContribution * a.skyAmbientContribution.y * d.y, 0.0f),
                             std::max(own.z + skyContribution * a.skyAmbientContribution.z * d.z, 0.0f), 1.0f);
    }
    return p;
}
