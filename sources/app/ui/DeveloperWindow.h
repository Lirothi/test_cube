#pragma once

#include <string>
#include <vector>

#include "app/scene/SceneFrameData.h"
#include "app/ui/LogWindow.h"
#include "materials/Texture2D.h"
#include "ocean/OceanControlsWindow.h"
#include "rendering/debug/TextureDebugViewer.h"
#include "ui/ImGuiWindowUtils.h"

#if WITH_EDITOR
class EditorController;
#endif
class InputManager;
class GraphicsSettingsManager;
class LevelManager;
class Renderer;
class Scene;

class DeveloperWindow
{
public:
    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    void ToggleOpen() { open_ = !open_; }
    void ToggleTextureInspector();
    void ToggleOceanControls() { oceanControlsWindow_.ToggleOpen(); }
    void OpenOceanControls() { oceanControlsWindow_.SetOpen(true); }
    bool IsLogWindowOpen() const { return logWindow_.IsOpen(); }
    void ToggleLogWindow() { logWindow_.ToggleOpen(); }

    // Tabs edit scene settings live, including shadows and atmosphere previews.
    bool Draw(Renderer& renderer, Scene& scene, const InputManager& input, LevelManager& levelManager,
        SceneRenderSettings& settings, GraphicsSettingsManager& graphicsSettings
#if WITH_EDITOR
        , EditorController& editorController
#endif
    );

private:
    enum class Tab
    {
        Frame, AntiAliasing, Visibility, Reflections, Fog, Sky,
        Debug, Lod, Csm, Contact, Vsm, Bindings
    };
    Tab activeTab_ = Tab::Frame;

    void RefreshLevelList();
    void ApplySunElevation(Scene& scene) const;
    float sunElevationMin_ = 0.0f;
    float sunElevationMax_ = 90.0f;
    float sunElevationSeconds_ = 10.0f;
    double sunElevationProgress_ = 0.0;
    double sunElevationAzimuth_ = 0.0;
    bool sunElevationPlaying_ = false;
    // Start/Stop trace capture. Its own window rather than a tab, so it stays reachable while the
    // ocean/other windows have focus — the stalls worth capturing happen WHILE dragging something
    // else, and a fixed frame count forces you to guess the length in advance.
    void DrawTraceControls();

    TextureDebugViewer textureDebugViewer_;
    Texture2D resetIconAtlas_;
    OceanControlsWindow oceanControlsWindow_;
    LogWindow logWindow_;
    ui::ImGuiWindowMaximizeState windowMaximize_;
    std::vector<std::string> availableLevelPaths_;
    std::string levelChangeStatus_;
    char levelPathBuffer_[1024] = "data/levels/demo.json";
    bool levelListScanned_ = false;
    bool preserveCameraOnLevelChange_ = false;
    bool traceWindowOpen_ = false;
    bool resetIconAtlasTried_ = false;
    bool resetIconAtlasReady_ = false;
    bool open_ = false;
};
