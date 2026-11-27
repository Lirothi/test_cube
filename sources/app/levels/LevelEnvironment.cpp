#include "app/levels/LevelEnvironment.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include <DirectXMath.h>

#include "app/scene/BloomSettingsJson.h"
#include "app/scene/GtaoSettingsJson.h"
#include "app/scene/HeightFogSettingsJson.h"
#include "app/scene/Scene.h"
#include "app/scene/SkyAtmosphereSettingsJson.h"
#include "app/scene/VolumetricCloudSettingsJson.h"
#include "rendering/core/PhotographicSettings.h"
#include "rendering/core/PhotographicSettingsJson.h"
#include "rendering/lighting/NightLamps.h"
#include "rendering/lighting/PointLight.h"
#include "rendering/lighting/SpotLight.h"

using nlohmann::json;

namespace levelenv
{
namespace
{
float Number(const json& j, const char* key, float def, JsonRead mode)
{
    if (mode == JsonRead::Strict) { return j.value(key, def); }
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<float>() : def;
}

// The same in both modes: not an array of three (or more) = the default.
Math::float3 Vector(const json& j, const char* key, const Math::float3& def)
{
    const auto it = j.find(key);
    if (it != j.end() && it->is_array() && it->size() >= 3)
    {
        return Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
    }
    return def;
}

void ReadPointLightFlicker(const json& source, PointLightDesc& desc, JsonRead mode)
{
    const auto it = source.find("flicker");
    if (it == source.end() || !it->is_object())
    {
        return;
    }

    const json& flicker = *it;
    desc.flicker.amplitude = Number(flicker, "amplitude", desc.flicker.amplitude, mode);
    desc.flicker.frequencyHz = Number(flicker, "frequencyHz", desc.flicker.frequencyHz, mode);
    const auto seedIt = flicker.find("seed");
    if (seedIt != flicker.end() && seedIt->is_number_integer())
    {
        const std::int64_t seed = seedIt->get<std::int64_t>();
        desc.flicker.seed = seed > 0 ? static_cast<std::uint32_t>(seed) : 0u;
    }
}
} // namespace

void ReadSpotLight(const json& sl, SpotLightDesc& desc, JsonRead mode)
{
    desc.position = Vector(sl, "position", desc.position);
    desc.direction = Vector(sl, "direction", desc.direction).Normalized();
    desc.range = Number(sl, "range", desc.range, mode);
    desc.innerAngle = DirectX::XMConvertToRadians(Number(sl, "innerAngleDeg", 15.0f, mode));
    desc.outerAngle = DirectX::XMConvertToRadians(Number(sl, "outerAngleDeg", 25.0f, mode));
    desc.color = Vector(sl, "color", desc.color);
    // P16.5: `luminousFluxLm` is the field going forward; `intensity` is the pre-P16.5
    // number, converted by the r/2 rule (PhotographicSettings.h). NOT lossless -- the
    // falloff shape changed with the unit -- so an existing level keeps its lights roughly
    // where they were rather than exactly.
    desc.luminousFluxLm = sl.contains("luminousFluxLm")
        ? Number(sl, "luminousFluxLm", desc.luminousFluxLm, mode)
        : render::LumensFromLegacyIntensity(Number(sl, "intensity", 5.0f, mode), desc.range);
    desc.shadowNormalBias = Number(sl, "shadowNormalBias", desc.shadowNormalBias, mode);
    desc.volumetricIntensity = Number(sl, "volumetricIntensity", desc.volumetricIntensity, mode); // plan A4d
    desc.shadowDepthBias = Number(sl, "shadowDepthBias", desc.shadowDepthBias, mode);
    desc.shadowsEnabled = sl.value("shadowsEnabled", desc.shadowsEnabled);
    desc.onAt = LampSwitchFromName(sl.value("onAt", std::string())); // plan L
    desc.dayBrightness = std::max(Number(sl, "dayBrightness", desc.dayBrightness, mode), 0.0f);
    desc.shadowBias = Number(sl, "shadowBias", desc.shadowBias, mode);           // local-light shadows S1 (UE's)
    desc.shadowSlopeBias = Number(sl, "shadowSlopeBias", desc.shadowSlopeBias, mode);
    desc.shadowSharpen = Number(sl, "shadowSharpen", desc.shadowSharpen, mode); // S2
    desc.sourceRadius = std::max(Number(sl, "sourceRadius", desc.sourceRadius, mode), 0.0f);
    desc.nearPlane = Number(sl, "shadowNear", desc.nearPlane, mode);           // S8: the shadow projection's near
}

void ReadPointLight(const json& pl, PointLightDesc& desc, JsonRead mode)
{
    desc.position = Vector(pl, "position", desc.position);
    desc.radius = Number(pl, "radius", desc.radius, mode);
    desc.color = Vector(pl, "color", desc.color);
    desc.luminousFluxLm = pl.contains("luminousFluxLm") // P16.5, see the spot above
        ? Number(pl, "luminousFluxLm", desc.luminousFluxLm, mode)
        : render::LumensFromLegacyIntensity(Number(pl, "intensity", 1.0f, mode), desc.radius);
    desc.shadowsEnabled = pl.value("shadowsEnabled", desc.shadowsEnabled);
    desc.volumetricIntensity = Number(pl, "volumetricIntensity", desc.volumetricIntensity, mode); // plan A4d
    ReadPointLightFlicker(pl, desc, mode);
    desc.onAt = LampSwitchFromName(pl.value("onAt", std::string())); // plan L
    desc.dayBrightness = std::max(Number(pl, "dayBrightness", desc.dayBrightness, mode), 0.0f);
    desc.shadowBias = Number(pl, "shadowBias", desc.shadowBias, mode);           // local-light shadows S1 (UE's)
    desc.shadowSlopeBias = Number(pl, "shadowSlopeBias", desc.shadowSlopeBias, mode);
    desc.shadowSharpen = Number(pl, "shadowSharpen", desc.shadowSharpen, mode); // S2
    desc.sourceRadius = std::max(Number(pl, "sourceRadius", desc.sourceRadius, mode), 0.0f);
}

void ApplySkyAtmosphere(Scene& scene, const json* section)
{
    SkyAtmosphereSettings sky{};
    if (section) { SkyAtmosphereSettingsJson::ApplyOverrides(*section, sky); }
    scene.SetSkyAtmosphere(sky);
}

void ApplyVolumetricCloud(Scene& scene, const json* section)
{
    VolumetricCloudSettings clouds{};
    if (section) { VolumetricCloudSettingsJson::ApplyOverrides(*section, clouds); }
    scene.SetVolumetricCloud(clouds);
}

void ApplyGtao(Scene& scene, const json* section)
{
    GtaoSettings gtao{};
    if (section) { GtaoSettingsJson::ApplyOverrides(*section, gtao); }
    scene.SetGtao(gtao);
}

void ApplyHeightFog(Scene& scene, const json* section)
{
    HeightFogSettings heightFog{};
    if (section) { HeightFogSettingsJson::ApplyOverrides(*section, heightFog); }
    scene.SetHeightFog(heightFog);
}

void ApplyBloom(Scene& scene, const json* section)
{
    BloomSettings bloom{};
    if (section) { BloomSettingsJson::ApplyOverrides(*section, bloom); }
    scene.SetBloom(bloom);
}

void ApplyColorPipeline(Scene& scene, const json* section)
{
    render::ColorPipelineSettings color{};
    if (section) { render::PhotographicSettingsJson::ApplyOverrides(*section, color); }
    scene.SetColorPipeline(color);
}

void ApplyCameraExposure(Scene& scene, const json* exposure)
{
    render::CameraExposureSettings settings{};
    if (exposure) { render::PhotographicSettingsJson::ApplyOverrides(*exposure, settings); }
    scene.SetCameraExposure(settings);
}
} // namespace levelenv
