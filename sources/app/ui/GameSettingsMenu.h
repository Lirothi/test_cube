#pragma once

#include <string>
#include <vector>

// The player's settings (docs/scalability_plan.md §6), behind the game menu's "Settings": Graphics -- the preset (Low ..
// Ultra; Custom shows itself while the groups differ), the render resolution, each group with what it buys -- and
// Display -- VSync, the frame-rate limit. A change is applied at once, so it is seen behind the menu; Done keeps it
// (user_settings.json), Cancel puts back what was there when the screen opened.
//
// The display section of user_settings.json is read at boot (ApplySavedDisplay), after graphics_settings.json: what the
// player chose wins over the build's defaults.
class GameSettingsMenu
{
public:
    bool IsOpen() const { return open_; }
    void Open();
    // Draws it while open. Returns true on the frame it closes.
    bool Draw();
    // Esc while it is open: as Cancel.
    void Cancel();
    // "--settings-menu[=<tab>]" (§8.5): open at boot on that tab (graphics, display), for a --shot.
    void OpenTab(const std::string& tab);

    // At boot, after the graphics settings: the player's saved VSync and frame-rate limit.
    static void ApplySavedDisplay();

private:
    struct Snapshot
    {
        std::vector<int> groups;
        int resolution = -1;
        bool vsync = false;
        int maxFps = 0;
    };
    static Snapshot Take();
    static void Restore(const Snapshot& s);
    static bool SaveDisplay(std::string& why);
    void DrawGraphics();
    void DrawDisplay();

    bool open_ = false;
    int tab_ = 0;          // 0 graphics, 1 display
    int requestedTab_ = -1;
    Snapshot atOpen_;
    std::string status_;
};
