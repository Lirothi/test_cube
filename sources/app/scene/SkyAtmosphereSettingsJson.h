#pragma once

#include "third_party/json/json.hpp"

#include "rendering/lighting/SkyAtmosphereSettings.h"

#include <algorithm>

// B6.2: the ONE mapping between the LEVEL-AUTHORED half of SkyAtmosphereSettings and JSON. The
// level reader, the editor's runtime apply and the serializer all go through here, for the same
// reason GtaoSettingsJson and HeightFogSettingsJson do: three copies of a field list drift.
//
// WHICH HALF. This carries what the level decides -- is this sky procedural, how bright, does it
// light the scene, does it build the distant sky light and the aerial-perspective volume, and the
// medium's own parameters. It deliberately does NOT carry `lutDebugView`, `aerialDebugView`,
// `lutValidate` or `lutEnabled`: those are debug views and a validator, they belong to the session
// and to the Developer window, and writing them into a level would ship somebody's debug state.
//
// The parameter SCALES are authored, not the raw coefficients: `rayleighScale` multiplies Earth's
// values rather than replacing them, which is the same shape `--set=sky.rayleighScale` has always
// had and keeps a level readable ("1.0 = Earth") instead of listing six-decimal cross-sections.
namespace SkyAtmosphereSettingsJson
{

inline float ScaleOf(const float authored[4], const float earth[4])
{
    // Recovered from the first channel; the three are always written as one scale.
    return earth[0] > 0.0f ? authored[0] / earth[0] : 1.0f;
}

inline void ApplyOverrides(const nlohmann::json& j, SkyAtmosphereSettings& s)
{
    // PRESENCE IS NOT THE SWITCH, `enabled` IS -- but the section existing at all is what makes the
    // level procedural, exactly as adding a SkyAtmosphere actor is in UE. A level with no section
    // never reaches here and stays on its cubemap.
    s.mode = j.value("enabled", true) ? 1u : 0u;
    s.luminanceScale = std::clamp(j.value("luminanceScale", s.luminanceScale), 0.0f, 10.0f);
    s.skyLuminanceFactor = std::clamp(j.value("skyLuminanceFactor", s.skyLuminanceFactor), 0.0f, 10.0f);
    s.aerialStartDepthMetres = std::clamp(j.value("aerialStartDepthMetres", s.aerialStartDepthMetres), 0.0f, 100000.0f);
    s.aerialViewDistanceScale = std::clamp(j.value("aerialViewDistanceScale", s.aerialViewDistanceScale), 0.01f, 100.0f);
    s.environmentLighting = j.value("environmentLighting", s.environmentLighting);
    s.aerialPerspective = j.value("aerialPerspective", s.aerialPerspective);

    const SkyAtmosphereParameters earth{};
    const float rayleigh = std::clamp(j.value("rayleighScale", 1.0f), 0.0f, 10.0f);
    const float mie = std::clamp(j.value("mieScale", 1.0f), 0.0f, 10.0f);
    const float ozone = std::clamp(j.value("ozoneScale", 1.0f), 0.0f, 10.0f);
    for (unsigned i = 0; i < 3; ++i)
    {
        s.parameters.rayleigh[i] = earth.rayleigh[i] * rayleigh;
        s.parameters.mieScattering[i] = earth.mieScattering[i] * mie;
        s.parameters.mieAbsorption[i] = earth.mieAbsorption[i] * mie;
        s.parameters.ozone[i] = earth.ozone[i] * ozone;
        s.parameters.groundAlbedo[i] = std::clamp(j.value("groundAlbedo", earth.groundAlbedo[0]), 0.0f, 1.0f);
    }
    s.parameters.groundAlbedo[3] =
        std::clamp(j.value("multiScatteringFactor", earth.groundAlbedo[3]), 0.0f, 10.0f);
    // UE's MieAnisotropy, their range and default: how forward-peaked the Mie phase is, which is
    // what decides the size and hardness of the halo around the sun rather than how much haze
    // there is. Lives in mieScattering.w, which `mieScale` deliberately does not touch.
    s.parameters.mieScattering[3] =
        std::clamp(j.value("mieAnisotropy", earth.mieScattering[3]), 0.0f, 0.999f);
}

inline nlohmann::json ToJson(const SkyAtmosphereSettings& s)
{
    const SkyAtmosphereParameters earth{};
    nlohmann::json j = nlohmann::json::object();
    j["enabled"] = s.mode != 0u;
    j["luminanceScale"] = s.luminanceScale;
    j["skyLuminanceFactor"] = s.skyLuminanceFactor;
    j["aerialStartDepthMetres"] = s.aerialStartDepthMetres;
    j["aerialViewDistanceScale"] = s.aerialViewDistanceScale;
    j["environmentLighting"] = s.environmentLighting;
    j["aerialPerspective"] = s.aerialPerspective;
    j["rayleighScale"] = ScaleOf(s.parameters.rayleigh, earth.rayleigh);
    j["mieScale"] = ScaleOf(s.parameters.mieScattering, earth.mieScattering);
    j["ozoneScale"] = ScaleOf(s.parameters.ozone, earth.ozone);
    j["groundAlbedo"] = s.parameters.groundAlbedo[0];
    j["multiScatteringFactor"] = s.parameters.groundAlbedo[3];
    j["mieAnisotropy"] = s.parameters.mieScattering[3];
    return j;
}

} // namespace SkyAtmosphereSettingsJson
