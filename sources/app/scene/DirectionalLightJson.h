#pragma once

#include "third_party/json/json.hpp"

#include "rendering/lighting/DirectionalLight.h"

// The ONE mapping between a directional light's JSON properties and DirectionalLight, for the level
// reader, the editor's runtime apply and the time of day's sun (`timeOfDay.sun` carries the same keys
// as `directionalLight`, so T3 moves an old section across unchanged). It used to be written out twice,
// in JsonLevel and EnvironmentRuntime, each commented "mirrors the other exactly". The DIRECTION is the
// caller's: the two readers default it differently and the time of day computes it.
namespace DirectionalLightJson
{

namespace detail
{
inline float Number(const nlohmann::json& j, const char* key, float def)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<float>() : def;
}

inline bool Flag(const nlohmann::json& j, const char* key, bool def)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

inline Math::float3 Vector(const nlohmann::json& j, const char* key, const Math::float3& def)
{
    const auto it = j.find(key);
    if (it != j.end() && it->is_array() && it->size() >= 3 && (*it)[0].is_number() && (*it)[1].is_number() &&
        (*it)[2].is_number())
    {
        return Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
    }
    return def;
}
} // namespace detail

inline void ApplyOverrides(const nlohmann::json& j, DirectionalLight& dl)
{
    using namespace detail;
    // A disabled light contributes nothing: its direct colour and its ambient term are zeroed (the real
    // values stay in the JSON for re-enabling).
    const bool enabled = Flag(j, "enabled", true);
    dl.SetColor(enabled ? Vector(j, "color", Math::float3(1.0f, 1.0f, 1.0f)) : Math::float3(0.0f, 0.0f, 0.0f));
    dl.SetAmbient(enabled ? Number(j, "ambient", 0.05f) : 0.0f);
    // P16.2: `sunIlluminanceLux` is the field going forward, then P4's `sunIntensity`, then the original
    // whole-scene `exposure` multiplier. Newest present wins, because a level that has been converted
    // should not keep paying for the older semantics, and every step of the chain is LOSSLESS -- P4
    // folded the multiplier into the intensity for an identical product, and P16.2 changed only what
    // the number is called (see DirectionalLight).
    //
    // ONLY the intensity branches. The fill fields below are read UNCONDITIONALLY: they are orthogonal
    // to how the intensity was authored, and gating them on `sunIntensity` made them dead on every
    // existing level -- ticking the boxes in the inspector did nothing at all.
    if (j.contains("sunIlluminanceLux"))
    {
        dl.SetSunIlluminanceLux(Number(j, "sunIlluminanceLux", 100000.0f));
    }
    else if (j.contains("sunIntensity"))
    {
        dl.MigrateLegacySunIntensity(Number(j, "sunIntensity", 1.0f));
    }
    else
    {
        dl.MigrateLegacyExposure(Number(j, "exposure", 1.0f));
    }
    dl.SetSkyFillIntensity(Number(j, "skyFillIntensity", 1.0f));
    // P16.12: the ground's diffuse reflectance. NOT gated on `enabled`: a disabled sun still has a lit
    // sky, and the term scales itself down through the illuminance it reads.
    dl.SetGroundAlbedo(Vector(j, "groundAlbedo", dl.GetGroundAlbedo()));
    dl.SetUseSunTemperature(Flag(j, "useSunTemperature", false));
    dl.SetSunTemperatureK(Number(j, "sunTemperatureK", 6500.0f));
    // Plan A7 light shafts: the light's own bloom properties (UE ULightComponent fields), all optional.
    dl.SetLightShaftsEnabled(Flag(j, "lightShaftsEnabled", dl.GetLightShaftsEnabled()));
    dl.SetLightShaftBloomScale(Number(j, "lightShaftBloomScale", dl.GetLightShaftBloomScale()));
    dl.SetLightShaftBloomThreshold(Number(j, "lightShaftBloomThreshold", dl.GetLightShaftBloomThreshold()));
    dl.SetLightShaftBloomMaxBrightness(Number(j, "lightShaftBloomMaxBrightness", dl.GetLightShaftBloomMaxBrightness()));
    dl.SetLightShaftBloomTint(Vector(j, "lightShaftBloomTint", dl.GetLightShaftBloomTint()));
    dl.SetLightShaftOcclusionDepthRange(Number(j, "lightShaftOcclusionDepthRange", dl.GetLightShaftOcclusionDepthRange()));
    dl.SetLightShaftSmoothingSec(Number(j, "lightShaftSmoothing", dl.GetLightShaftSmoothingSec()));
    // The angular diameter in degrees (UE LightSourceAngle); drives the drawn disc and the specular floor.
    dl.SetLightSourceAngleDegrees(Number(j, "lightSourceAngle", dl.GetLightSourceAngleDegrees()));
}

inline Math::float3 Direction(const nlohmann::json& j, const Math::float3& def)
{
    return detail::Vector(j, "direction", def).Normalized();
}

} // namespace DirectionalLightJson
