#include "app/settings/DisplayMode.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include "app/settings/UserSettings.h"
#include "core/JsonField.h" // the player's file: "mode": 1 threw before the window was made, at every start
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "third_party/json/json.hpp"

namespace display
{
namespace
{
    using json = nlohmann::json;

    HWND g_mainWindow = nullptr;
    std::atomic<bool> g_applying{ false };
    int g_backgroundFps = -1;
    std::mutex g_requestMutex;
    bool g_hasRequest = false;
    DisplaySettings g_request;

    // GetDpiForMonitor lives in shcore.dll; loaded, not linked (one more library for one call).
    UINT MonitorDpi(HMONITOR monitor)
    {
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static GetDpiForMonitorFn fn = []() -> GetDpiForMonitorFn {
            HMODULE shcore = LoadLibraryW(L"shcore.dll");
            return shcore ? reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
        }();
        UINT x = 96, y = 96;
        if (fn && SUCCEEDED(fn(monitor, 0 /*MDT_EFFECTIVE_DPI*/, &x, &y))) { return x; }
        return 96;
    }

    BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM user)
    {
        auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(user);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(monitor, &mi)) { return TRUE; }
        MonitorInfo m;
        m.device = vfs::Narrow(mi.szDevice);
        m.rect = mi.rcMonitor;
        m.work = mi.rcWork;
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        m.dpi = MonitorDpi(monitor);
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        m.name = EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0) && dd.DeviceString[0] ? vfs::Narrow(dd.DeviceString) : m.device;
        out->push_back(std::move(m));
        return TRUE;
    }

    int Width(const RECT& r) { return r.right - r.left; }
    int Height(const RECT& r) { return r.bottom - r.top; }

    // What each monitor calls itself (its EDID: "LG ULTRAGEAR+", "LS27D70xE"), by GDI device ("\\.\DISPLAY1"). The
    // display device's own string is the driver's, "Generic PnP Monitor" for most -- two such monitors read the same.
    std::map<std::wstring, std::wstring> FriendlyNames()
    {
        std::map<std::wstring, std::wstring> out;
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) { return out; }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        {
            return out;
        }
        for (UINT32 i = 0; i < pathCount; ++i)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = paths[i].sourceInfo.adapterId;
            source.header.id = paths[i].sourceInfo.id;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = paths[i].targetInfo.adapterId;
            target.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
                DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0])
            {
                out[source.viewGdiDeviceName] = target.monitorFriendlyDeviceName;
            }
        }
        return out;
    }

    // The window's frame around a client of w x h at this DPI.
    RECT FramedRect(int w, int h, DWORD style, UINT dpi)
    {
        RECT r{ 0, 0, w, h };
        using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        static AdjustForDpiFn fn = reinterpret_cast<AdjustForDpiFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
        if (!fn || !fn(&r, style, FALSE, 0, dpi)) { AdjustWindowRect(&r, style, FALSE); }
        return r;
    }
}

bool SameWindow(const DisplaySettings& a, const DisplaySettings& b)
{
    if (a.mode != b.mode || a.monitor != b.monitor) { return false; }
    return a.mode == WindowMode::Borderless || (a.width == b.width && a.height == b.height);
}

std::vector<MonitorInfo> Monitors()
{
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&out));
    const std::map<std::wstring, std::wstring> names = FriendlyNames();
    for (MonitorInfo& m : out)
    {
        const auto it = names.find(vfs::Widen(m.device));
        if (it != names.end()) { m.name = vfs::Narrow(it->second); }
    }
    // The primary first, then left to right: the order a person sees them in.
    std::sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) { return a.primary; }
        return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
    });
    return out;
}

int FindMonitor(const std::vector<MonitorInfo>& monitors, const std::string& device)
{
    int primary = monitors.empty() ? -1 : 0;
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        if (!device.empty() && monitors[i].device == device) { return static_cast<int>(i); }
        if (monitors[i].primary) { primary = static_cast<int>(i); }
    }
    return primary;
}

std::string MonitorLabel(const std::vector<MonitorInfo>& monitors, int index)
{
    if (index < 0 || index >= static_cast<int>(monitors.size())) { return "?"; }
    const MonitorInfo& m = monitors[static_cast<std::size_t>(index)];
    // Where it stands from the primary: two monitors of one model differ by that alone.
    const char* where = "primary";
    if (!m.primary)
    {
        const auto primary = std::find_if(monitors.begin(), monitors.end(), [](const MonitorInfo& o) { return o.primary; });
        if (primary != monitors.end())
        {
            const int dx = (m.rect.left + m.rect.right) - (primary->rect.left + primary->rect.right);
            const int dy = (m.rect.top + m.rect.bottom) - (primary->rect.top + primary->rect.bottom);
            where = std::abs(dx) >= std::abs(dy) ? (dx < 0 ? "left" : "right") : (dy < 0 ? "above" : "below");
        }
        else { where = ""; }
    }
    char text[256];
    std::snprintf(text, sizeof(text), "%d: %s, %dx%d%s%s", index + 1, m.name.c_str(), Width(m.rect), Height(m.rect),
                  where[0] ? ", " : "", where);
    return text;
}

std::vector<std::pair<int, int>> WindowedSizes(const MonitorInfo& monitor, int currentWidth, int currentHeight)
{
    static const std::pair<int, int> kStandard[] = {
        { 1280, 720 }, { 1280, 800 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 },
        { 1920, 1200 }, { 2560, 1080 }, { 2560, 1440 }, { 2560, 1600 }, { 3200, 1800 }, { 3440, 1440 }, { 3840, 1600 },
        { 3840, 2160 }, { 5120, 2160 }, { 5120, 2880 },
    };
    std::vector<std::pair<int, int>> out;
    for (const auto& [w, h] : kStandard)
    {
        if (static_cast<float>(w) / static_cast<float>(h) < 1.6f - 1e-3f) { continue; }
        const RECT framed = FramedRect(w, h, WS_OVERLAPPEDWINDOW, monitor.dpi);
        if (Width(framed) > Width(monitor.work) || Height(framed) > Height(monitor.work)) { continue; }
        out.emplace_back(w, h);
    }
    if (currentWidth > 0 && currentHeight > 0 &&
        std::find(out.begin(), out.end(), std::make_pair(currentWidth, currentHeight)) == out.end())
    {
        out.emplace_back(currentWidth, currentHeight);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.first * a.second != b.first * b.second ? a.first * a.second < b.first * b.second : a.first < b.first;
    });
    return out;
}

DisplaySettings Load()
{
    DisplaySettings s;
    const json d = json::parse(settings::ReadUserSection("display"), nullptr, false);
    if (!d.is_object()) { return s; }
    const std::string mode = jsonfield::Get(d, "mode", std::string());
    if (mode == "borderless" || mode == "windowed")
    {
        s.mode = mode == "borderless" ? WindowMode::Borderless : WindowMode::Windowed;
        s.saved = true;
    }
    if (d.contains("monitor") && d["monitor"].is_string()) { s.monitor = d["monitor"].get<std::string>(); }
    if (d.contains("width") && d["width"].is_number_integer() && d.contains("height") && d["height"].is_number_integer())
    {
        s.width = std::clamp(d["width"].get<int>(), 0, 16384);
        s.height = std::clamp(d["height"].get<int>(), 0, 16384);
        if (s.width < 320 || s.height < 200) { s.width = s.height = 0; }
    }
    if (d.contains("backgroundFps") && d["backgroundFps"].is_number_integer())
    {
        s.backgroundFps = std::clamp(d["backgroundFps"].get<int>(), -1, 1000);
    }
    return s;
}

bool Save(const DisplaySettings& s, std::string& why)
{
    const json d = {
        { "mode", s.mode == WindowMode::Borderless ? "borderless" : "windowed" },
        { "monitor", s.monitor },
        { "width", s.width },
        { "height", s.height },
        { "backgroundFps", s.backgroundFps },
    };
    return settings::WriteUserSection("display", d.dump(), why);
}

DisplaySettings Current(HWND window)
{
    DisplaySettings s;
    s.backgroundFps = g_backgroundFps;
    if (!window) { return s; }
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    s.mode = (style & WS_POPUP) != 0 ? WindowMode::Borderless : WindowMode::Windowed;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi)) { s.monitor = vfs::Narrow(mi.szDevice); }
    RECT client{};
    if (GetClientRect(window, &client))
    {
        s.width = Width(client);
        s.height = Height(client);
    }
    return s;
}

Placement PlacementFor(const DisplaySettings& s, const std::vector<MonitorInfo>& monitors)
{
    Placement p;
    const int index = FindMonitor(monitors, s.monitor);
    if (index < 0)
    {
        p.rect = RECT{ 0, 0, 1280, 720 };
        p.clientWidth = 1280;
        p.clientHeight = 720;
        return p;
    }
    const MonitorInfo& m = monitors[static_cast<std::size_t>(index)];
    if (s.mode == WindowMode::Borderless)
    {
        p.style = WS_POPUP;
        p.rect = m.rect;
        p.clientWidth = Width(m.rect);
        p.clientHeight = Height(m.rect);
        return p;
    }
    p.style = WS_OVERLAPPEDWINDOW;
    int w = s.width, h = s.height;
    if (w <= 0 || h <= 0)
    {
        // The window as it always started: 2560 wide at most, 8 pixels inside the work area, 16:9.
        w = std::min(2560, Width(m.work) - 8);
        h = static_cast<int>(static_cast<float>(w) * (9.0f / 16.0f));
    }
    // A saved size larger than this monitor (another monitor, a lower resolution since): fitted, keeping its shape.
    const RECT frame = FramedRect(0, 0, p.style, m.dpi);
    const int maxW = Width(m.work) - (Width(frame)), maxH = Height(m.work) - (Height(frame));
    if (w > maxW || h > maxH)
    {
        const float k = std::min(static_cast<float>(maxW) / static_cast<float>(w), static_cast<float>(maxH) / static_cast<float>(h));
        w = std::max(320, static_cast<int>(static_cast<float>(w) * k));
        h = std::max(200, static_cast<int>(static_cast<float>(h) * k));
    }
    const RECT framed = FramedRect(w, h, p.style, m.dpi);
    const int ow = Width(framed), oh = Height(framed);
    const int x = m.work.left + (Width(m.work) - ow) / 2;
    const int y = m.work.top + std::max(0, (Height(m.work) - oh) / 2);
    p.rect = RECT{ x, y, x + ow, y + oh };
    p.clientWidth = w;
    p.clientHeight = h;
    return p;
}

bool Apply(HWND window, const DisplaySettings& s)
{
    if (!window) { return false; }
    const std::vector<MonitorInfo> monitors = Monitors();
    const Placement p = PlacementFor(s, monitors);
    g_applying = true;
    if (IsZoomed(window) || IsIconic(window)) { ShowWindow(window, SW_RESTORE); }
    SetWindowLongPtrW(window, GWL_STYLE, static_cast<LONG_PTR>(p.style | WS_VISIBLE));
    const BOOL ok = SetWindowPos(window, HWND_TOP, p.rect.left, p.rect.top, Width(p.rect), Height(p.rect),
                                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    g_applying = false;
    const int index = FindMonitor(monitors, s.monitor);
    LOG_INFO(logging::LogCategory::App, "display: {} on {}, client {}x{}{}", s.mode == WindowMode::Borderless ? "borderless" : "windowed",
             MonitorLabel(monitors, index), p.clientWidth, p.clientHeight, ok ? "" : " -- SetWindowPos failed");
    return ok != FALSE;
}

bool Applying() { return g_applying.load(); }

void SetMainWindow(HWND window) { g_mainWindow = window; }
HWND MainWindow() { return g_mainWindow; }

void Request(const DisplaySettings& s)
{
    std::lock_guard<std::mutex> lock(g_requestMutex);
    g_request = s;
    g_hasRequest = true;
}

DisplaySettings PendingOrCurrent(HWND window)
{
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        if (g_hasRequest) { return g_request; }
    }
    return Current(window);
}

bool TakeRequest(DisplaySettings& out)
{
    std::lock_guard<std::mutex> lock(g_requestMutex);
    if (!g_hasRequest) { return false; }
    out = g_request;
    g_hasRequest = false;
    return true;
}

int DefaultBackgroundFps()
{
#if TC_PACKAGED
    return 30;
#else
    return 0;
#endif
}

int BackgroundFps() { return g_backgroundFps < 0 ? DefaultBackgroundFps() : g_backgroundFps; }
void SetBackgroundFps(int fps) { g_backgroundFps = std::clamp(fps, -1, 1000); }
}
