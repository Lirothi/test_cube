#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app/scene/SceneFrameData.h"
#include "core/task/TaskSystem.h"

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
    void BuildHud(Renderer& renderer, const Scene& scene, const InputManager& input) const;
    void WaitForHudBuild();
    void BuildDebugUi(Renderer& renderer);

    const SceneRenderSettings& Settings() const { return settings_; }

private:
    struct BindingsOverlayCache {
        std::size_t signature = 0;
        float regionWidth = 0.0f;
        std::wstring title;
        std::vector<std::wstring> lines;
    };

    void ScheduleHudBuild(Renderer& renderer, const Scene& scene, const InputManager& input);
    void BuildBindingsOverlay(Renderer& renderer, const InputManager& input) const;

    SceneRenderSettings settings_{};
    TaskSystem::TaskHandle hudBuildTask_ = nullptr;
    mutable BindingsOverlayCache bindingsOverlayCache_{};
    bool showBindings_ = false;
};
