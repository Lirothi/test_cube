#pragma once

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

// The window (docs/scalability_plan.md §6.2 item 1, §6.3, step S6): WINDOWED or BORDERLESS over the whole monitor -- no
// exclusive fullscreen (§10 item 10: a flip-model swapchain with tearing may not run in it, and borderless gives the
// same without switching the monitor's mode; Alt+Enter is refused, SwapchainManager.cpp) -- which monitor, the windowed
// size, and the frame-rate limit while another window has the focus. Saved in user_settings.json "display" and read
// BEFORE the window is made (App::InitWindow); changed live by the window's style and SetWindowPos, whose WM_SIZE takes
// the existing road (Renderer::OnResize -> the next frame's ApplyPendingResize). `--window=<w>x<h>` is stronger than all
// of it and never saved.
namespace display
{
    enum class WindowMode { Windowed = 0, Borderless = 1 };

    struct DisplaySettings
    {
        WindowMode mode = WindowMode::Windowed;
        std::string monitor;       // the monitor's device name ("\\.\DISPLAY1"); "" = the primary
        int width = 0;             // the WINDOWED client size; 0 = the build's default for the monitor
        int height = 0;
        int backgroundFps = -1;    // the frame-rate limit while another window has the focus: 0 = none, -1 = the build's
        bool saved = false;        // read from the file (Load)
    };
    // The same window: mode, monitor, size (the background limit and `saved` aside).
    bool SameWindow(const DisplaySettings& a, const DisplaySettings& b);

    struct MonitorInfo
    {
        std::string device;  // "\\.\DISPLAY1"
        std::string name;    // what the monitor calls itself ("DELL U2720Q"), else the device
        RECT rect{};         // the whole monitor, desktop pixels
        RECT work{};         // without the taskbar
        UINT dpi = 96;
        bool primary = false;
    };
    std::vector<MonitorInfo> Monitors();
    // Its index in `monitors`; the primary when `device` is empty or not connected any more.
    int FindMonitor(const std::vector<MonitorInfo>& monitors, const std::string& device);
    // "2: DELL U2720Q, 3840x2160 (primary)" for the menu.
    std::string MonitorLabel(const std::vector<MonitorInfo>& monitors, int index);
    // Windowed client sizes that fit the monitor's work area with the window's frame, 16:10 or wider (as Lyra lists
    // them), smallest first, with `currentWidth` x `currentHeight` among them.
    std::vector<std::pair<int, int>> WindowedSizes(const MonitorInfo& monitor, int currentWidth, int currentHeight);

    DisplaySettings Load();
    bool Save(const DisplaySettings& s, std::string& why);
    // The window as it is now: its mode by its style, its monitor, its client size; the background limit as set.
    DisplaySettings Current(HWND window);

    // Where a window with these settings goes. Windowed without a size: the build's default (2560 wide at most, 16:9,
    // as the window always started) on that monitor, centred in its work area.
    struct Placement
    {
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT rect{};          // the window's outer rect
        int clientWidth = 0;
        int clientHeight = 0;
    };
    Placement PlacementFor(const DisplaySettings& s, const std::vector<MonitorInfo>& monitors);

    // The window to these settings, now (the game thread: the window's own). False when nothing could be done.
    bool Apply(HWND window, const DisplaySettings& s);
    // Inside Apply: a WM_DPICHANGED that arrives then is ours, and its suggested rect is not taken (App::WndProc).
    bool Applying();

    // The app's window (App::InitWindow) and a change asked for by the menu or `--set display.*`, taken at the frame's
    // safe point (App.cpp afterFrame).
    void SetMainWindow(HWND window);
    HWND MainWindow();
    void Request(const DisplaySettings& s);
    // The change asked for and not taken yet, else the window as it is: a second request in the same frame (--set
    // display.width then display.height) builds on the first instead of undoing it.
    DisplaySettings PendingOrCurrent(HWND window);
    bool TakeRequest(DisplaySettings& out);

    // The frame-rate limit while the window is not the foreground one (the game half's frame pacing). A packaged build
    // defaults to 30 (Lyra's), a dev build to none: a script's editor in the background must keep its pace.
    int DefaultBackgroundFps();
    int BackgroundFps();
    void SetBackgroundFps(int fps); // -1 = the default
}
