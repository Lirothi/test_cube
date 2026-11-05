#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <string>

#include "input/ActionMap.h"

class InputManager {
public:
    void Initialize(HWND hwnd);
    // The frame's input: the presses, releases, mouse motion and wheel that arrived since the last NewFrame (whenever
    // their messages were pumped) become this frame's, and the accumulators start over.
    void NewFrame();

    // Forward WndProc messages from App::WndProc
    void OnWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool LoadActions(const std::wstring& path);

    bool  IsActionDown(std::string_view name) const;
    bool  WasActionPressed(std::string_view name) const;
    bool  WasActionReleased(std::string_view name) const;
    float GetActionAxis(std::string_view name) const;

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
    // Whether the person is at this window. Capture confines the cursor for the whole desktop: a window in the background has no business taking it.
    bool WindowIsForeground() const { return GetForegroundWindow() == hwnd_; }

private:
    void HandleRawInput_(HRAWINPUT hRaw);

private:
    HWND hwnd_ = nullptr;

    ActionMap actions_;

    std::array<uint8_t, 256> keyDown_{};     // Current state
    std::array<uint8_t, 256> keyPressed_{};  // Triggered during this frame
    std::array<uint8_t, 256> keyReleased_{}; // Released during this frame
    // Game thread plan G8 fix (2026-11-03): what the window's messages brought since the last NewFrame. The game thread
    // pumps messages while it waits for the render thread (RenderThread::WaitIdle), i.e. after this frame's Tick, and
    // NewFrame used to ZERO the frame's state -- every press, release and mouse move that arrived during the wait was
    // gone before the next Tick saw it (F9 in Debug, where the wait is most of the frame, almost never toggled).
    std::array<uint8_t, 256> pendingPressed_{};
    std::array<uint8_t, 256> pendingReleased_{};

    bool lButtonDown_ = false;
    bool mButtonDown_ = false;
    bool rButtonDown_ = false;

    int mouseDX_ = 0;
    int mouseDY_ = 0;
    int mouseWheelDelta_ = 0;
    int pendingDX_ = 0;
    int pendingDY_ = 0;
    int pendingWheel_ = 0;
    bool mouseCaptured_ = false;
};