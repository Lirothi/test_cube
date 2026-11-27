#pragma once

#include "third_party/json/json.hpp"
#include "core/schema/KeyTable.h" // R9.2: the section's table of keys

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

inline void ReadTint(const nlohmann::json& j, const char* key, float out[3])
{
    const auto it = j.find(key);
    if (it != j.end() && it->is_array() && it->size() == 3)
    {
        for (unsigned c = 0; c < 3; ++c)
        {
            out[c] = (*it)[c].is_number() ? std::clamp((*it)[c].get<float>(), 0.0f, 20.0f) : out[c];
        }
    }
}

// Architecture plan R9.2: the section's table of keys (core/schema/KeyTable.h). Most of the sky is not one member per
// key: `enabled` is the mode, the medium's three scales and three tints become the scattering coefficients together
// (SetSkyMedium -- one row reads and writes all seven), the ground albedo and the two UE numbers live in the parameters'
// fourth components. PRESENCE IS NOT THE SWITCH, `enabled` IS -- but the section existing at all is what makes the
// level procedural, exactly as adding a SkyAtmosphere actor is in UE. A level with no section never reaches here and
// stays on its cubemap.
inline const schema::Table<SkyAtmosphereSettings>& Keys()
{
    using S = SkyAtmosphereSettings;
    using J = nlohmann::json;
    static const schema::Table<S> table{ {
        schema::Custom<S>("enabled", [](const J& j, S& s) { s.mode = jsonfield::Value(j, "enabled", true) ? 1u : 0u; },
                          [](J& j, const S& s) { j["enabled"] = s.mode != 0u; }),
        schema::Field("luminanceScale", &S::luminanceScale).Clamp(0.0f, 10.0f),
        schema::Field("skyLuminanceFactor", &S::skyLuminanceFactor).Clamp(0.0f, 10.0f),
        schema::Field("aerialStartDepthMetres", &S::aerialStartDepthMetres).Clamp(0.0f, 100000.0f),
        schema::Field("aerialViewDistanceScale", &S::aerialViewDistanceScale).Clamp(0.01f, 100.0f),
        schema::Field("environmentLighting", &S::environmentLighting),
        schema::Field("aerialPerspective", &S::aerialPerspective),
        schema::Custom<S>("heightFogContribution",
            [](const J& j, S& s) { s.heightFogContribution = std::clamp(jsonfield::Value(j, "heightFogContribution", 1.0f), 0.0f, 10.0f); },
            [](J& j, const S& s) { j["heightFogContribution"] = s.heightFogContribution; }),
        // rayleighScale, mieScale, ozoneScale, rayleighTint, mieTint, ozoneTint, mieAbsorptionScale: the medium.
        schema::Custom<S>("medium",
            [](const J& j, S& s) {
                const float rayleigh = std::clamp(jsonfield::Value(j, "rayleighScale", 1.0f), 0.0f, 10.0f);
                const float mie = std::clamp(jsonfield::Value(j, "mieScale", 1.0f), 0.0f, 10.0f);
                const float ozone = std::clamp(jsonfield::Value(j, "ozoneScale", 1.0f), 0.0f, 10.0f);
                // The medium's colours over the x Earth scales (absent = Earth's): UE's colour / its default, per channel.
                for (unsigned c = 0; c < 3; ++c) { s.rayleighTint[c] = s.mieTint[c] = s.ozoneTint[c] = 1.0f; }
                ReadTint(j, "rayleighTint", s.rayleighTint);
                ReadTint(j, "mieTint", s.mieTint);
                ReadTint(j, "ozoneTint", s.ozoneTint);
                const float absorption = jsonfield::Value(j, "mieAbsorptionScale", -1.0f); // < 0 (or absent) = the Mie scale's
                s.mieAbsorptionScale = absorption < 0.0f ? -1.0f : std::min(absorption, 10.0f);
                SetSkyMedium(s, rayleigh, mie, ozone);
            },
            [](J& j, const S& s) {
                const SkyAtmosphereParameters earth{};
                j["rayleighScale"] = SkyMediumScaleOf(s.parameters.RayleighScattering, earth.RayleighScattering, s.rayleighTint);
                j["mieScale"] = SkyMediumScaleOf(s.parameters.MieScattering, earth.MieScattering, s.mieTint);
                j["ozoneScale"] = SkyMediumScaleOf(s.parameters.AbsorptionExtinction, earth.AbsorptionExtinction, s.ozoneTint);
                const auto tint = [](const float t[3]) { return J::array({t[0], t[1], t[2]}); };
                j["rayleighTint"] = tint(s.rayleighTint);
                j["mieTint"] = tint(s.mieTint);
                j["ozoneTint"] = tint(s.ozoneTint);
                if (s.mieAbsorptionScale >= 0.0f) { j["mieAbsorptionScale"] = s.mieAbsorptionScale; }
            }),
        schema::Custom<S>("groundAlbedo",
            [](const J& j, S& s) {
                const SkyAtmosphereParameters earth{};
                for (unsigned i = 0; i < 3; ++i)
                {
                    s.parameters.GroundAlbedo[i] = std::clamp(jsonfield::Value(j, "groundAlbedo", earth.GroundAlbedo[0]), 0.0f, 1.0f);
                }
            },
            [](J& j, const S& s) { j["groundAlbedo"] = s.parameters.GroundAlbedo[0]; }),
        schema::Custom<S>("multiScatteringFactor",
            [](const J& j, S& s) {
                const SkyAtmosphereParameters earth{};
                s.parameters.GroundAlbedo[3] = std::clamp(jsonfield::Value(j, "multiScatteringFactor", earth.GroundAlbedo[3]), 0.0f, 10.0f);
            },
            [](J& j, const S& s) { j["multiScatteringFactor"] = s.parameters.GroundAlbedo[3]; }),
        // UE's MieAnisotropy, their range and default: how forward-peaked the Mie phase is, which is
        // what decides the size and hardness of the halo around the sun rather than how much haze
        // there is. Lives in mieScattering.w, which `mieScale` deliberately does not touch.
        schema::Custom<S>("mieAnisotropy",
            [](const J& j, S& s) {
                const SkyAtmosphereParameters earth{};
                s.parameters.MieScattering[3] = std::clamp(jsonfield::Value(j, "mieAnisotropy", earth.MieScattering[3]), 0.0f, 0.999f);
            },
            [](J& j, const S& s) { j["mieAnisotropy"] = s.parameters.MieScattering[3]; }),
    } };
    return table;
}

inline void ApplyOverrides(const nlohmann::json& j, SkyAtmosphereSettings& s)
{
    Keys().Read(j, s);
}

inline nlohmann::json ToJson(const SkyAtmosphereSettings& s)
{
    return Keys().Write(s);
}

} // namespace SkyAtmosphereSettingsJson

