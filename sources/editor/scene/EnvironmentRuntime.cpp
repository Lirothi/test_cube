#include "editor/scene/EnvironmentRuntime.h"
#if WITH_EDITOR

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "app/Systems.h"
#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "ocean/OceanSimulation.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/scene/EditorSceneDocument.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/PointLight.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/SpotLight.h"

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
        ctx.scene.SetSkybox(std::move(skybox));
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
        dl.SetExposure(JF(p, "exposure", 1.0f));
        dl.SetAmbient(enabled ? JF(p, "ambient", 0.05f) : 0.0f);
        ctx.scene.SetDirectionalLight(dl);
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
        // Live wind overrides (the "scene" block). Defaults are the sim's current
        // values, so an absent override key is a no-op. Preset/enable are handled
        // by the ocean inspector + the Ocean menu, not here (they are discrete).
        if (OceanSimulation* ocean = Systems::GetOceanSimulation())
        {
            const float windDir = JF(p, "windDirectionDeg", ocean->GetLocalWindDirectionDegrees());
            const float swellDir = JF(p, "swellDirectionDeg", ocean->GetSwellDirectionDegrees());
            const float windForce = JF(p, "windForce", ocean->GetWindForce01());
            ocean->SetSceneVariables(&ctx.renderer, windDir, swellDir, windForce);
        }
    }
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

void EnvironmentRuntime::SetEnabled(EditorContext& ctx, EditorObject& env, bool enabled)
{
    env.properties["enabled"] = enabled;
    if (env.type == "spotLight" || env.type == "pointLight")
    {
        RebuildLights(ctx);
    }
    else if (env.type == "directionalLight")
    {
        Apply(ctx, env);
    }
    else if (env.type == "ocean")
    {
        ctx.scene.SetOceanVisible(enabled);
    }
    ctx.document.SetDirty(true);
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
