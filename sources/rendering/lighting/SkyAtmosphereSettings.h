#pragma once

// B1: kilometre-space atmosphere, separate from metre-space height/froxel fog.
// UE SkyAtmosphereComponent.cpp:94-128 (Earth defaults). No sun/view/exposure in this key.
struct SkyAtmosphereParameters
{
    float radii[4] = {6360.0f, 6420.0f, -1.0f / 8.0f, -1.0f / 1.2f};
    float rayleigh[4] = {0.005802f, 0.013558f, 0.033100f, 0.0f};
    float mieScattering[4] = {0.003996f, 0.003996f, 0.003996f, 0.8f};
    float mieAbsorption[4] = {0.000444f, 0.000444f, 0.000444f, 0.0f};
    float ozone[4] = {0.000650f, 0.001881f, 0.000085f, 25.0f};
    float ozoneDensity[4] = {1.0f / 15.0f, -2.0f / 3.0f, -1.0f / 15.0f, 8.0f / 3.0f};
    // FColor(170) converted from sRGB to linear, NOT 170/255.
    float groundAlbedo[4] = {0.40197778f, 0.40197778f, 0.40197778f, 1.0f};
};
static_assert(sizeof(SkyAtmosphereParameters) == 112, "SkyAtmosphereCB layout");

struct SkyAtmosphereSettings
{
    // 0 HDRI cubemap, 1 procedural atmosphere. AUTHORED BY THE LEVEL (`skybox.procedural`), not a
    // session override: which sky a level uses is part of the level, the same as which cubemap it
    // names. `--set=sky.mode` still overrides it for a headless run.
    unsigned mode = 0;
    float luminanceScale = 2.13f; // sky-only HDRI calibration, never camera exposure
    bool distantSkyLight = false; // B5: isotropic ambient at 6km, session opt-in
    bool environmentLighting = false; // B4: session opt-in until visual acceptance
    bool aerialPerspective = false; // B3: procedural mode only; enable after visual acceptance
    unsigned aerialDebugView = 0; // 0 scene, 1 transmittance, 2 luminance, 3 depth slices
    bool lutValidate = false; // GPU readback + double-precision UE reference on parameter changes
    bool lutEnabled = false; // Explicit LUT calculation even in HDRI mode.
    unsigned lutDebugView = 0; // compose: 1 transmittance, 2 multi-scattering (display gain 10).
    SkyAtmosphereParameters parameters{};
};

// Shared CPU/HLSL layout; LUT uses local Z-up, world Y is altitude above sea level.
struct SkyViewFrameData
{
    float sunDirection[4]{}; // local XYZ to sun; w angular radius in radians
    float illuminance[4]{}; // outer-space RGB lux; w sky-only luminance scale
    float exposure[4] = {1, 0, 0, 0}; // pre-exposure before FP16 storage
    float planet[4]{}; // view height, bottom radius, top radius (km), enabled
};
static_assert(sizeof(SkyViewFrameData) == 64, "SkyView frame CB layout");

inline constexpr float kMetresPerKm = 1000.0f;
