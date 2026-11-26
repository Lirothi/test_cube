#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/SkyAtmosphere.h"
#include "rendering/lighting/SkyAtmosphereSettings.h"
#include "rendering/lighting/Skybox.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

// --set / --sweep / MCP set_settings names sky.*, sun.*, light.*, lightShafts.* (the sky atmosphere, the sun, the sky
// light) (architecture plan R2.2). Each row is the branch App.cpp's ApplySweepValue had, its clamp included, and the
// reader ReadSweepValue had (R2.2 gave every simple name one first, so --settings-dump compared the move); a row
// without a reader had none.
namespace settings
{
namespace
{
// Time-of-day plan T3: with a Time of Day these put the sun there by solving its time and path (Scene::SetSunToward),
// so they keep meaning what they say on a converted level. One branch answered both.
bool SetSunAngle(SettingContext& c, std::string_view setting, float value)
{
    const auto d = c.scene.GetSunToward();
    float elevation = std::asin(std::clamp(d.y, -1.0f, 1.0f));
    float azimuth = std::atan2(d.x, d.z);
    constexpr float rad = 3.14159265358979323846f / 180.0f;
    if (setting == "sun.elevation") elevation = std::clamp(value, -90.0f, 90.0f) * rad;
    else azimuth = value * rad;
    c.scene.SetSunToward(Math::float3(std::cos(elevation)*std::sin(azimuth), std::sin(elevation), std::cos(elevation)*std::cos(azimuth)));
    return true;
}

float SunAngle(SettingContext& c, std::string_view name)
{
    const auto d = c.scene.GetSunToward();
    constexpr float rad = 3.14159265358979323846f / 180.0f;
    return name == "sun.elevation" ? std::asin(std::clamp(d.y, -1.0f, 1.0f)) / rad : std::atan2(d.x, d.z) / rad;
}

// The x Earth scales keep the level's per-channel colours (SkyAtmosphereSettings rayleighTint / mieTint / ozoneTint).
// One branch answered the three.
bool SetSkyScale(SettingContext& c, std::string_view setting, float value)
{
    auto& s = c.scene.SkyAtmosphereRef();
    const SkyAtmosphereParameters earth{};
    float rayleigh = SkyMediumScaleOf(s.parameters.RayleighScattering, earth.RayleighScattering, s.rayleighTint);
    float mie = SkyMediumScaleOf(s.parameters.MieScattering, earth.MieScattering, s.mieTint);
    float ozone = SkyMediumScaleOf(s.parameters.AbsorptionExtinction, earth.AbsorptionExtinction, s.ozoneTint);
    const float v = std::clamp(value, 0.0f, 10.0f);
    (setting == "sky.rayleighScale" ? rayleigh : setting == "sky.mieScale" ? mie : ozone) = v;
    SetSkyMedium(s, rayleigh, mie, ozone);
    return true;
}

float SkyScale(SettingContext& c, std::string_view name)
{
    const SkyAtmosphereSettings& s = c.scene.SkyAtmosphereRef();
    const SkyAtmosphereParameters earth{};
    return name == "sky.rayleighScale" ? SkyMediumScaleOf(s.parameters.RayleighScattering, earth.RayleighScattering, s.rayleighTint)
         : name == "sky.mieScale" ? SkyMediumScaleOf(s.parameters.MieScattering, earth.MieScattering, s.mieTint)
                                  : SkyMediumScaleOf(s.parameters.AbsorptionExtinction, earth.AbsorptionExtinction, s.ozoneTint);
}
const SettingDesc kSkyRows[] = {
    // P4: the SCENE-side brightness knobs, so a sweep can measure them against the camera-side
    // ones. `light.exposure` is the field P4 exists to retire -- it multiplies sun AND ambient
    // in lighting_cs, which is a camera control wearing a light's name.
    TC_SETTING("light.exposure", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetExposure(value); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetExposure()); return true; }),
    TC_SETTING("light.ambient", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetAmbient(value); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetAmbient()); return true; }),
    TC_SETTING("sky.environmentLighting", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().environmentLighting = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().environmentLighting); return true; }),
    TC_SETTING("sky.mode", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().mode = value >= 1.0f ? 1u : 0u; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().mode); return true; }),
    TC_SETTING("sky.aerialPerspective", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().aerialPerspective = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().aerialPerspective); return true; }),
    TC_SETTING("sky.aerialDebugView", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().aerialDebugView = static_cast<unsigned>(std::clamp(value, 0.0f, 3.0f)); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().aerialDebugView); return true; }),
    TC_SETTING("sky.luminanceScale", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().luminanceScale = std::clamp(value, 0.0f, 10.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().luminanceScale); return true; }),
    // P1's A/B arm: 0 = the capture and the distant light rebuild on every change of the sky (exact keys).
    TC_SETTING("sky.keyTolerance", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_skyKeyTolerance_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_skyKeyTolerance_game); return true; }),
    // The sun's half-apex angle in radians (the disc and the specular floor), the level sun's property.
    TC_SETTING("sun.angularSize", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetSunHalfApexRadians(std::clamp(value, 0.0f, 0.25f)); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetSunHalfApexRadians()); return true; }),
    // UE's LightSourceSoftAngle of the sun, degrees (apex): softens the specular glint's edge.
    TC_SETTING("sun.softAngle", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetLightSourceSoftAngleDegrees(std::clamp(value, 0.0f, 20.0f)); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetLightSourceSoftAngleDegrees()); return true; }),
    TC_SETTING("sky.skyLuminanceFactor", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().skyLuminanceFactor = std::clamp(value, 0.0f, 10.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().skyLuminanceFactor); return true; }),
    TC_SETTING("sky.aerialStartDepth", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().aerialStartDepthMetres = std::clamp(value, 0.0f, 100000.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().aerialStartDepthMetres); return true; }),
    TC_SETTING("sky.aerialViewDistanceScale", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().aerialViewDistanceScale = std::clamp(value, 0.01f, 100.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().aerialViewDistanceScale); return true; }),
    // B1 transfer LUT controls.
    TC_SETTING("sky.lutValidate", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().lutValidate = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().lutValidate); return true; }),
    TC_SETTING("sky.lutEnabled", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().lutEnabled = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().lutEnabled); return true; }),
    TC_SETTING("sky.lutDebugView", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().lutDebugView = static_cast<unsigned>(std::clamp(value, 0.0f, 2.0f)); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().lutDebugView); return true; }),
    // UE's MieAbsorptionScale over Earth's (0.000444); < 0 = follows sky.mieScale.
    TC_SETTING("sky.mieAbsorptionScale", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   auto& s = c.scene.SkyAtmosphereRef();
                   const SkyAtmosphereParameters earth{};
                   const float mie = SkyMediumScaleOf(s.parameters.MieScattering, earth.MieScattering, s.mieTint);
                   s.mieAbsorptionScale = value < 0.0f ? -1.0f : std::min(value, 10.0f);
                   SetSkyMedium(s, SkyMediumScaleOf(s.parameters.RayleighScattering, earth.RayleighScattering, s.rayleighTint), mie,
                                SkyMediumScaleOf(s.parameters.AbsorptionExtinction, earth.AbsorptionExtinction, s.ozoneTint));
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SkyAtmosphereRef().mieAbsorptionScale); return true; }),
    // UE's SkyAtmosphere HeightFogContribution: the sky's light in the height fog (its ambient colour, the lobe's ground term).
    TC_SETTING("sky.heightFogContribution", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().heightFogContribution = std::clamp(value, 0.0f, 10.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().heightFogContribution); return true; }),
    TC_SETTING("sky.mieAnisotropy", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().parameters.MieScattering[3] = std::clamp(value, 0.0f, 0.999f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().parameters.MieScattering[3]); return true; }),
    TC_SETTING("sky.multiScatteringFactor", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.SkyAtmosphereRef().parameters.GroundAlbedo[3] = std::clamp(value, 0.0f, 10.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.SkyAtmosphereRef().parameters.GroundAlbedo[3]); return true; }),
    TC_SETTING("sky.groundAlbedo", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   for (unsigned i = 0; i < 3; ++i) c.scene.SkyAtmosphereRef().parameters.GroundAlbedo[i] = std::clamp(value, 0.0f, 1.0f);
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SkyAtmosphereRef().parameters.GroundAlbedo[0]); return true; }),
    // Plan A7 light shafts: the sun's own bloom properties (UE ULightComponent fields), headless.
    TC_SETTING("lightShafts.enabled", Float, SettingLevelOwned | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetLightShaftsEnabled(value != 0.0f); return true; },
               [](SettingContext& c, float& out) { out = (c.scene.SunLightRef().GetLightShaftsEnabled()) ? 1.0f : 0.0f; return true; }),
    TC_SETTING("lightShafts.bloomScale", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetLightShaftBloomScale(value); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetLightShaftBloomScale()); return true; }),
    TC_SETTING("lightShafts.bloomThreshold", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetLightShaftBloomThreshold(value); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetLightShaftBloomThreshold()); return true; }),
    TC_SETTING("lightShafts.bloomMaxBrightness", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetLightShaftBloomMaxBrightness(value); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetLightShaftBloomMaxBrightness()); return true; }),
    TC_SETTING("lightShafts.smoothing", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetLightShaftSmoothingSec(std::max(value, 0.0f)); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetLightShaftSmoothingSec()); return true; }),
    TC_SETTING("lightShafts.length", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetLightShaftLength(std::clamp(value, 0.0f, 2.0f)); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetLightShaftLength()); return true; }),
    TC_SETTING("lightShafts.occlusionDepthRange", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SunLightRef().SetLightShaftOcclusionDepthRange(value); return true; },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetLightShaftOcclusionDepthRange()); return true; }),
    TC_SETTING("light.terminatorOffset", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.terminatorOffset = std::clamp(value, 0.0f, 4.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.terminatorOffset); return true; }),
    TC_SETTING("light.geometricTerminator", Int, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.sunGeometricTerminator = static_cast<std::uint32_t>(std::clamp(value, 0.0f, 2.0f) + 0.5f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.sunGeometricTerminator); return true; }),
    TC_SETTING("light.foliageBackSky", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.foliageBackSky = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.foliageBackSky); return true; }),
    TC_SETTING("light.foliageTransmissionUe", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.foliageTransmissionUe = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.foliageTransmissionUe); return true; }),
    TC_SETTING("light.roughDiffuse", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.roughDiffuse = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.roughDiffuse); return true; }),
    TC_SETTING("light.bumpShadowing", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.bumpShadowing = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.bumpShadowing); return true; }),
    // (1 + metal * this) on the analytic sun specular (graphics_settings reflections.sunMetalSpec).
    TC_SETTING("light.sunMetalSpec", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.sunMetalSpecInfluence = std::clamp(value, 0.0f, 16.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.sunMetalSpecInfluence); return true; }),
    TC_SETTING("light.sunTemperatureK", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.scene.SunLightRef().SetUseSunTemperature(value > 0.0f);
                   if (value > 0.0f) { c.scene.SunLightRef().SetSunTemperatureK(value); }
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetUseSunTemperature() ? c.scene.SunLightRef().GetSunTemperatureK() : 0.0f); return true; }),
    // P16.12: one scalar drives all three channels, which is what a sweep needs; the level
    // and the inspector carry the full colour. 0 switches the bounce off.
    TC_SETTING("light.groundAlbedo", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.scene.SunLightRef().SetGroundAlbedo(Math::float3(value, value, value));
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetGroundAlbedo().x); return true; }),
    TC_SETTING("light.skyFillIntensity", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.scene.SunLightRef().SetSkyFillIntensity(value);
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetSkyFillIntensity()); return true; }),
    TC_SETTING("light.skyIntensity", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (Skybox* sky = c.scene.GetSkybox()) { sky->SetExposure(value); }
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.GetSkybox() ? c.scene.GetSkybox()->GetIntensity() : 0.0f); return true; }),
    TC_SETTING("light.skyIlluminanceLux", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (Skybox* sky = c.scene.GetSkybox()) { sky->SetIlluminanceLux(value); }
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.GetSkybox() ? c.scene.GetSkybox()->GetIlluminanceLux() : 0.0f); return true; }),
    // P16.2: the sun's illuminance in lux. `light.sunIntensity` is kept as an ALIAS, not a
    // second control -- it is the same number under its pre-P16.2 name, and every measurement
    // recipe written down in the plan spells it that way.
    TC_SETTING("light.sunIlluminanceLux", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.scene.SunLightRef().SetSunIlluminanceLux(value);
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetSunIlluminanceLux()); return true; }),
    // the same as light.sunIlluminanceLux (one branch answered both)
    TC_SETTING("light.sunIntensity", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.scene.SunLightRef().SetSunIlluminanceLux(value);
                   return true;
               },
               [](SettingContext& c, float& out) { out = static_cast<float>(c.scene.SunLightRef().GetSunIlluminanceLux()); return true; }),
    TC_SETTING("sun.elevation", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return SetSunAngle(c, "sun.elevation", value); },
               [](SettingContext& c, float& out) { out = SunAngle(c, "sun.elevation"); return true; }),
    TC_SETTING("sun.azimuth", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return SetSunAngle(c, "sun.azimuth", value); },
               [](SettingContext& c, float& out) { out = SunAngle(c, "sun.azimuth"); return true; }),
    TC_SETTING("sky.rayleighScale", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return SetSkyScale(c, "sky.rayleighScale", value); },
               [](SettingContext& c, float& out) { out = SkyScale(c, "sky.rayleighScale"); return true; }),
    TC_SETTING("sky.mieScale", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return SetSkyScale(c, "sky.mieScale", value); },
               [](SettingContext& c, float& out) { out = SkyScale(c, "sky.mieScale"); return true; }),
    TC_SETTING("sky.ozoneScale", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return SetSkyScale(c, "sky.ozoneScale", value); },
               [](SettingContext& c, float& out) { out = SkyScale(c, "sky.ozoneScale"); return true; }),
};
TC_SETTINGS(kSkyRows);
}
}
