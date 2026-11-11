#pragma once

#include "app/GraphicsSettings.h"
#include "app/ui/DeveloperWindow.h"
#include "app/game/PlaySession.h"
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
    void InitializeGraphicsSettings(Renderer& renderer, Scene& scene);
    void ApplySceneGraphicsSettings(Scene& scene) const;
    void FlushGraphicsSettings(Renderer& renderer, const Scene& scene);
    // Shutdown, the GPU idle, before Renderer::Shutdown: the UI's own GPU textures go while the device is there.
    void ReleaseGpuResources(Renderer& renderer);
    void Tick(InputManager& input, Renderer& renderer, Scene& scene, LevelManager& levelManager, float deltaTime);
    // After a level load: the play session goes on from the new level's Player Start, or ends if it was the editor's.
    void OnLevelLoaded(Renderer& renderer, Scene& scene, InputManager& input);
    void BuildHud(Renderer& renderer, const Scene& scene);
    void WaitForHudBuild();
    // Game thread plan G6: the HUD's lines (formatted by the build task, the game half's) into the render half's text
    // buffer -- at the hand-over (App's Sync).
    void PublishHud(Renderer& renderer);
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
    // The play session's keys and requests (docs/play_mode_plan.md), and what it says to the world this frame.
    void TickPlay(InputManager& input, Renderer& renderer, Scene& scene, LevelManager& levelManager, bool editorKeyPressed);

    SceneRenderSettings settings_{};
    GraphicsSettingsManager graphicsSettings_;
    TaskSystem::TaskHandle hudBuildTask_ = nullptr;
    // The level's time of day for the HUD line, copied when the build is scheduled: the build runs
    // beside Scene::Tick, which advances the clock. < 0 = the level has no time of day.
    float hudTimeOfDayHours_ = -1.0f;
    struct HudLine
    {
        int x = 0;
        int y = 0;
        float px = 16.0f;
        float4 color{};
        std::wstring text;
    };
    std::vector<HudLine> hudLines_; // G6: the build task's output, published at the hand-over
    PlaySession playSession_;
    bool editorOpenBeforePlay_ = false;
    std::wstring hudPlayLine_; // written in Tick, before the build is scheduled
    std::wstring hudRaceLine_;
    std::wstring hudBigText_; // the race's countdown, in the middle of the screen
    DeveloperWindow developerWindow_;
#if WITH_EDITOR
    EditorController editorController_;
#endif
};
