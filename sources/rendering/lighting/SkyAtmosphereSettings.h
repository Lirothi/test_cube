#pragma once

#include <cmath>
#include "shaders/shared/shader_types.h"
#include "shaders/shared/sky_atmosphere.h" // SkyAtmosphereParameters, SkyViewFrameData: the fields, once (R5.3)
#include "shaders/shared/night_sky.h" // NightSkyFrameData's fields, once (R5.3)

// B1: kilometre-space atmosphere, separate from metre-space height/froxel fog.
// UE SkyAtmosphereComponent.cpp:94-128 (Earth defaults). No sun/view/exposure in this key.
struct SkyAtmosphereParameters
{
    // shaders/shared/sky_atmosphere.h (architecture plan R5.3): the fields, once for HLSL and C++.
    using float4 = shader_shared::FloatArray<4>; // rows the C++ indexes (float[4] before R5.3)
    SKY_ATMOSPHERE_FIELDS
};
static_assert(sizeof(SkyAtmosphereParameters) == 112, "SkyAtmosphereCB layout");

// THE SUN'S OWN TRANSMITTANCE, evaluated on the CPU once per frame, so the directional light dims
// and reddens as the sun sets instead of staying at its outer-space illuminance all the way down.
// Transcribed from UE's FAtmosphereSetup::GetTransmittanceAtGroundLevel
// (Engine/Public/Rendering/SkyAtmosphereCommonData.cpp:156-243), including the two details that are
// not obvious: the origin sits 500 m ABOVE the ground rather than on it, and the azimuth is thrown
// away, because transmittance is symmetric about the zenith axis and only the elevation matters.
//
// UE multiply this into the light's colour in every lighting path they have -- the forward light
// grid (LightGridInjection.cpp:1064), the simple directional light (SceneRendering.cpp:1477), Lumen
// and ray tracing -- and fall back to WHITE when the scene has no atmosphere
// (SkyAtmosphereRendering.cpp:610), which is exactly our HDRI mode.
//
// `minElevationDegrees` is their TransmittanceMinLightElevationAngle, whose default is -90, i.e. no
// clamp; it exists so an artist can keep a visible sun on meshes after it has gone below the horizon.
inline void SkyTransmittanceTowardSun(const SkyAtmosphereParameters& a, float sunElevationRadians,
                                      float outTransmittance[3], float minElevationDegrees = -90.0f)
{
    const float bottom = a.AtmosphereRadii[0], top = a.AtmosphereRadii[1];
    const float minEl = minElevationDegrees * 3.14159265358979323846f / 180.0f;
    const float elevation = sunElevationRadians > minEl ? sunElevationRadians : minEl;

    // Origin on the zenith axis, 500 m up; direction in the same plane, so the ray is (cos, sin).
    const float originHeight = bottom + 0.5f;
    const float dirUp = std::sin(elevation), dirOut = std::cos(elevation);

    // Distance to the top of the atmosphere: solve |origin + t*dir| = top with origin = (0, height).
    const float b = 2.0f * originHeight * dirUp;
    const float c = originHeight * originHeight - top * top;
    const float disc = b * b - 4.0f * c;
    float tMax = 0.0f;
    if (disc >= 0.0f)
    {
        const float s = std::sqrt(disc);
        const float t0 = 0.5f * (-b - s), t1 = 0.5f * (-b + s);
        tMax = t1 > 0.0f ? (t0 > 0.0f ? (t0 < t1 ? t0 : t1) : t1) : 0.0f;
    }

    // THE PLANET IS IN THE WAY below this origin's horizon (time-of-day plan T0): the ray to the sun
    // meets the ground before the top of the atmosphere, nothing gets through. Without the test the
    // march ran on INSIDE the planet, where heights go negative and densities explode -- an answer of
    // ~0 by accident, not by construction.
    {
        const float cBottom = originHeight * originHeight - bottom * bottom;
        const float discBottom = b * b - 4.0f * cBottom;
        if (discBottom >= 0.0f && 0.5f * (-b - std::sqrt(discBottom)) > 0.0f)
        {
            outTransmittance[0] = outTransmittance[1] = outTransmittance[2] = 0.0f;
            return;
        }
    }

    double opticalDepth[3] = {0.0, 0.0, 0.0};
    if (tMax > 0.0f)
    {
        constexpr int kSamples = 15; // UE's count, and it is coarse on purpose: this is per frame
        const float step = 1.0f / static_cast<float>(kSamples);
        const float segment = step * tMax;
        for (int i = 0; i < kSamples; ++i)
        {
            const float t = tMax * (static_cast<float>(i) * step);
            const float x = t * dirOut, y = originHeight + t * dirUp;
            const float height = std::sqrt(x * x + y * y) - bottom;

            const float densityMie = std::exp(a.AtmosphereRadii[3] * height);
            const float densityRay = std::exp(a.AtmosphereRadii[2] * height);
            const float ozoneRaw = height < a.AbsorptionExtinction[3] ? a.AbsorptionDensity[0] * height + a.AbsorptionDensity[1]
                                                       : a.AbsorptionDensity[2] * height + a.AbsorptionDensity[3];
            const float densityOzo = ozoneRaw < 0.0f ? 0.0f : (ozoneRaw > 1.0f ? 1.0f : ozoneRaw);
            for (int ch = 0; ch < 3; ++ch)
            {
                const float extinction = densityMie * (a.MieScattering[ch] + a.MieAbsorption[ch])
                                       + densityRay * a.RayleighScattering[ch]
                                       + densityOzo * a.AbsorptionExtinction[ch];
                opticalDepth[ch] += static_cast<double>(segment) * extinction;
            }
        }
    }
    for (int ch = 0; ch < 3; ++ch)
    {
        outTransmittance[ch] = static_cast<float>(std::exp(-opticalDepth[ch]));
    }
}

struct SkyAtmosphereSettings
{
    // 0 HDRI cubemap, 1 procedural atmosphere. AUTHORED BY THE LEVEL (the `skyAtmosphere` object),
    // session override: which sky a level uses is part of the level, the same as which cubemap it
    // names. `--set=sky.mode` still overrides it for a headless run.
    unsigned mode = 0;
    // UE SkyAndAerialPerspectiveLuminanceFactor: multiplies the sun's illuminance on the way INTO every
    // LUT (SkyView, the aerial-perspective volume, the distant sky light, the environment capture), so
    // it brightens the sky, the AP haze on geometry AND the ambient the probes deliver -- all together,
    // relative to the direct sun. NOT sky-only; that is `skyLuminanceFactor`. Never the camera exposure.
    float luminanceScale = 2.13f;
    // THE PICTURE OF THE SKY, and nothing the sky lights: applied at draw time to the sky pixel only
    // (skybox.hlsl). Deliberate deviation from UE's SkyLuminanceFactor, which also reaches their sky-light
    // capture and the distant sky light (usf:919-922, :1397): measured 2026-09-11, that made it
    // indistinguishable from `luminanceScale` (lit sand x1.11, palm crowns x1.37 under both), and the
    // owner wants a knob that brightens the sky he sees without touching the lighting. Not the disc either.
    float skyLuminanceFactor = 1.0f;
    // UE AerialPerspectiveStartDepth (SkyAtmosphereComponent.cpp:132, 0.1 km): view depth in METRES
    // before which the aerial-perspective volume adds nothing. Its own knob (part-B review): it used to
    // ride on the height fog's `volumetricDistance`, a subsystem that can be off while this is on.
    float aerialStartDepthMetres = 100.0f;
    // UE AerialPespectiveViewDistanceScale (SkyAtmosphereComponent.cpp:129, 1.0): multiplies the optical
    // depth per sample of the aerial-perspective volume (SkyAtmosphere.usf:601-606) -- how many metres of
    // Earth's air one metre of view distance counts as. 1 is Earth, which over a few hundred metres is
    // invisible (measured 0.8 % on the far water); artists set 5-20 for a haze that reads at island scale.
    float aerialViewDistanceScale = 1.0f;
    // UE USkyAtmosphereComponent::HeightFogContribution (default 1): the sky's light IN THE HEIGHT FOG -- the fog's
    // sky-ambient colour (the distant sky light, HeightFogCommon.ush:190) and the ground-illuminance half of its sun lobe
    // (:334) -- not the sky itself. Ultra Dynamic Sky drives it by the sun's height (2 by day, ~1.2 at sunset; ours:
    // timeOfDay.fog's UDS curves multiply this).
    float heightFogContribution = 1.0f;
    // THE MEDIUM'S COLOURS, per channel, over the x Earth scales (the level's rayleighScale / mieScale / ozoneScale): UE's
    // RayleighScattering / MieScattering / OtherAbsorption colours relative to their defaults, so a UE sky ports
    // exactly. (1, 1, 1) = Earth's colour. Kept beside `parameters` (which hold the products) for --set.
    float rayleighTint[3] = {1.0f, 1.0f, 1.0f};
    float mieTint[3] = {1.0f, 1.0f, 1.0f};
    float ozoneTint[3] = {1.0f, 1.0f, 1.0f};
    // Mie ABSORPTION x Earth's (UE MieAbsorptionScale / 0.000444). < 0 = the Mie scale's, which `mieScale` has always
    // moved together with the scattering.
    float mieAbsorptionScale = -1.0f;
    bool environmentLighting = false; // B4: session opt-in until visual acceptance
    bool aerialPerspective = false; // B3: procedural mode only; enable after visual acceptance
    unsigned aerialDebugView = 0; // 0 scene, 1 transmittance, 2 luminance, 3 depth slices
    bool lutValidate = false; // GPU readback + double-precision UE reference on parameter changes
    bool lutEnabled = false; // Explicit LUT calculation even in HDRI mode.
    unsigned lutDebugView = 0; // compose: 1 transmittance, 2 multi-scattering (display gain 10).
    SkyAtmosphereParameters parameters{};
};

// The medium's coefficients from the x Earth scales and the tints (SkyAtmosphereSettings): the ONE place they are
// multiplied out -- the level reader and --set both come here, so a scale set live keeps the level's colours.
inline void SetSkyMedium(SkyAtmosphereSettings& s, float rayleighScale, float mieScale, float ozoneScale)
{
    const SkyAtmosphereParameters earth{};
    const float absorption = s.mieAbsorptionScale >= 0.0f ? s.mieAbsorptionScale : mieScale;
    for (unsigned i = 0; i < 3; ++i)
    {
        s.parameters.RayleighScattering[i] = earth.RayleighScattering[i] * rayleighScale * s.rayleighTint[i];
        s.parameters.MieScattering[i] = earth.MieScattering[i] * mieScale * s.mieTint[i];
        s.parameters.MieAbsorption[i] = earth.MieAbsorption[i] * absorption;
        s.parameters.AbsorptionExtinction[i] = earth.AbsorptionExtinction[i] * ozoneScale * s.ozoneTint[i];
    }
}

// The scale a coefficient carries over Earth's x its tint, from the channel whose tint is largest (never 0).
inline float SkyMediumScaleOf(const float authored[4], const float earth[4], const float tint[3])
{
    unsigned c = 0;
    for (unsigned i = 1; i < 3; ++i) { if (tint[i] > tint[c]) c = i; }
    return (earth[c] > 0.0f && tint[c] > 0.0f) ? authored[c] / (earth[c] * tint[c]) : 1.0f;
}

// ULTRA DYNAMIC SKY'S SKY MATERIAL over the atmosphere, this frame (TimeOfDaySettings::skyUdsMaterial; uds_sky.hlsli
// UdsSkyLayer). Read off its Ultra_Dynamic_Sky_Mat node by node and checked against UE's bare SkyAtmosphere with the same
// numbers (2026-11-04: UDS / bare = Overall Intensity x Lit (2.3 + 1.8 u) to 1-4 % by day). Absolute colours are UDS's
// (its 5-lux sun) times K = our sun's outer-space luminance / 5.
struct UdsSkyFrameData
{
    float params[4]{}; // x Overall Intensity, y Lit_Intensity, z Contrast Midpoint (cd/m2, ours), w Extra_Contrast
    float glow[4]{};   // rgb Night Sky Glow Color (cd/m2, ours); w 1 = the layer is on
    float sun[4]{};    // xyz world, TO the sky's sun; w the moon gradient's gate, saturate(-15 sin(sun elevation))
    float moon[4]{};   // xyz world, TO the moon; w Enable Moon Gradient
};

// Shared CPU/HLSL layout; LUT uses local Z-up, world Y is altitude above sea level.
struct SkyViewFrameData
{
    // shaders/shared/sky_atmosphere.h (architecture plan R5.3): the fields, once for HLSL and C++.
    using float4 = shader_shared::FloatArray<4>; // rows the C++ indexes (float[4] before R5.3)
    SKY_VIEW_FIELDS
};
static_assert(sizeof(SkyViewFrameData) == 144, "SkyView frame CB layout");

// Time-of-day plan N2: the night sky as the sky PIXEL and its captured picture draw it (skybox.hlsl,
// sky_ibl_capture_cs.hlsl): the rotation onto the celestial sphere and the layers' scales.
struct NightSkyFrameData
{
    // shaders/shared/night_sky.h (architecture plan R5.3): the fields, once for HLSL and C++.
    using float4 = shader_shared::FloatArray<4>; // rows the C++ indexes (float[4] before R5.3)
    NIGHT_SKY_FIELDS
};
static_assert(sizeof(NightSkyFrameData) == 80, "night sky CB layout");
// The stars' and the Milky Way's mean radiance over the sphere, cd/m2 -- what a picture too coarse to
// hold a point sees of them (the capture, the distant light). Mirrors night_sky.hlsli.
inline constexpr float kNightStarsMeanRadiance = 4.5e-5f;
inline constexpr float kNightMilkyWayMeanRadiance = 1.0e-5f;

inline constexpr float kMetresPerKm = 1000.0f;
