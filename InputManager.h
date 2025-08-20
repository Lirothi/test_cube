#pragma once

#include <windows.h>
#include <array>
#include <cstdint>

class InputManager {
public:
    void Initialize(HWND hwnd);
    void NewFrame(); // сброс delta/pressed/released на начало кадра

    // WndProc прокидываем сюда из App::WndProc
    void OnWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Клавиатура
    bool IsKeyDown(int vk) const {
        return keyDown_[vk] != 0;
    }
    bool WasKeyPressed(int vk) const {
        return keyPressed_[vk] != 0;
    }
    bool WasKeyReleased(int vk) const {
        return keyReleased_[vk] != 0;
    }

    // Мышь
    int  MouseDeltaX() const { return mouseDX_; }
    int  MouseDeltaY() const { return mouseDY_; }
    bool IsRButtonDown() const { return rButtonDown_; }
    bool IsLButtonDown() const { return lButtonDown_; }
    bool IsMButtonDown() const { return mButtonDown_; }

    // Захват мыши (конфин + курсор)
    void SetMouseCapture(bool capture);
    bool IsMouseCaptured() const { return mouseCaptured_; }

private:
    void HandleRawInput_(HRAWINPUT hRaw);

private:
    HWND hwnd_ = nullptr;

    std::array<uint8_t, 256> keyDown_{};     // текущее состояние
    std::array<uint8_t, 256> keyPressed_{};  // сработало в этом кадре
    std::array<uint8_t, 256> keyReleased_{}; // отпустили в этом кадре

    bool lButtonDown_ = false;
    bool mButtonDown_ = false;
    bool rButtonDown_ = false;

    int mouseDX_ = 0;
    int mouseDY_ = 0;
    bool mouseCaptured_ = false;
};