#include "model_reaper_tray.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/logging/Log.h"

namespace
{
    // WM_APP and up are ours. The first is what the shell sends us for mouse activity on the
    // icon; the second is how the watch thread asks for a redraw without touching the window.
    constexpr UINT kTrayCallbackMessage = WM_APP + 1;
    constexpr UINT kStatusChangedMessage = WM_APP + 2;

    constexpr UINT_PTR kTickTimer = 1;
    constexpr UINT kIconId = 1;
    constexpr UINT kMenuStopServer = 100;
    constexpr UINT kMenuCloseWatchdog = 101;

    // Written by the watch thread, read by the window thread. Small enough to copy under the
    // lock rather than hold the lock while drawing.
    std::mutex g_statusMutex;
    llmreaper::Status g_status;

    std::string g_endpoint;
    unsigned long g_serverPid = 0;

    // What the icon currently SHOWS, so a redraw that would change nothing costs nothing:
    // the tick runs once a second forever and most seconds nothing has moved.
    HICON g_icon = nullptr;
    COLORREF g_shownColour = CLR_INVALID;
    std::wstring g_shownTip;

    // Broadcast by the shell when Explorer restarts. A message-only window would never see
    // it -- broadcasts do not reach HWND_MESSAGE -- which is why this window is an ordinary
    // one that is simply never shown.
    UINT g_taskbarCreated = 0;

    llmreaper::Status CurrentStatus()
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        return g_status;
    }

    std::wstring Widen(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
            static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
            wide.data(), length);
        return wide;
    }

    std::string Narrow(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string narrow(static_cast<std::size_t>(length), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
            narrow.data(), length, nullptr, nullptr);
        return narrow;
    }

    // Seconds left on the deadline the watch published, clamped at zero. Computed HERE from
    // the tick count rather than counted down by the watch, so it is exact between two polls
    // half a minute apart.
    int SecondsLeft(const llmreaper::Status& status)
    {
        if (status.phase != llmreaper::Status::Phase::CountingDown || status.retireAtTick == 0)
        {
            return 0;
        }
        const unsigned long long now = ::GetTickCount64();
        if (status.retireAtTick <= now)
        {
            return 0;
        }
        return static_cast<int>((status.retireAtTick - now) / 1000ull);
    }

    std::wstring MinutesAndSeconds(int seconds)
    {
        wchar_t text[32] = {};
        ::swprintf(text, 32, L"%d:%02d", seconds / 60, seconds % 60);
        return text;
    }

    // The state as a colour. This is the whole reason the icon is drawn rather than loaded:
    // the notification area then answers "is the model still up?" without a click.
    COLORREF ColourFor(llmreaper::Status::Phase phase)
    {
        switch (phase)
        {
        case llmreaper::Status::Phase::EditorOpen:
            return RGB(63, 191, 106);    // green -- an editor is open, the server is held
        case llmreaper::Status::Phase::CountingDown:
            return RGB(224, 160, 48);    // amber -- the clock is running
        case llmreaper::Status::Phase::Retired:
            return RGB(198, 72, 72);     // red -- the server is gone
        case llmreaper::Status::Phase::Reaching:
        default:
            return RGB(138, 148, 158);   // grey -- it has not answered yet
        }
    }

    std::wstring StateInWords(const llmreaper::Status& status)
    {
        switch (status.phase)
        {
        case llmreaper::Status::Phase::EditorOpen:
            return L"An editor is open -- the server is held";
        case llmreaper::Status::Phase::CountingDown:
            return L"No editor open -- the server stops in " +
                MinutesAndSeconds(SecondsLeft(status));
        case llmreaper::Status::Phase::Retired:
            return L"The server is gone";
        case llmreaper::Status::Phase::Reaching:
        default:
            return L"Waiting for the server to answer";
        }
    }

    // A filled dot with a darker rim, drawn at whatever size this machine asks small icons
    // to be. The watchdog has no .ico and does not need a resource step for one; drawing it
    // is also what lets the colour carry the state.
    HICON MakeDotIcon(COLORREF colour)
    {
        const int size = std::max(16, ::GetSystemMetrics(SM_CXSMICON));

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;   // top-down, so row 0 is the top
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* rawPixels = nullptr;
        const HDC screen = ::GetDC(nullptr);
        const HBITMAP colourBitmap =
            ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &rawPixels, nullptr, 0);
        ::ReleaseDC(nullptr, screen);
        if (!colourBitmap || !rawPixels)
        {
            return nullptr;
        }

        const double centre = (size - 1) * 0.5;
        const double radius = size * 0.45;
        const double rim = radius - std::max(1.0, size * 0.09);
        const int samples = 4;   // 4x4 per pixel: enough that the edge does not look sawn

        auto* pixels = static_cast<unsigned char*>(rawPixels);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                // Accumulated over ALL subsamples and divided by their count, which makes
                // the result premultiplied by construction -- what a 32-bit icon wants.
                double blue = 0.0;
                double green = 0.0;
                double red = 0.0;
                double alpha = 0.0;
                for (int sy = 0; sy < samples; ++sy)
                {
                    for (int sx = 0; sx < samples; ++sx)
                    {
                        const double px = x + (sx + 0.5) / samples;
                        const double py = y + (sy + 0.5) / samples;
                        const double dx = px - centre - 0.5;
                        const double dy = py - centre - 0.5;
                        const double distance = std::sqrt(dx * dx + dy * dy);
                        if (distance > radius)
                        {
                            continue;
                        }
                        // The rim is the same hue at two thirds, so the dot reads as a dot
                        // on a light taskbar and on a dark one.
                        const double shade = distance > rim ? 0.62 : 1.0;
                        blue += GetBValue(colour) * shade;
                        green += GetGValue(colour) * shade;
                        red += GetRValue(colour) * shade;
                        alpha += 255.0;
                    }
                }
                const double total = samples * samples;
                unsigned char* pixel = pixels + (static_cast<std::size_t>(y) * size + x) * 4;
                pixel[0] = static_cast<unsigned char>(blue / total);
                pixel[1] = static_cast<unsigned char>(green / total);
                pixel[2] = static_cast<unsigned char>(red / total);
                pixel[3] = static_cast<unsigned char>(alpha / total);
            }
        }

        // An all-zero AND mask means "opaque everywhere" and leaves the decision to the
        // alpha channel. Supplying the bits rather than passing null matters: a bitmap
        // created empty holds whatever was in that memory.
        const std::size_t maskStride = ((static_cast<std::size_t>(size) + 15) / 16) * 2;
        std::vector<unsigned char> maskBits(maskStride * size, 0);
        const HBITMAP maskBitmap = ::CreateBitmap(size, size, 1, 1, maskBits.data());

        ICONINFO iconInfo = {};
        iconInfo.fIcon = TRUE;
        iconInfo.hbmMask = maskBitmap;
        iconInfo.hbmColor = colourBitmap;
        const HICON icon = ::CreateIconIndirect(&iconInfo);

        ::DeleteObject(colourBitmap);
        if (maskBitmap)
        {
            ::DeleteObject(maskBitmap);
        }
        return icon;
    }

    void FillIconData(NOTIFYICONDATAW& data, HWND window)
    {
        data.cbSize = sizeof(NOTIFYICONDATAW);
        data.hWnd = window;
        data.uID = kIconId;
    }

    void CopyTip(NOTIFYICONDATAW& data, const std::wstring& tip)
    {
        const std::size_t room = sizeof(data.szTip) / sizeof(data.szTip[0]) - 1;
        const std::size_t length = tip.size() < room ? tip.size() : room;
        std::wmemcpy(data.szTip, tip.c_str(), length);
        data.szTip[length] = L'\0';
    }

    // Puts the icon into the notification area, or puts it back after Explorer restarted.
    void AddIcon(HWND window)
    {
        const llmreaper::Status status = CurrentStatus();
        g_shownColour = ColourFor(status.phase);
        g_shownTip = L"test_cube model watchdog\n" + StateInWords(status);

        if (g_icon)
        {
            ::DestroyIcon(g_icon);
        }
        g_icon = MakeDotIcon(g_shownColour);

        NOTIFYICONDATAW data = {};
        FillIconData(data, window);
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = g_icon;
        CopyTip(data, g_shownTip);
        // Said out loud because "I do not see any icon" is the complaint this feature will
        // produce, and the answer is usually the overflow chevron rather than a failure.
        if (::Shell_NotifyIconW(NIM_ADD, &data))
        {
            LOG_INFO(logging::LogCategory::Editor,
                "model reaper: notification-area icon shown (it may be under the taskbar's "
                "overflow chevron until it is dragged out)");
        }
        else
        {
            LOG_WARNING(logging::LogCategory::Editor,
                "model reaper: the shell refused the notification-area icon (error {}); the "
                "watch carries on without it", ::GetLastError());
        }
    }

    void RemoveIcon(HWND window)
    {
        NOTIFYICONDATAW data = {};
        FillIconData(data, window);
        ::Shell_NotifyIconW(NIM_DELETE, &data);
        if (g_icon)
        {
            ::DestroyIcon(g_icon);
            g_icon = nullptr;
        }
    }

    // Called every second and on every published status. Touches the shell only when
    // something actually changed -- while an editor is open that is never.
    void RefreshIcon(HWND window)
    {
        const llmreaper::Status status = CurrentStatus();
        const COLORREF colour = ColourFor(status.phase);
        const std::wstring tip = L"test_cube model watchdog\n" + StateInWords(status);
        if (colour == g_shownColour && tip == g_shownTip)
        {
            return;
        }

        NOTIFYICONDATAW data = {};
        FillIconData(data, window);
        data.uFlags = NIF_TIP;
        CopyTip(data, tip);

        HICON replaced = nullptr;
        if (colour != g_shownColour)
        {
            replaced = g_icon;
            g_icon = MakeDotIcon(colour);
            data.uFlags |= NIF_ICON;
            data.hIcon = g_icon;
        }
        // Only what the shell ACCEPTED is recorded as shown, so a refused update is tried
        // again a second later instead of being remembered as done.
        if (::Shell_NotifyIconW(NIM_MODIFY, &data))
        {
            g_shownTip = tip;
            g_shownColour = colour;
        }
        if (replaced)
        {
            // After the shell has taken the new one, never before.
            ::DestroyIcon(replaced);
        }
    }

    void ShowMenu(HWND window)
    {
        const llmreaper::Status status = CurrentStatus();
        const bool serverGone = status.phase == llmreaper::Status::Phase::Retired;

        const std::wstring header = L"llama-server pid " + std::to_wstring(g_serverPid) +
            L" on " + Widen(g_endpoint);

        // What the user was shown, in the log, so that what they did next can be read
        // against what they were looking at.
        LOG_INFO(logging::LogCategory::Editor, "model reaper: menu opened showing \"{}\"",
            Narrow(StateInWords(status)));

        const HMENU menu = ::CreatePopupMenu();
        if (!menu)
        {
            return;
        }
        // The first two lines are the state, not commands -- greyed so they cannot be
        // mistaken for one.
        ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, header.c_str());
        ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, StateInWords(status).c_str());
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING | (serverGone ? (MF_DISABLED | MF_GRAYED) : 0),
            kMenuStopServer, L"Stop the server now");
        // Says what it costs. Closing the watchdog is a real choice -- it is how you keep a
        // loaded model across a long break -- but it leaves 13 GB held by a process nothing
        // is minding, and a menu item that did not say so would be a trap.
        ::AppendMenuW(menu, MF_STRING, kMenuCloseWatchdog,
            serverGone ? L"Close the watchdog"
                       : L"Close the watchdog (the server keeps running)");

        POINT cursor = {};
        ::GetCursorPos(&cursor);
        // The pair around TrackPopupMenu that makes the menu dismiss when the user clicks
        // somewhere else: without the foreground call it would not, and without the posted
        // message afterwards it would stay up the first time.
        ::SetForegroundWindow(window);
        ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0,
            window, nullptr);
        ::PostMessageW(window, WM_NULL, 0, 0);
        ::DestroyMenu(menu);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_taskbarCreated != 0 && message == g_taskbarCreated)
        {
            AddIcon(window);
            return 0;
        }

        switch (message)
        {
        case kTrayCallbackMessage:
            // Either button opens the menu. There is no useful "default action" to reserve
            // the left click for -- the watchdog has no window to bring up.
            if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_RBUTTONUP)
            {
                ShowMenu(window);
            }
            return 0;

        case kStatusChangedMessage:
        case WM_TIMER:
            RefreshIcon(window);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == kMenuStopServer)
            {
                llmreaper::RequestStop(true);
            }
            else if (LOWORD(wParam) == kMenuCloseWatchdog)
            {
                llmreaper::RequestStop(false);
            }
            return 0;

        case WM_CLOSE:
            // The icon goes first and by hand. The shell would drop it when the window died
            // anyway, but not until it next looks, and an icon that lingers after the
            // watchdog has gone is an icon that lies.
            RemoveIcon(window);
            ::DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

namespace reapertray
{
    int RunWithTray(const llmreaper::Options& options)
    {
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = llmreaper::Status{};
            g_status.serverPid = options.serverPid;
        }
        g_endpoint = options.endpoint;
        g_serverPid = options.serverPid;

        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"TestCubeModelReaperTray";
        ::RegisterClassExW(&windowClass);

        g_taskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");

        // Never shown, so it is in no taskbar and no Alt+Tab; it exists to receive the
        // shell's messages. WS_EX_TOOLWINDOW in case some future code does show it.
        const HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName,
            L"test_cube model watchdog", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
            instance, nullptr);
        if (!window)
        {
            LOG_WARNING(logging::LogCategory::Editor,
                "model reaper: could not create the tray window (error {}); watching without "
                "an icon", ::GetLastError());
            return llmreaper::Run(options);
        }

        AddIcon(window);
        // Once a second, so the countdown in the tooltip is a countdown and not a number
        // that jumps thirty at a time.
        ::SetTimer(window, kTickTimer, 1000, nullptr);

        int exitCode = 1;
        std::thread watch([&options, window, &exitCode]()
        {
            exitCode = llmreaper::Run(options, [window](const llmreaper::Status& status)
            {
                {
                    std::lock_guard<std::mutex> lock(g_statusMutex);
                    g_status = status;
                }
                // Posted, not called: this runs on the watch thread and the icon belongs to
                // the window thread.
                ::PostMessageW(window, kStatusChangedMessage, 0, 0);
            });
            // The watch is what decides when this process ends; the pump merely serves it.
            ::PostMessageW(window, WM_CLOSE, 0, 0);
        });

        MSG message = {};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        // If the pump ended for any reason other than the watch finishing -- somebody closed
        // the window from outside -- this is what keeps the join from waiting forever.
        llmreaper::RequestStop(false);
        watch.join();
        return exitCode;
    }
}
