#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ui/ImGuiWindowUtils.h"

class EqualizerPreset;
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
    bool LoadConfigAtIndex(Renderer& renderer, OceanSimulation& ocean, int configIndex);

    ui::ImGuiWindowMaximizeState maximize_;
    std::vector<std::wstring> configPaths_;
    int selectedConfigIndex_ = -1;
    int pendingConfigLoadIndex_ = -1;
    float pendingConfigLoadPopupX_ = 0.0f;
    float pendingConfigLoadPopupY_ = 0.0f;
    bool configFilesInitialized_ = false;
    bool configDirty_ = false;
    std::vector<std::shared_ptr<EqualizerPreset>> localEqualizerBackups_;
    char saveAsName_[128] = "default.json";
    std::string configStatus_;
    bool open_ = false;
};
