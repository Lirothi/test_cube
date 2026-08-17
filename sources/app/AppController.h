#pragma once

#include "app/ui/DeveloperWindow.h"
#include "app/scene/SceneFrameData.h"
#include "core/task/TaskSystem.h"
#if WITH_EDITOR
#include "editor/EditorController.h"
#endif

class InputManager;
class LevelManager;
class Renderer;
class Scene;
struct LevelChangeRequest;

// App-level debug controls: maps input actions to render/debug settings
// (SSR technique, FXAA, DLSS, wireframe, profiler/debug overlays) and builds
// the on-screen HUD text. Ticks once per frame before Scene::Render.
class AppController
{
public:
    void Tick(InputManager& input, Renderer& renderer, Scene& scene, LevelManager& levelManager, float deltaTime);
    void BuildHud(Renderer& renderer, const Scene& scene) const;
    void WaitForHudBuild();
#if WITH_EDITOR
    void OnLevelChangeRequestCompleted(const LevelChangeRequest& request,
        bool loaded,
        Renderer& renderer,
        Scene& scene,
        LevelManager& levelManager);
#endif

    const SceneRenderSettings& Settings() const { return settings_; }
    // The app layer OWNS these and re-pushes them into the Scene every Tick, so anything that
    // wants a live change (the --sweep harness, the developer window) has to write HERE.
    // Writing Scene::RenderSettingsRef would survive exactly one frame.
    SceneRenderSettings& SettingsRef() { return settings_; }

private:
    void ScheduleHudBuild(Renderer& renderer, const Scene& scene);

    SceneRenderSettings settings_{};
    TaskSystem::TaskHandle hudBuildTask_ = nullptr;
    DeveloperWindow developerWindow_;
#if WITH_EDITOR
    EditorController editorController_;
#endif
};
