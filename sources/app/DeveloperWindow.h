#pragma once

#include <string>
#include <vector>

#include "app/scene/SceneFrameData.h"
#include "ocean/OceanControlsWindow.h"
#include "rendering/debug/TextureDebugViewer.h"
#include "ui/ImGuiWindowUtils.h"

#if WITH_EDITOR
class EditorController;
#endif
class InputManager;
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

    void Draw(Renderer& renderer, const Scene& scene, const InputManager& input, LevelManager& levelManager, SceneRenderSettings& settings
#if WITH_EDITOR
        , EditorController& editorController
#endif
    );

private:
    void RefreshLevelList();

    TextureDebugViewer textureDebugViewer_;
    OceanControlsWindow oceanControlsWindow_;
    ui::ImGuiWindowMaximizeState windowMaximize_;
    std::vector<std::string> availableLevelPaths_;
    std::string levelChangeStatus_;
    char levelPathBuffer_[1024] = "data/levels/demo.json";
    bool levelListScanned_ = false;
    bool preserveCameraOnLevelChange_ = false;
    bool open_ = false;
};
