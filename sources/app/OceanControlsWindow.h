#pragma once

#include <string>
#include <vector>

#include "ui/ImGuiWindowUtils.h"

class Renderer;
class OceanSimulation;

class OceanControlsWindow
{
public:
    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    void ToggleOpen() { open_ = !open_; }

    void Draw(Renderer& renderer);

private:
    void RefreshConfigFiles(const OceanSimulation& ocean);
    void DrawConfigControls(Renderer& renderer, OceanSimulation& ocean);

    ui::ImGuiWindowMaximizeState maximize_;
    std::vector<std::wstring> configPaths_;
    int selectedConfigIndex_ = -1;
    bool configFilesInitialized_ = false;
    char saveAsName_[128] = "default.json";
    std::string configStatus_;
    bool open_ = false;
};
