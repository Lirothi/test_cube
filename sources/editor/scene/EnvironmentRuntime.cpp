#include "editor/scene/EnvironmentRuntime.h"
#include "app/levels/LevelEnvironment.h" // R9.4: the level loader's road for lights and sections
#include "core/vfs/Vfs.h"
#include "rendering/core/PhotographicSettings.h" // P16.5 lumens migration
#include "app/scene/GtaoSettingsJson.h"
#include "app/scene/HeightFogSettingsJson.h"
#include "app/scene/BloomSettingsJson.h"
#include "app/scene/SkyAtmosphereSettingsJson.h"
#include "app/scene/VolumetricCloudSettingsJson.h"
#include "app/scene/TimeOfDaySettingsJson.h"
#if WITH_EDITOR

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/Systems.h"
#include "rendering/view/Camera.h"
#include "app/settings/Scalability.h"
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
#include "rendering/wind/WindState.h"

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
        return vfs::Widen(s);
    }

    void ApplySkybox(EditorContext& ctx, const nlohmann::json& p)
    {
        const std::string texture = p.value("texture", std::string());

        // R16.10: Apply parked the render thread; the old sky goes through render::deferred (Scene::SetSkybox) and the
        // new one's upload with the next hand-over -- no GPU idle (it was a WaitForPreviousFrame and a SubmitAndWait).
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
        // P16.3b: before Init(), which is what measures the sky and logs the resulting scale.
        skybox->SetIlluminanceLux(JF(p, "illuminanceLux", 0.0f));
        skybox->Init(&ctx.renderer, uploads.CommandList(), uploads.KeepAlive());
        (void)uploads.Submit(&ctx.renderer);
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

        // Direction/strength are also the ocean's source of truth. ResetInitialSpectrum retires the simulation's GPU
        // resources -- to render::deferred since R16.7 -- and Apply parked the render half first (the simulation is its
        // object): no GPU idle (R16.10). Gust/sway-only edits avoid touching the FFT and apply on the next Scene::Tick.
        if (oceanDriveChanged)
        {
            if (OceanSimulation* ocean = Systems::GetOceanSimulation())
            {
                threading::WaitRenderForGame("EnvironmentRuntime: the ocean's wind"); // Apply's park; this says why
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
    // Game thread plan G8b: an environment object reaches the render half's own objects (the sky's LUT settings, the
    // ocean simulation, the skybox's textures): the render thread's frame in flight finishes first.
    threading::WaitRenderForGame("EnvironmentRuntime::Apply");
    const nlohmann::json& p = env.properties;
    // The level's own clouds, fog, bloom, GTAO, sun were re-applied from the document -- each struct whole, so their
    // knobs now hold the level's values, whatever the graphics levels wrote before (an Inspector that unticks GTAO at Low
    // writes the 0 the ceiling wrote: without forgetting, Medium would turn it back on). Then the ceilings over the new
    // values (docs/scalability_plan.md §4.2), whichever way this function leaves.
    struct ScalabilityAfter
    {
        std::vector<std::string> knobs;
        ~ScalabilityAfter()
        {
            if (knobs.empty()) { return; }
            settings::TheScalability().ForgetContent(knobs);
            settings::TheScalability().Apply("environment edit", /*quiet=*/true);
        }
    } scalabilityAfter{ env.type == "gtao" ? std::vector<std::string>{ "gtao." }
                        : env.type == "heightFog" ? std::vector<std::string>{ "fog." }
                        : env.type == "bloom" ? std::vector<std::string>{ "bloom." }
                        : env.type == "postProcess" ? std::vector<std::string>{ "gtao.", "fog.", "bloom." }
                        : env.type == "volumetricCloud" ? std::vector<std::string>{ "cloud." }
                        : env.type == "timeOfDay" || env.type == "directionalLight" ? std::vector<std::string>{ "lightShafts." }
                        : std::vector<std::string>{} };

    if (env.type == "pointLight")
    {
        const int i = LightManagerIndexFor(ctx.document, env);
        auto& points = ctx.scene.GetLightManager().PointLights();
        if (i < 0 || i >= static_cast<int>(points.size())) { return; }
        PointLightDesc d;
        levelenv::ReadPointLight(p, d, levelenv::JsonRead::Tolerant); // R9.4: the level's reader too
        points[i].SetDesc(d);
    }
    else if (env.type == "spotLight")
    {
        const int i = LightManagerIndexFor(ctx.document, env);
        auto& spots = ctx.scene.GetLightManager().SpotLights();
        if (i < 0 || i >= static_cast<int>(spots.size())) { return; }
        SpotLightDesc d;
        levelenv::ReadSpotLight(p, d, levelenv::JsonRead::Tolerant); // R9.4: the level's reader too
        spots[i].SetDesc(d);
    }
    else if (env.type == "timeOfDay")
    {
        // T3: the same reader as the level's (TimeOfDaySettingsJson); a running clock keeps its place
        // unless the edit moved the authored time (Scene::ApplyTimeOfDay).
        TimeOfDaySettings settings{};
        DirectionalLight sun = TimeOfDaySettingsJson::DefaultSun();
        TimeOfDaySettingsJson::ApplyOverrides(p, settings, sun);
        ctx.scene.ApplyTimeOfDay(settings, sun);
    }
    else if (env.type == "cameraExposure")
    {
        // P1: rebuilt from defaults on every edit rather than patched in place, so clearing a
        // field in the inspector returns it to the documented default instead of keeping whatever
        // the previous edit left behind. R9.5: the P3B fields an older level kept in its colour
        // pipeline were moved onto the camera when the document was read (levelmigrate::Migrate).
        levelenv::ApplyCameraExposure(ctx.scene, &p); // R9.4: the level loader's road
    }
    else if (env.type == "gtao")
    {
        // P6B: rebuilt from defaults on every edit, same reasoning as the two blocks around it --
        // a field the document does not carry must land on the struct default, not on whatever the
        // previous edit left behind.
        levelenv::ApplyGtao(ctx.scene, &p); // R9.4
    }
    else if (env.type == "heightFog")
    {
        // P7: rebuilt from defaults on every edit, same reasoning as the blocks around it -- a
        // field the document does not carry must land on the struct default, not on whatever the
        // previous edit left behind.
        levelenv::ApplyHeightFog(ctx.scene, &p); // R9.4
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
        // R9.4: the level loader's road, group by group. R9.5: the colour pipeline's old P3B local-exposure keys are
        // the camera's in the document already (levelmigrate::Migrate) -- before, an edit here dropped them where only
        // the colour pipeline still carried them, while the loader lifted them, and the level looked other than it loaded.
        levelenv::ApplyCameraExposure(ctx.scene, &sub("cameraExposure"));
        levelenv::ApplyColorPipeline(ctx.scene, &sub("colorPipeline"));
        levelenv::ApplyGtao(ctx.scene, &sub("gtao"));
        levelenv::ApplyHeightFog(ctx.scene, &sub("heightFog"));
        levelenv::ApplyBloom(ctx.scene, &sub("bloom"));
    }
    else if (env.type == "bloom")
    {
        // P8: rebuilt from defaults on every edit, same reasoning as the blocks around it -- a
        // field the document does not carry must land on the struct default, not on whatever the
        // previous edit left behind.
        levelenv::ApplyBloom(ctx.scene, &p); // R9.4
    }
    else if (env.type == "colorPipeline")
    {
        // P3C: rebuilt from defaults on every edit, same reasoning as cameraExposure above.
        levelenv::ApplyColorPipeline(ctx.scene, &p); // R9.4
        // R9.5: no camera re-apply here any more -- the local-exposure keys an older level kept in its colour
        // pipeline are the camera's in the document (levelmigrate::Migrate), and this object's keys reach no camera.
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
    else if (env.type == "skyAtmosphere")
    {
        levelenv::ApplySkyAtmosphere(ctx.scene, &p); // R9.4
    }
    else if (env.type == "volumetricCloud")
    {
        levelenv::ApplyVolumetricCloud(ctx.scene, &p); // R9.4
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
    threading::WaitRenderForGame("EnvironmentRuntime::ApplyChange"); // as Apply
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
    threading::WaitRenderForGame("EnvironmentRuntime::Remove"); // as Apply
    if (env.type == "pointLight" || env.type == "spotLight")
    {
        RebuildLights(ctx);
    }
    else if (env.type == "timeOfDay")
    {
        // No time of day: no sun, no moon -- the light goes dark where it stands (as removing the old
        // Directional Light did), and the clock stops owning it.
        DirectionalLight dark;
        dark.SetDirection(ctx.scene.GetDirectionalLight().GetDirection());
        dark.SetColor(Math::float3(0.0f, 0.0f, 0.0f));
        dark.SetAmbient(0.0f);
        ctx.scene.SetTimeOfDay(TimeOfDaySettings{}, TimeOfDaySettingsJson::DefaultSun());
        ctx.scene.SetDirectionalLight(dark);
    }
    else if (env.type == "skybox")
    {
        ctx.scene.SetSkybox(nullptr); // R16.10: the old sky through render::deferred; parked above
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
    else if (env.type == "heightFog")
    {
        ctx.scene.SetHeightFog(HeightFogSettings{});
    }
    else if (env.type == "colorPipeline")
    {
        ctx.scene.SetColorPipeline(render::ColorPipelineSettings{});
    }
    else if (env.type == "wind")
    {
        ctx.scene.GetWindState() = vfx::WindState{};
    }
    else if (env.type == "skyAtmosphere")
    {
        // Removing the atmosphere returns the level to its cubemap -- the state a level without
        // the section loads with.
        ctx.scene.SetSkyAtmosphere(SkyAtmosphereSettings{});
    }
    else if (env.type == "volumetricCloud")
    {
        ctx.scene.SetVolumetricCloud(VolumetricCloudSettings{}); // no section = no clouds
    }
}

void EnvironmentRuntime::RebuildLights(EditorContext& ctx)
{
    // Repopulate the LightManager from the enabled light entities, in document
    // order, exactly like JsonLevel does at load.
    //
    // The light lists below are what the render thread's frame walks: park it (the game half's rule). A grow of the
    // light rings hands the old buffers to render::deferred (LightManager::Release*LightBuffer), so no GPU idle: this
    // was a WaitForPreviousFrame -- once per light of a group switched together, 212 for wind_test's "Lamps" (R16.10).
    // A second park in the same frame returns at once. (Ensure*Buffer only grows, so disabling a light keeps the
    // existing buffer.)
    threading::ParkRenderForGame("EnvironmentRuntime::RebuildLights");

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
            levelenv::ReadSpotLight(p, d, levelenv::JsonRead::Tolerant); // R9.4: the level's reader too
            lm.SpotLights().push_back({});
            lm.SpotLights().back().SetDesc(d);
        }
        else if (e.type == "pointLight")
        {
            PointLightDesc d;
            levelenv::ReadPointLight(p, d, levelenv::JsonRead::Tolerant); // R9.4: the level's reader too
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
