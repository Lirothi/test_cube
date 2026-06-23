#pragma once

#include "app/scene/SceneFrameData.h"
#include "app/OceanControlsWindow.h"
#include "rendering/debug/TextureDebugViewer.h"
#include "ui/ImGuiWindowUtils.h"

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

    void Draw(Renderer& renderer, const Scene& scene, const InputManager& input, LevelManager& levelManager, SceneRenderSettings& settings);

private:
    TextureDebugViewer textureDebugViewer_;
    OceanControlsWindow oceanControlsWindow_;
    ui::ImGuiWindowMaximizeState windowMaximize_;
    bool open_ = false;
};
