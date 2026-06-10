#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <string>

#include "input/ActionMap.h"

class InputManager {
public:
    void Initialize(HWND hwnd);
    void NewFrame(); // Reset delta/pressed/released at the start of the frame

    // Forward WndProc messages from App::WndProc
    void OnWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool LoadActions(const std::wstring& path);

    bool  IsActionDown(const std::string& name) const;
    bool  WasActionPressed(const std::string& name) const;
    bool  WasActionReleased(const std::string& name) const;
    float GetActionAxis(const std::string& name) const;

    // Bindings in file order, for the on-screen controls overlay.
    const std::vector<ActionMap::BindingDesc>& GetBindingDescs() const { return actions_.GetBindingDescs(); }

    // Keyboard
    bool IsKeyDown(int vk) const {
        return keyDown_[vk] != 0;
    }
    bool WasKeyPressed(int vk) const {
        return keyPressed_[vk] != 0;
    }
    bool WasKeyReleased(int vk) const {
        return keyReleased_[vk] != 0;
    }

    // Mouse
    int  MouseDeltaX() const { return mouseDX_; }
    int  MouseDeltaY() const { return mouseDY_; }
    int  MouseWheelDelta() const { return mouseWheelDelta_; }
    bool IsRButtonDown() const { return rButtonDown_; }
    bool IsLButtonDown() const { return lButtonDown_; }
    bool IsMButtonDown() const { return mButtonDown_; }

    // Mouse capture (confine + cursor)
    void SetMouseCapture(bool capture);
    bool IsMouseCaptured() const { return mouseCaptured_; }

private:
    void HandleRawInput_(HRAWINPUT hRaw);

private:
    HWND hwnd_ = nullptr;

    ActionMap actions_;

    std::array<uint8_t, 256> keyDown_{};     // Current state
    std::array<uint8_t, 256> keyPressed_{};  // Triggered during this frame
    std::array<uint8_t, 256> keyReleased_{}; // Released during this frame

    bool lButtonDown_ = false;
    bool mButtonDown_ = false;
    bool rButtonDown_ = false;

    int mouseDX_ = 0;
    int mouseDY_ = 0;
    int mouseWheelDelta_ = 0;
    bool mouseCaptured_ = false;
};