#include "input/InputManager.h"
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h" // Narrow
#include "input/InputActions.h"
#include <algorithm>
#include <hidusage.h>
#include <iterator>
#include <vector>

void InputManager::Initialize(HWND hwnd) {
    hwnd_ = hwnd;

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid.usUsage = HID_USAGE_GENERIC_MOUSE;
    rid.dwFlags = RIDEV_INPUTSINK; // receive WM_INPUT even if the cursor leaves the window (while active)
    rid.hwndTarget = hwnd_;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void InputManager::NewFrame() {
    keyPressed_ = pendingPressed_;
    keyReleased_ = pendingReleased_;
    pendingPressed_.fill(0);
    pendingReleased_.fill(0);
    mouseDX_ = pendingDX_;
    mouseDY_ = pendingDY_;
    mouseWheelDelta_ = pendingWheel_;
    pendingDX_ = 0;
    pendingDY_ = 0;
    pendingWheel_ = 0;
    takenThisFrame_.clear();
    // A script's presses wait until taken (TakeActionPress); the ones sent since the last frame join them.
    for (std::string& press : pendingInjectedPresses_) {
        injectedPresses_.push_back(std::move(press));
    }
    pendingInjectedPresses_.clear();
}

void InputManager::SetMouseCapture(bool capture) {
    if (mouseCaptured_ == capture) {
        return;
    }
    mouseCaptured_ = capture;

    if (mouseCaptured_) {
        RECT rc{};
        if (GetClientRect(hwnd_, &rc)) {
            POINT tl{ rc.left, rc.top };
            POINT br{ rc.right, rc.bottom };
            ClientToScreen(hwnd_, &tl);
            ClientToScreen(hwnd_, &br);
            rc.left = tl.x; rc.top = tl.y; rc.right = br.x; rc.bottom = br.y;
            ClipCursor(&rc);
        }
        ShowCursor(FALSE);
        SetCapture(hwnd_);
    } else {
        ClipCursor(nullptr);
        ShowCursor(TRUE);
        ReleaseCapture();
    }
}

void InputManager::HandleRawInput_(HRAWINPUT hRaw) {
    UINT size = 0;
    if (GetRawInputData(hRaw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0) {
        return;
    }
    thread_local std::vector<BYTE> buffer;
    if (buffer.size() < size) {
        buffer.resize(size);
    }
    if (GetRawInputData(hRaw, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
        return;
    }
    RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buffer.data());
    if (ri->header.dwType == RIM_TYPEMOUSE && mouseCaptured_) {
        const RAWMOUSE& m = ri->data.mouse;
        pendingDX_ += static_cast<int>(m.lLastX);
        pendingDY_ += static_cast<int>(m.lLastY);

        if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) { rButtonDown_ = true; }
        if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)   { rButtonDown_ = false; }
    }
}

void InputManager::OnWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INPUT: {
            HandleRawInput_(reinterpret_cast<HRAWINPUT>(lParam));
            break;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const int vk = static_cast<int>(wParam) & 0xFF;
            if (keyDown_[vk] == 0) {
                pendingPressed_[vk] = 1;
            }
            keyDown_[vk] = 1;
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const int vk = static_cast<int>(wParam) & 0xFF;
            if (keyDown_[vk] != 0) {
                pendingReleased_[vk] = 1;
            }
            keyDown_[vk] = 0;
            break;
        }
        case WM_LBUTTONDOWN: { lButtonDown_ = true;  break; }
        case WM_LBUTTONUP: { lButtonDown_ = false; break; }
        case WM_MBUTTONDOWN: { mButtonDown_ = true;  break; }
        case WM_MBUTTONUP: { mButtonDown_ = false; break; }
        case WM_RBUTTONDOWN: { rButtonDown_ = true; break; }
        case WM_RBUTTONUP: { rButtonDown_ = false; break; }
        case WM_MOUSEWHEEL: {
            pendingWheel_ += GET_WHEEL_DELTA_WPARAM(wParam);
            break;
        }
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                SetMouseCapture(false);
                for (auto& v : keyDown_) { v = 0; }
            }
            break;
        }
        case WM_KILLFOCUS: {
            SetMouseCapture(false);
            for (auto& v : keyDown_) { v = 0; }
            break;
        }
        default: {
            break;
        }
    }
}

bool InputManager::LoadActions(const std::wstring& path) {
    if (!actions_.LoadFromJsonFile(path)) {
        return false;
    }
    // R12.1: the file against the actions the code reads (input/InputActions.h). A name the code reads and the file
    // does not bind is a key that silently does nothing -- an Error, the run goes on; a name the file binds and nothing
    // reads is a key the controls overlay shows for nothing -- a Warning.
    for (std::string_view name : actions::kAll) {
        if (!actions_.Has(name)) {
            LOG_ERROR(logging::LogCategory::App, "input bindings: the code reads action '{}' but {} does not bind it",
                      name, vfs::Narrow(path));
        }
    }
    for (std::string_view name : actions_.Names()) {
        if (std::find(std::begin(actions::kAll), std::end(actions::kAll), name) == std::end(actions::kAll)) {
            LOG_WARNING(logging::LogCategory::App, "input bindings: {} binds action '{}' but no code reads it",
                        vfs::Narrow(path), name);
        }
    }
    return true;
}

namespace {
bool Holds(const std::vector<std::string>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}
}

bool InputManager::IsActionDown(std::string_view name) const {
    return (keysCount_ && actions_.IsActionDown(name, *this)) || Holds(injectedHeld_, name);
}

bool InputManager::WasActionPressed(std::string_view name) const {
    return (keysCount_ && actions_.WasActionPressed(name, *this)) || Holds(injectedPresses_, name);
}

bool InputManager::TakeActionPress(std::string_view name) {
    if (!WasActionPressed(name)) {
        return false;
    }
    for (const std::string& taken : takenThisFrame_) {
        if (taken == name) {
            return false;
        }
    }
    takenThisFrame_.emplace_back(name);
    // A script's press is used up by the listener that took it.
    if (const auto it = std::find(injectedPresses_.begin(), injectedPresses_.end(), name); it != injectedPresses_.end()) {
        injectedPresses_.erase(it);
    }
    return true;
}

bool InputManager::WasActionReleased(std::string_view name) const {
    return keysCount_ && actions_.WasActionReleased(name, *this);
}

float InputManager::GetActionAxis(std::string_view name) const {
    float value = keysCount_ ? actions_.GetAxis(name, *this) : 0.0f;
    for (const auto& axis : injectedAxes_) {
        if (axis.first == name) {
            value += axis.second;
        }
    }
    return value;
}

void InputManager::ClearInjected() {
    pendingInjectedPresses_.clear();
    injectedPresses_.clear();
    injectedHeld_.clear();
    injectedAxes_.clear();
}

void InputManager::InjectPress(std::string_view name) {
    pendingInjectedPresses_.emplace_back(name);
}

void InputManager::InjectHeld(std::string_view name, bool held) {
    const auto it = std::find(injectedHeld_.begin(), injectedHeld_.end(), name);
    if (held && it == injectedHeld_.end()) {
        injectedHeld_.emplace_back(name);
    }
    if (!held && it != injectedHeld_.end()) {
        injectedHeld_.erase(it);
    }
}

void InputManager::InjectAxis(std::string_view name, float value) {
    const auto it = std::find_if(injectedAxes_.begin(), injectedAxes_.end(), [&](const auto& a) { return a.first == name; });
    if (it != injectedAxes_.end()) {
        if (value == 0.0f) {
            injectedAxes_.erase(it);
        } else {
            it->second = value;
        }
    } else if (value != 0.0f) {
        injectedAxes_.emplace_back(std::string(name), value);
    }
}
