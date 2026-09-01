#pragma once

#include <string>

#include "app/scene/SceneRenderConfig.h"

class Renderer;
class Scene;
struct SceneRenderSettings;

// Persists project-wide graphics quality. Scene-authored look settings (GTAO, atmosphere, bloom,
// exposure and colour grading) stay in the level. The CSM fit has no level-serialization path, so
// it is a project default here and is re-applied whenever a level is loaded.
class GraphicsSettingsManager
{
public:
    static constexpr const char* kPath = "graphics_settings.json";

    void Initialize(Renderer& renderer, Scene& scene, SceneRenderSettings& settings);
    void ApplySceneSettings(Scene& scene) const;

    void ObserveUiEdit(Renderer& renderer, Scene& scene,
                       const SceneRenderSettings& settings,
                       bool graphicsSettingsDirty,
                       float deltaTime);
    void Flush(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);

    bool SaveCurrent(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);
    bool Reload(Renderer& renderer, Scene& scene, SceneRenderSettings& settings);
    bool ResetAll(Renderer& renderer, Scene& scene, SceneRenderSettings& settings);
    bool ResetRender(Renderer& renderer, const Scene& scene, SceneRenderSettings& settings);
    bool ResetLod(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);
    bool ResetCsm(Renderer& renderer, Scene& scene, const SceneRenderSettings& settings);
    bool ResetContactShadows(Renderer& renderer, const Scene& scene,
                             const SceneRenderSettings& settings);
    bool ResetVsm(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);

    const std::string& Status() const { return status_; }

private:
    bool initialized_ = false;
    bool savePending_ = false;
    float saveDelaySeconds_ = 0.0f;
    std::string status_;
    CascadeShadowConfig projectCsm_{};
};
