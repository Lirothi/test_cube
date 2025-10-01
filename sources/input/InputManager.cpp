#include "input/InputManager.h"
#include <hidusage.h>
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
    for (auto& v : keyPressed_) { v = 0; }
    for (auto& v : keyReleased_) { v = 0; }
    mouseDX_ = 0;
    mouseDY_ = 0;
    mouseWheelDelta_ = 0;
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
        mouseDX_ += static_cast<int>(m.lLastX);
        mouseDY_ += static_cast<int>(m.lLastY);

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
                keyPressed_[vk] = 1;
            }
            keyDown_[vk] = 1;
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const int vk = static_cast<int>(wParam) & 0xFF;
            if (keyDown_[vk] != 0) {
                keyReleased_[vk] = 1;
            }
            keyDown_[vk] = 0;
            if (vk == VK_MENU) {
                // if Alt was released we may drop capture if it was held solely for the camera (scene logic decides)
            }
            break;
        }
        case WM_LBUTTONDOWN: { lButtonDown_ = true;  break; }
        case WM_LBUTTONUP: { lButtonDown_ = false; break; }
        case WM_MBUTTONDOWN: { mButtonDown_ = true;  break; }
        case WM_MBUTTONUP: { mButtonDown_ = false; break; }
        case WM_RBUTTONDOWN: { rButtonDown_ = true; break; }
        case WM_RBUTTONUP: { rButtonDown_ = false; break; }
        case WM_MOUSEWHEEL: {
            mouseWheelDelta_ += GET_WHEEL_DELTA_WPARAM(wParam);
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