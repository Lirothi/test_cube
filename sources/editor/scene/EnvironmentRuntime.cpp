#include "editor/scene/EnvironmentRuntime.h"
#include "app/scene/GtaoSettingsJson.h"
#include "app/scene/AtmosphereSettingsJson.h"
#include "app/scene/BloomSettingsJson.h"
#if WITH_EDITOR

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "app/Systems.h"
#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "ocean/OceanRenderConfigJson.h"
#include "ocean/OceanSimulation.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/scene/EditorSceneDocument.h"
#include "rendering/core/PhotographicSettingsJson.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/PointLight.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/SpotLight.h"
#include "vfx/WindState.h"

namespace
{
    constexpr float kDeg2Rad = 0.01745329252f;

    float JF(const nlohmann::json& p, const char* key, float def)
    {
        const auto it = p.find(key);
        return (it != p.end() && it->is_number()) ? it->get<float>() : def;
    }

    Math::float3 JF3(const nlohmann::json& p, const char* key, const Math::float3& def)
    {
        const auto it = p.find(key);
        if (it != p.end() && it->is_array() && it->size() >= 3)
        {
            return Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
        }
        return def;
    }

    void ParsePointLightFlicker(const nlohmann::json& source, PointLightDesc& desc)
    {
        const auto it = source.find("flicker");
        if (it == source.end() || !it->is_object())
        {
            return;
        }

        const nlohmann::json& flicker = *it;
        desc.flicker.amplitude = JF(flicker, "amplitude", desc.flicker.amplitude);
        desc.flicker.frequencyHz = JF(flicker, "frequencyHz", desc.flicker.frequencyHz);
        const auto seedIt = flicker.find("seed");
        if (seedIt != flicker.end() && seedIt->is_number_integer())
        {
            const std::int64_t seed = seedIt->get<std::int64_t>();
            desc.flicker.seed = seed > 0 ? static_cast<std::uint32_t>(seed) : 0u;
        }
    }

    // Map an env light entity to its LightManager index. JsonLevel skips disabled
    // lights, so count only enabled same-type entities before the target; -1 if the
    // target is disabled / absent (then it is saved but has no live runtime light).
    int LightManagerIndexFor(EditorSceneDocument& doc, const EditorObject& target)
    {
        int idx = 0;
        for (const EditorObject& e : doc.Environment())
        {
            if (e.type != target.type) { continue; }
            const bool enabled = e.properties.value("enabled", true);
            if (e.id.value == target.id.value) { return enabled ? idx : -1; }
            if (enabled) { ++idx; }
        }
        return -1;
    }

    std::wstring Widen(const std::string& s)
    {
        return std::wstring(s.begin(), s.end());
    }

    void ApplySkybox(EditorContext& ctx, const nlohmann::json& p)
    {
        const std::string texture = p.value("texture", std::string());

        ctx.renderer.WaitForPreviousFrame();
        if (texture.empty())
        {
            ctx.scene.SetSkybox(nullptr);
            return;
        }

        UploadBatch uploads;
        if (!uploads.Begin(&ctx.renderer))
        {
            return;
        }

        auto skybox = std::make_unique<Skybox>(Widen(texture));
        skybox->Init(&ctx.renderer, uploads.CommandList(), uploads.KeepAlive());
        uploads.SubmitAndWait(&ctx.renderer);
        // See JsonLevel: HDRI libraries are not calibrated to the engine's linear scale, so the
        // level says how bright its sky is. 1 = the cubemap untouched.
        skybox->SetExposure(JF(p, "intensity", 1.0f));
        ctx.scene.SetSkybox(std::move(skybox));
    }

    void ApplyWind(EditorContext& ctx, const nlohmann::json& p)
    {
        vfx::WindState& wind = ctx.scene.GetWindState();
        const float directionDeg = JF(p, "directionDeg", wind.directionDeg);
        const float strength = Math::Saturate(JF(p, "strength", wind.strength));
        const bool oceanDriveChanged =
            directionDeg != wind.directionDeg || strength != wind.strength;

        wind.active = true;
        wind.directionDeg = directionDeg;
        wind.strength = strength;
        wind.swayFrequency = std::max(0.0f, JF(p, "swayFrequency", wind.swayFrequency));
        wind.foliageSwayMeters = std::max(
            0.0f,
            JF(p, "foliageSwayMeters", wind.foliageSwayMeters));

        const auto gustIt = p.find("gust");
        if (gustIt != p.end() && gustIt->is_object())
        {
            wind.gustAmplitude = std::max(
                0.0f,
                JF(*gustIt, "amplitude", wind.gustAmplitude));
            wind.gustFrequencyHz = std::max(
                0.0f,
                JF(*gustIt, "frequencyHz", wind.gustFrequencyHz));
            wind.gustSeed = JF(*gustIt, "seed", wind.gustSeed);
        }

        // Direction/strength are also the ocean's source of truth. ResetInitialSpectrum retires
        // GPU resources, so make the same edit-time GPU-idle guarantee used by other rebuilds.
        // Gust/sway-only edits avoid touching the FFT and apply on the next Scene::Tick.
        if (oceanDriveChanged)
        {
            if (OceanSimulation* ocean = Systems::GetOceanSimulation())
            {
                ctx.renderer.WaitForPreviousFrame();
                ocean->SetSceneVariables(
                    &ctx.renderer,
                    wind.directionDeg,
                    ocean->GetSwellDirectionDegrees(),
                    wind.strength);
            }
        }
    }
}

void EnvironmentRuntime::Apply(EditorContext& ctx, const EditorObject& env)
{
    const nlohmann::json& p = env.properties;

    if (env.type == "pointLight")
    {
        const int i = LightManagerIndexFor(ctx.document, env);
        auto& points = ctx.scene.GetLightManager().PointLights();
        if (i < 0 || i >= static_cast<int>(points.size())) { return; }
        PointLightDesc d;
        d.position = JF3(p, "position", d.position);
        d.radius = JF(p, "radius", d.radius);
        d.color = JF3(p, "color", d.color);
        d.intensity = JF(p, "intensity", d.intensity);
        d.shadowsEnabled = p.value("shadowsEnabled", d.shadowsEnabled);
        ParsePointLightFlicker(p, d);
        points[i].SetDesc(d);
    }
    else if (env.type == "spotLight")
    {
        const int i = LightManagerIndexFor(ctx.document, env);
        auto& spots = ctx.scene.GetLightManager().SpotLights();
        if (i < 0 || i >= static_cast<int>(spots.size())) { return; }
        SpotLightDesc d;
        d.position = JF3(p, "position", d.position);
        d.direction = JF3(p, "direction", d.direction).Normalized();
        d.range = JF(p, "range", d.range);
        d.innerAngle = JF(p, "innerAngleDeg", 15.0f) * kDeg2Rad;
        d.outerAngle = JF(p, "outerAngleDeg", 25.0f) * kDeg2Rad;
        d.color = JF3(p, "color", d.color);
        d.intensity = JF(p, "intensity", d.intensity);
        d.shadowNormalBias = JF(p, "shadowNormalBias", d.shadowNormalBias);
        d.shadowDepthBias = JF(p, "shadowDepthBias", d.shadowDepthBias);
        d.shadowsEnabled = p.value("shadowsEnabled", d.shadowsEnabled);
        spots[i].SetDesc(d);
    }
    else if (env.type == "directionalLight")
    {
        // A disabled sun contributes nothing: zero both its direct color and its
        // ambient term (the real values stay in `properties` for re-enable).
        const bool enabled = p.value("enabled", true);
        DirectionalLight dl;
        dl.SetDirection(JF3(p, "direction", Math::float3(-1.0f, -1.0f, -1.0f)).Normalized());
        dl.SetColor(enabled ? JF3(p, "color", Math::float3(1.0f, 1.0f, 1.0f)) : Math::float3(0.0f, 0.0f, 0.0f));
        dl.SetAmbient(enabled ? JF(p, "ambient", 0.05f) : 0.0f);
        // P4: mirrors JsonLevel exactly -- `sunIntensity` present means the level is converted and
        // the legacy multiplier is ignored; absent means migrate it losslessly. ONLY the intensity
        // branches; the fill fields are read unconditionally, because gating them here is what made
        // the inspector checkboxes inert on every legacy level.
        if (p.contains("sunIntensity"))
        {
            dl.SetSunIntensity(JF(p, "sunIntensity", 1.0f));
        }
        else
        {
            dl.MigrateLegacyExposure(JF(p, "exposure", 1.0f));
        }
        dl.SetSkyFillIntensity(JF(p, "skyFillIntensity", 1.0f));
        dl.SetUseSunTemperature(p.value("useSunTemperature", false));
        dl.SetSunTemperatureK(JF(p, "sunTemperatureK", 6500.0f));
        ctx.scene.SetDirectionalLight(dl);
    }
    else if (env.type == "cameraExposure")
    {
        // P1: rebuilt from defaults on every edit rather than patched in place, so clearing a
        // field in the inspector returns it to the documented default instead of keeping whatever
        // the previous edit left behind.
        render::CameraExposureSettings exposure{};
        render::PhotographicSettingsJson::ApplyOverrides(p, exposure);
        // The P3B fields moved here from the Color Pipeline object; an unconverted document still
        // has them over there, so lift them rather than snapping the level back to neutral.
        for (const EditorObject& other : ctx.document.Environment())
        {
            if (other.type == "colorPipeline")
            {
                render::PhotographicSettingsJson::MigrateLegacyLocalExposure(other.properties, p, exposure);
                break;
            }
        }
        ctx.scene.SetCameraExposure(exposure);
    }
    else if (env.type == "gtao")
    {
        // P6B: rebuilt from defaults on every edit, same reasoning as the two blocks around it --
        // a field the document does not carry must land on the struct default, not on whatever the
        // previous edit left behind.
        GtaoSettings gtao{};
        GtaoSettingsJson::ApplyOverrides(p, gtao);
        ctx.scene.SetGtao(gtao);
    }
    else if (env.type == "atmosphere")
    {
        // P7: rebuilt from defaults on every edit, same reasoning as the blocks around it -- a
        // field the document does not carry must land on the struct default, not on whatever the
        // previous edit left behind.
        AtmosphereSettings atmosphere{};
        AtmosphereSettingsJson::ApplyOverrides(p, atmosphere);
        ctx.scene.SetAtmosphere(atmosphere);
    }
    else if (env.type == "postProcess")
    {
        // P8B: apply every group the folded object carries, through the SAME per-group readers the
        // individual objects use. A missing group is applied as struct defaults, exactly as a
        // missing section is at level load, so "the group is not in the file" means one thing.
        const auto sub = [&p](const char* key) -> const nlohmann::json& {
            static const nlohmann::json kEmpty = nlohmann::json::object();
            const auto it = p.find(key);
            return (it != p.end() && it->is_object()) ? *it : kEmpty;
        };
        {
            render::CameraExposureSettings exposure{};
            render::PhotographicSettingsJson::ApplyOverrides(sub("cameraExposure"), exposure);
            ctx.scene.SetCameraExposure(exposure);
        }
        {
            render::ColorPipelineSettings color{};
            render::PhotographicSettingsJson::ApplyOverrides(sub("colorPipeline"), color);
            ctx.scene.SetColorPipeline(color);
        }
        {
            GtaoSettings gtao{};
            GtaoSettingsJson::ApplyOverrides(sub("gtao"), gtao);
            ctx.scene.SetGtao(gtao);
        }
        {
            AtmosphereSettings atmosphere{};
            AtmosphereSettingsJson::ApplyOverrides(sub("atmosphere"), atmosphere);
            ctx.scene.SetAtmosphere(atmosphere);
        }
        {
            BloomSettings bloom{};
            BloomSettingsJson::ApplyOverrides(sub("bloom"), bloom);
            ctx.scene.SetBloom(bloom);
        }
    }
    else if (env.type == "bloom")
    {
        // P8: rebuilt from defaults on every edit, same reasoning as the blocks around it -- a
        // field the document does not carry must land on the struct default, not on whatever the
        // previous edit left behind.
        BloomSettings bloom{};
        BloomSettingsJson::ApplyOverrides(p, bloom);
        ctx.scene.SetBloom(bloom);
    }
    else if (env.type == "colorPipeline")
    {
        // P3C: rebuilt from defaults on every edit, same reasoning as cameraExposure above.
        render::ColorPipelineSettings color{};
        render::PhotographicSettingsJson::ApplyOverrides(p, color);
        ctx.scene.SetColorPipeline(color);
        // Legacy documents still carry the local-exposure fields here; keep the camera in step so
        // an old level behaves the same as before the move until it is re-saved.
        for (const EditorObject& other : ctx.document.Environment())
        {
            if (other.type == "cameraExposure")
            {
                render::CameraExposureSettings exposure{};
                render::PhotographicSettingsJson::ApplyOverrides(other.properties, exposure);
                render::PhotographicSettingsJson::MigrateLegacyLocalExposure(p, other.properties, exposure);
                ctx.scene.SetCameraExposure(exposure);
                break;
            }
        }
    }
    else if (env.type == "camera")
    {
        Camera& cam = ctx.scene.CameraRef();
        cam.SetHFov(JF(p, "hfovDeg", 90.0f) * kDeg2Rad);
        cam.SetZNearFar(JF(p, "zNear", 0.01f), JF(p, "zFar", 10000.0f));
    }
    else if (env.type == "skybox")
    {
        ApplySkybox(ctx, p);
    }
    else if (env.type == "ocean")
    {
        if (OceanSimulation* ocean = Systems::GetOceanSimulation())
        {
            const auto renderIt = p.find("render");
            if (renderIt != p.end() && renderIt->is_object())
            {
                OceanRenderConfig render = ocean->GetRenderConfig();
                OceanRenderConfigJson::ApplyOverrides(*renderIt, render);
                ocean->SetRenderConfig(render);
            }

            // Wind changes rebuild the FFT spectrum. Render-only inspector changes must not.
            const float windDir = JF(p, "windDirectionDeg", ocean->GetLocalWindDirectionDegrees());
            const float swellDir = JF(p, "swellDirectionDeg", ocean->GetSwellDirectionDegrees());
            const float windForce = JF(p, "windForce", ocean->GetWindForce01());
            if (windDir != ocean->GetLocalWindDirectionDegrees() ||
                swellDir != ocean->GetSwellDirectionDegrees() ||
                windForce != ocean->GetWindForce01())
            {
                ocean->SetSceneVariables(&ctx.renderer, windDir, swellDir, windForce);
            }
        }
    }
    else if (env.type == "wind")
    {
        ApplyWind(ctx, p);
    }
}

void EnvironmentRuntime::ApplyChange(
    EditorContext& ctx,
    const EditorObject& env,
    const EditorObject& previous)
{
    const bool enabledChanged =
        env.properties.value("enabled", true) !=
        previous.properties.value("enabled", true);

    if (env.type == "pointLight" || env.type == "spotLight")
    {
        if (enabledChanged)
        {
            RebuildLights(ctx);
        }
        else
        {
            Apply(ctx, env);
        }
        return;
    }

    if (env.type == "ocean")
    {
        const std::string preset = env.properties.value("preset", std::string());
        const std::string previousPreset =
            previous.properties.value("preset", std::string());
        if (preset != previousPreset && !preset.empty())
        {
            if (OceanSimulation* ocean = Systems::GetOceanSimulation())
            {
                ocean->LoadConfig(&ctx.renderer, Widen(preset));
            }
        }
        else
        {
            const bool hasRenderOverrides =
                env.properties.contains("render") && env.properties["render"].is_object();
            const bool hadRenderOverrides =
                previous.properties.contains("render") && previous.properties["render"].is_object();
            if (!hasRenderOverrides && hadRenderOverrides)
            {
                if (OceanSimulation* ocean = Systems::GetOceanSimulation())
                {
                    OceanSimulationConfig presetConfig;
                    if (LoadOceanSimulationConfigFromFile(ocean->GetConfigPath(), presetConfig))
                    {
                        ocean->SetRenderConfig(presetConfig.render);
                    }
                    else
                    {
                        ocean->SetRenderConfig(OceanRenderConfig{});
                    }
                }
            }
        }

        ctx.scene.SetOceanVisible(env.properties.value("enabled", true));
        Apply(ctx, env);
        return;
    }

    Apply(ctx, env);
}

void EnvironmentRuntime::Remove(EditorContext& ctx, const EditorObject& env)
{
    if (env.type == "pointLight" || env.type == "spotLight")
    {
        RebuildLights(ctx);
    }
    else if (env.type == "directionalLight")
    {
        DirectionalLight dl;
        dl.SetDirection(JF3(env.properties, "direction", Math::float3(0.0f, -1.0f, 0.0f)).Normalized());
        dl.SetColor(Math::float3(0.0f, 0.0f, 0.0f));
        dl.SetExposure(1.0f);
        dl.SetAmbient(0.0f);
        ctx.scene.SetDirectionalLight(dl);
    }
    else if (env.type == "skybox")
    {
        ctx.renderer.WaitForPreviousFrame();
        ctx.scene.SetSkybox(nullptr);
    }
    else if (env.type == "cameraExposure")
    {
        // P1: removing the section returns the scene to the dormant defaults, which is the same
        // state a level that never had the section loads with.
        ctx.scene.SetCameraExposure(render::CameraExposureSettings{});
    }
    else if (env.type == "gtao")
    {
        ctx.scene.SetGtao(GtaoSettings{});
    }
    else if (env.type == "bloom")
    {
        ctx.scene.SetBloom(BloomSettings{});
    }
    else if (env.type == "atmosphere")
    {
        ctx.scene.SetAtmosphere(AtmosphereSettings{});
    }
    else if (env.type == "colorPipeline")
    {
        ctx.scene.SetColorPipeline(render::ColorPipelineSettings{});
    }
    else if (env.type == "wind")
    {
        ctx.scene.GetWindState() = vfx::WindState{};
        vfx::g_maxSwayExtentMeters = 0.0f;
    }
}

void EnvironmentRuntime::RebuildLights(EditorContext& ctx)
{
    // Repopulate the LightManager from the enabled light entities, in document
    // order, exactly like JsonLevel does at load.
    //
    // Adding a light (duplicate, or re-enabling a loaded-disabled one) can push the
    // light count past the GPU buffer capacity. The per-frame render path grows a
    // light buffer by FREEING the old resource and allocating a bigger one; doing
    // that while a frame is still in flight frees a resource the GPU is using ->
    // DXGI_ERROR_DEVICE_HUNG. So idle the GPU and pre-grow the buffers here, while
    // nothing references them; the render path then sees sufficient capacity and
    // never reallocates mid-flight. (Ensure*Buffer only grows, so disabling a light
    // keeps the existing buffer.)
    ctx.renderer.WaitForPreviousFrame();

    LightManager& lm = ctx.scene.GetLightManager();
    lm.SpotLights().clear();
    lm.PointLights().clear();
    for (const EditorObject& e : ctx.document.Environment())
    {
        const nlohmann::json& p = e.properties;
        if (!p.value("enabled", true)) { continue; }
        if (e.type == "spotLight")
        {
            SpotLightDesc d;
            d.position = JF3(p, "position", d.position);
            d.direction = JF3(p, "direction", d.direction).Normalized();
            d.range = JF(p, "range", d.range);
            d.innerAngle = JF(p, "innerAngleDeg", 15.0f) * kDeg2Rad;
            d.outerAngle = JF(p, "outerAngleDeg", 25.0f) * kDeg2Rad;
            d.color = JF3(p, "color", d.color);
            d.intensity = JF(p, "intensity", d.intensity);
            d.shadowNormalBias = JF(p, "shadowNormalBias", d.shadowNormalBias);
            d.shadowDepthBias = JF(p, "shadowDepthBias", d.shadowDepthBias);
            d.shadowsEnabled = p.value("shadowsEnabled", d.shadowsEnabled);
            lm.SpotLights().push_back({});
            lm.SpotLights().back().SetDesc(d);
        }
        else if (e.type == "pointLight")
        {
            PointLightDesc d;
            d.position = JF3(p, "position", d.position);
            d.radius = JF(p, "radius", d.radius);
            d.color = JF3(p, "color", d.color);
            d.intensity = JF(p, "intensity", d.intensity);
            d.shadowsEnabled = p.value("shadowsEnabled", d.shadowsEnabled);
            ParsePointLightFlicker(p, d);
            lm.PointLights().push_back({});
            lm.PointLights().back().SetDesc(d);
        }
    }
    lm.UpdateSpotLightCache();

    if (!lm.PointLights().empty())
    {
        lm.EnsurePointLightBuffer(&ctx.renderer, lm.PointLights().size());
    }
    if (lm.GetSpotLightCount() > 0)
    {
        lm.EnsureSpotLightBuffer(&ctx.renderer, lm.GetSpotLightCount());
    }
}

std::vector<std::string> EnvironmentRuntime::OceanPresets()
{
    std::vector<std::string> presets;
    const std::filesystem::path dir("data/ocean");
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) { return presets; }
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec) { break; }
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json")
        {
            presets.push_back(entry.path().generic_string());
        }
    }
    std::sort(presets.begin(), presets.end());
    return presets;
}

#endif // WITH_EDITOR
