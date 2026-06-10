#pragma once

#include "app/scene/SceneFrameData.h"

class InputManager;
class Renderer;
class Scene;

// App-level debug controls: maps input actions to render/debug settings
// (SSR technique, FXAA, DLSS, wireframe, profiler/debug overlays) and builds
// the on-screen HUD text. Ticks once per frame before Scene::Render.
class AppController
{
public:
    void Tick(InputManager& input, Renderer& renderer, Scene& scene);
    void BuildHud(Renderer& renderer, const Scene& scene, const InputManager& input) const;

    const SceneRenderSettings& Settings() const { return settings_; }

private:
    void BuildBindingsOverlay(Renderer& renderer, const InputManager& input) const;

    SceneRenderSettings settings_{};
    bool showBindings_ = false;
};
