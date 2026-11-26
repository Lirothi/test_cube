#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
    // Architecture plan R12.2: a press goes to ONE listener. The frame's listeners ask top first -- the UI (App::WndProc
    // keeps a key ImGui took), the session's keys (AppController::TickPlay), then the helm or the player on foot
    // (PlaySession), the free camera last -- and the first to take a press has it: a press another listener took this
    // frame is not seen again (the E that takes the helm is not also the E that leaves it over the side, 2026-11-09).
    // WasActionPressed reads without taking: the developer keys, which no other listener shares.
    bool  TakeActionPress(std::string_view name);

    // Architecture plan R12.3: what a script says in place of the keys (--set / MCP set_settings player.test*, a gate)
    // enters HERE, the keys' own channel, and the same queries read it: an injected press is pressed from the next frame
    // until a listener takes it (TakeActionPress -- every action a script presses is read that way; the old test*
    // fields waited for their reader too), a held action holds IsActionDown until let go, an axis adds to
    // GetActionAxis until set to 0. An action no key binds is a script's alone (actions::kMoveWorldX / Z, ...).
    void InjectPress(std::string_view name);
    void InjectHeld(std::string_view name, bool held);
    void InjectAxis(std::string_view name, float value);
    // Lets go of everything a script holds or pressed (PlaySession's end: a script's keys are its session's, as a
    // person's are let go when they stop).
    void ClearInjected();

    // A listener whose keys are not its own this frame (PlaySession: the mouse free, a panel up, a bot watched): while
    // the scope lives the keys' actions read as not pressed / 0, and the injected ones still count -- a scripted
    // --play leaves the mouse, and with it the keys, free, and the script drives on.
    class KeysScope {
    public:
        KeysScope(InputManager& input, bool keysCount) : input_(input), before_(input.keysCount_) { input_.keysCount_ = keysCount; }
        ~KeysScope() { input_.keysCount_ = before_; }
        KeysScope(const KeysScope&) = delete;
        KeysScope& operator=(const KeysScope&) = delete;
    private:
        InputManager& input_;
        bool before_;
    };

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
    std::vector<std::string> takenThisFrame_; // R12.2: the presses a listener took this frame (NewFrame forgets them)
    // R12.3: a script's input. A press waits for the next NewFrame and lives that frame; held actions and axes stay.
    std::vector<std::string> pendingInjectedPresses_;
    std::vector<std::string> injectedPresses_;
    std::vector<std::string> injectedHeld_;
    std::vector<std::pair<std::string, float>> injectedAxes_;
    bool keysCount_ = true; // KeysScope

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