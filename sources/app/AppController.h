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
    void BuildHud(Renderer& renderer, const Scene& scene) const;

    const SceneRenderSettings& Settings() const { return settings_; }

private:
    SceneRenderSettings settings_{};
};
