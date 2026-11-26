#pragma once

#include <optional>
#include <string>
#include <vector>

#if TC_SHIPPING
// A shipped build has no Developer Controls (docs/scalability_plan.md §7.1, §10 item 12): the class is this stub, its
// 4000 lines and the windows it owns (the ocean's, the texture inspector, the log viewer, the memory tab) are not
// compiled in -- the linker drops what nothing reaches. The same interface, every call a no-op.
#if WITH_EDITOR
class EditorController;
#endif
class InputManager;
class GraphicsSettingsManager;
class LevelManager;
class Renderer;
class Scene;
struct SceneRenderSettings;

class DeveloperWindow
{
public:
    bool IsOpen() const { return false; }
    void SetOpen(bool) {}
    void ToggleOpen() {}
    void ToggleTextureInspector() {}
    void ReleaseGpuResources() {}
    void ToggleOceanControls() {}
    void OpenOceanControls() {}
    void OpenMemoryTab() {}
    bool OpenTab(const std::string&) { return false; }
    bool IsLogWindowOpen() const { return false; }
    bool AnyWindowOpen() const { return false; }
    void ToggleLogWindow() {}
    bool Draw(Renderer&, Scene&, const InputManager&, LevelManager&, SceneRenderSettings&, GraphicsSettingsManager&) { return false; }
};
#else

#include "rendering/scene/SceneFrameData.h"
#include "app/ui/LogWindow.h"
#include "app/ui/MemoryTab.h"
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
    // Shutdown, the GPU idle: the reset-icon atlas goes before the device does (the window itself outlives it).
    void ReleaseGpuResources()
    {
        resetIconAtlas_.reset();
        resetIconAtlasTried_ = false;
        resetIconAtlasReady_ = false;
    }
    void ToggleOceanControls() { oceanControlsWindow_.ToggleOpen(); }
    void OpenOceanControls() { oceanControlsWindow_.SetOpen(true); }
    // "--mem-window": the window open on the Memory tab, so a headless --shot can photograph it.
    void OpenMemoryTab() { open_ = true; activeTab_ = Tab::Memory; }
    // "--dev-tab=<label>": the window open on the tab of that label (case-insensitive: "gi", "lod"), for the
    // same reason. False (and the window left as it was) when no tab has that label.
    bool OpenTab(const std::string& label);
    bool IsLogWindowOpen() const { return logWindow_.IsOpen(); }
    // A window of ours is on screen, and it wants the mouse: the play session lets it go.
    bool AnyWindowOpen() const { return open_ || logWindow_.IsOpen() || oceanControlsWindow_.IsOpen() || textureDebugViewer_.IsOpen(); }
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
        Scalability, Frame, AntiAliasing, Visibility, Reflections, Gi, Fog, Sky,
        Debug, Lod, Csm, Contact, Vsm, Tessy, Physics, Streaming, Memory, Bindings, Settings
    };
    struct TabEntry { Tab tab; const char* label; };
    // The navigation list, in its order: Draw's and OpenTab's one table.
    static const TabEntry kTabs[19];
    // The graphics levels (docs/scalability_plan.md §4.5): the levels of the groups, the knobs each sets, the last apply.
    void DrawScalabilityTab();
    // Every --set name of the settings registry (architecture plan R2.n): a widget by its type, through the row.
    // True when a persisted setting changed (the graphics settings save it as the other tabs' edits).
    bool DrawSettingsTab(Scene& scene, SceneRenderSettings& settings);
    char settingsFilter_[64] = {};
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
    std::optional<Texture2D> resetIconAtlas_; // made on first use, reset by ReleaseGpuResources
    OceanControlsWindow oceanControlsWindow_;
    LogWindow logWindow_;
    MemoryTab memoryTab_;
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
#endif // TC_SHIPPING
