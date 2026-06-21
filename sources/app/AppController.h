#pragma once

#include "app/scene/SceneFrameData.h"
#include "core/task/TaskSystem.h"
#include "rendering/debug/TextureDebugViewer.h"
#include "ui/ImGuiWindowUtils.h"

class InputManager;
class Renderer;
class Scene;

// App-level debug controls: maps input actions to render/debug settings
// (SSR technique, FXAA, DLSS, wireframe, profiler/debug overlays) and builds
// the on-screen HUD text. Ticks once per frame before Scene::Render.
class AppController
{
public:
    void Tick(InputManager& input, Renderer& renderer, Scene& scene, float deltaTime);
    void BuildHud(Renderer& renderer, const Scene& scene) const;
    void WaitForHudBuild();

    const SceneRenderSettings& Settings() const { return settings_; }

private:
    void ScheduleHudBuild(Renderer& renderer, const Scene& scene);
    void BuildDeveloperWindow(Renderer& renderer, const Scene& scene, const InputManager& input);

    SceneRenderSettings settings_{};
    TaskSystem::TaskHandle hudBuildTask_ = nullptr;
    TextureDebugViewer textureDebugViewer_;
    ui::ImGuiWindowMaximizeState developerWindowMaximize_;
    bool showDeveloperWindow_ = false;
};
