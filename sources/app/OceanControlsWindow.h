#pragma once

#include "ui/ImGuiWindowUtils.h"

class Renderer;

class OceanControlsWindow
{
public:
    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    void ToggleOpen() { open_ = !open_; }

    void Draw(Renderer& renderer);

private:
    ui::ImGuiWindowMaximizeState maximize_;
    bool open_ = false;
};
