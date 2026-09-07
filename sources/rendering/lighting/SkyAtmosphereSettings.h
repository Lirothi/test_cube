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
    bool lutValidate = false; // GPU readback + double-precision UE reference on parameter changes
    bool lutEnabled = false; // B1 has no visible sky consumer yet; opt-in calculation/debugging.
    unsigned lutDebugView = 0; // compose: 1 transmittance, 2 multi-scattering (display gain 10).
    SkyAtmosphereParameters parameters{};
};

inline constexpr float kMetresPerKm = 1000.0f;
