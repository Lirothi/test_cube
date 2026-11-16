#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "app/settings/DisplayMode.h"
#include "app/settings/UserSettings.h"
#include "app/ui/GameMenuStrings.h"

class InputManager;

// The game's own look for its menus (docs/scalability_plan.md §6.1): a font sized by the window's height (a 1080 line
// is 20 pixels, 1440 27, 2160 40) whatever the panels' fixed 1.5 says, padding in proportion, a dark backdrop over the
// frame, its own colours. Pushed while it lives.
class GameMenuStyle
{
public:
    GameMenuStyle();
    ~GameMenuStyle();
    GameMenuStyle(const GameMenuStyle&) = delete;
    GameMenuStyle& operator=(const GameMenuStyle&) = delete;
    // `n` times the menu's font size, in pixels.
    float Em(float n) const { return em_ * n; }
    // The frame behind the menu darkened.
    void Backdrop() const;
    // A heading at 1.4 x the menu's size.
    void Title(const char* text) const;

private:
    float em_ = 20.0f;
    float unscaled_ = 20.0f;
    int colors_ = 0;
    int vars_ = 0;
};

// The player's settings (docs/scalability_plan.md §6), behind the game menu's "Settings":
//   Display  -- the window's mode (windowed / borderless), monitor, size; VSync, the frame-rate limit and the one in the
//               background. A change of the window asks "Keep these display settings?" and goes back after 15 s.
//   Graphics -- auto-detect, the preset (Low .. Ultra; Custom shows itself while the groups differ), the render
//               resolution, each group with what it buys; what was changed by hand over the levels; Advanced: a few
//               settings of the groups on their own.
//   Image    -- brightness (EV over the level's exposure), the field of view (Hor+).
//   Controls -- mouse sensitivity, inverted vertical look, mouse smoothing; the keys (to read).
// A change is applied at once, so it is seen behind the menu. Apply and Cancel show while something differs from what was
// there; Apply keeps it (user_settings.json), Cancel puts it back; Back keeps it and closes; Esc puts it back and closes.
class GameSettingsMenu
{
public:
    bool IsOpen() const { return open_; }
    void Open();
    // Draws it while open. Returns true on the frame it closes.
    bool Draw(InputManager& input);
    // Esc while it is open: everything as it was when it opened, and closed.
    void Cancel();
    // "--settings-menu[=<tab>]" (§8.5): open at boot on that tab (display, graphics, image, controls), for a --shot.
    void OpenTab(const std::string& tab);
    // Where Apply keeps the graphics section's part (VSync, the frame-rate limit, the mouse smoothing): the app's
    // GraphicsSettingsManager::SaveCurrent.
    void SetSaveGraphics(std::function<bool()> save) { saveGraphics_ = std::move(save); }

    // At boot, after the graphics settings: a 2026-11-13 file's VSync and frame-rate limit (they are the graphics
    // section's now; the display section keeps the window).
    static void ApplySavedDisplay();

private:
    struct Snapshot
    {
        std::vector<int> groups;
        int resolution = -1;
        std::map<std::string, float> overrides;
        bool vsync = false;
        int maxFps = 0;
        float mouseSmoothingMs = 0.0f;
        display::DisplaySettings window;
        settings::ImageSettings image;
        settings::ControlsSettings controls;
    };
    static Snapshot Take();
    void Restore(const Snapshot& s);
    static bool Same(const Snapshot& a, const Snapshot& b);
    bool SaveAll(std::string& why);
    void DrawDisplay();
    void DrawGraphics();
    void DrawImage();
    void DrawControls(InputManager& input);
    void DrawDisplayConfirm();
    // A change of the window: applied now, and confirmed or put back (§6.3).
    void ChangeWindow(const display::DisplaySettings& to);

    bool open_ = false;
    int requestedTab_ = -1; // 0 display, 1 graphics, 2 image, 3 controls
    Snapshot atOpen_;
    std::string status_;
    std::function<bool()> saveGraphics_;
    std::vector<display::MonitorInfo> monitors_;
    // The window's change waiting for Keep: what it was, and when it goes back.
    bool confirming_ = false;
    display::DisplaySettings confirmPrevious_;
    double confirmDeadline_ = 0.0;
};
