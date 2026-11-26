#include "app/settings/DisplayMode.h"
#include "app/settings/SettingContext.h"
#include "app/settings/UserSettings.h"
#include "core/settings/SettingsRegistry.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

// --set / --sweep / MCP set_settings names display.*, image.*, controls.* (the player's Display, Image and Controls,
// docs/scalability_plan.md S6 / S7), architecture plan R2.2. One branch of App.cpp's ApplySweepValue answered several
// names by testing `setting`; it is a function here, each name a row that calls it -- the body as it was.
namespace settings
{
namespace
{
// The window (docs/scalability_plan.md S6): the menu's Display, live and transient -- a script switches the mode
// (0 windowed, 1 borderless), the windowed client size, the monitor (its place in the list, 1 = the first), the
// frame-rate limit in the background (0 none, -1 the build's). Taken at the frame's safe point; never saved.
bool RequestDisplay(std::string_view setting, float value)
{
    display::DisplaySettings d = display::PendingOrCurrent(display::MainWindow());
    if (setting == "display.mode") { d.mode = value >= 0.5f ? display::WindowMode::Borderless : display::WindowMode::Windowed; }
    else if (setting == "display.width") { d.width = std::clamp(static_cast<int>(value), 320, 16384); }
    else if (setting == "display.height") { d.height = std::clamp(static_cast<int>(value), 200, 16384); }
    else
    {
        const std::vector<display::MonitorInfo> monitors = display::Monitors();
        const int index = static_cast<int>(value) - 1;
        if (index < 0 || index >= static_cast<int>(monitors.size())) { return false; }
        d.monitor = monitors[static_cast<std::size_t>(index)].device;
    }
    display::Request(d);
    return true;
}

// The window and the player's settings (S6, S7): what they are now.
bool ReadDisplay(std::string_view name, float& out)
{
    const display::DisplaySettings d = display::Current(display::MainWindow());
    if (name == "display.mode") { out = d.mode == display::WindowMode::Borderless ? 1.0f : 0.0f; return true; }
    if (name == "display.width") { out = static_cast<float>(static_cast<std::uint32_t>(d.width)); return true; }
    if (name == "display.height") { out = static_cast<float>(static_cast<std::uint32_t>(d.height)); return true; }
    const std::vector<display::MonitorInfo> monitors = display::Monitors();
    out = static_cast<float>(static_cast<std::uint32_t>(display::FindMonitor(monitors, d.monitor) + 1));
    return true;
}

// The player's Image and Controls (S7), transient: brightness in EV, the field of view (0 = the level's), the
// look's sensitivity and its inverted vertical.
bool SetImage(std::string_view setting, float value)
{
    settings::ImageSettings i = settings::CurrentImageSettings();
    if (setting == "image.brightnessEv") { i.brightnessEv = value; }
    else { i.fovDeg = value; }
    settings::ApplyImageSettings(i);
    return true;
}

bool SetControls(std::string_view setting, float value)
{
    settings::ControlsSettings c = settings::CurrentControlsSettings();
    if (setting == "controls.sensitivity") { c.sensitivity = value; }
    else { c.invertY = value != 0.0f; }
    settings::ApplyControlsSettings(c);
    return true;
}

const SettingDesc kDisplayRows[] = {
    TC_SETTING("display.mode", Int, SettingTransient,
               [](SettingContext&, float value) { return RequestDisplay("display.mode", value); },
               [](SettingContext&, float& out) { return ReadDisplay("display.mode", out); }),
    TC_SETTING("display.width", Int, SettingTransient,
               [](SettingContext&, float value) { return RequestDisplay("display.width", value); },
               [](SettingContext&, float& out) { return ReadDisplay("display.width", out); }),
    TC_SETTING("display.height", Int, SettingTransient,
               [](SettingContext&, float value) { return RequestDisplay("display.height", value); },
               [](SettingContext&, float& out) { return ReadDisplay("display.height", out); }),
    TC_SETTING("display.monitor", Int, SettingTransient,
               [](SettingContext&, float value) { return RequestDisplay("display.monitor", value); },
               [](SettingContext&, float& out) { return ReadDisplay("display.monitor", out); }),
    TC_SETTING("display.backgroundFps", Int, SettingTransient,
               [](SettingContext&, float value) { display::SetBackgroundFps(static_cast<int>(value)); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(display::BackgroundFps()); return true; }),
    TC_SETTING("image.brightnessEv", Float, SettingTransient,
               [](SettingContext&, float value) { return SetImage("image.brightnessEv", value); },
               [](SettingContext&, float& out) { out = settings::CurrentImageSettings().brightnessEv; return true; }),
    TC_SETTING("image.fovDeg", Float, SettingTransient,
               [](SettingContext&, float value) { return SetImage("image.fovDeg", value); },
               [](SettingContext&, float& out) { out = settings::CurrentImageSettings().fovDeg; return true; }),
    TC_SETTING("controls.sensitivity", Float, SettingTransient,
               [](SettingContext&, float value) { return SetControls("controls.sensitivity", value); },
               [](SettingContext&, float& out) { out = settings::CurrentControlsSettings().sensitivity; return true; }),
    TC_SETTING("controls.invertY", Bool, SettingTransient,
               [](SettingContext&, float value) { return SetControls("controls.invertY", value); },
               [](SettingContext&, float& out) { out = settings::CurrentControlsSettings().invertY ? 1.0f : 0.0f; return true; }),
};
TC_SETTINGS(kDisplayRows);
}
}
