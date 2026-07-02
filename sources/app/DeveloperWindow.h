#pragma once

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

    void Draw(Renderer& renderer, const Scene& scene, const InputManager& input, LevelManager& levelManager, SceneRenderSettings& settings
#if WITH_EDITOR
        , EditorController& editorController
#endif
    );

private:
    TextureDebugViewer textureDebugViewer_;
    OceanControlsWindow oceanControlsWindow_;
    ui::ImGuiWindowMaximizeState windowMaximize_;
    bool open_ = false;
};
