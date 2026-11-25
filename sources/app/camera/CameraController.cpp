#include "app/camera/CameraController.h"
#include "input/InputManager.h"
#include "input/InputActions.h"

#include <algorithm>
#include <cmath>

void CameraController::DropOwedLookOnReset() {
    const uint64_t resets = camera_->GetLookResets();
    if (resets != lookResetsSeen_) {
        lookResetsSeen_ = resets;
        lookOwedYaw_ = lookOwedPitch_ = 0.0f;
    }
}

void CameraController::AddLookInput(InputManager& input) {
    DropOwedLookOnReset();
    // The axes' scale is the bindings' sensitivity and invert; the player's own multiply it (CameraController.h).
    const float sensitivity = std::clamp(g_cameraLookSensitivity, 0.01f, 100.0f);
    lookOwedYaw_ += input.GetActionAxis(actions::kLookX) * sensitivity;
    lookOwedPitch_ += input.GetActionAxis(actions::kLookY) * sensitivity * (g_cameraInvertY ? -1.0f : 1.0f);
}

void CameraController::TakeOwedLook(float dt) {
    DropOwedLookOnReset();
    // Mouse smoothing (g_cameraMouseSmoothingMs). The raw input arrives as whole counts per frame, and at a few hundred
    // frames a second against the mouse's own polling a steady hand gives 3, 5, 2, 6 counts in a row -- a turn that
    // stutters from frame to frame (owner, 2026-10-27, recording a video: "unpleasant jerks when turning the camera").
    // With a time constant tau the input is OWED rather than applied: each frame takes 1 - exp(-dt / tau) of what is
    // still owed, so the camera follows the hand tau behind, the same at any frame rate, and finishes the turn after the
    // button is let go. 0 = all of it at once, as before.
    if (lookOwedYaw_ != 0.0f || lookOwedPitch_ != 0.0f) {
        const float tau = std::clamp(g_cameraMouseSmoothingMs, 0.0f, kCameraMouseSmoothingMaxMs) * 0.001f;
        float share = (tau > 0.0f && dt > 0.0f) ? 1.0f - std::exp(-dt / tau) : 1.0f;
        constexpr float kSettledRadians = 1.0e-6f; // the tail below this is taken whole, not chased for ever
        if (std::abs(lookOwedYaw_) < kSettledRadians && std::abs(lookOwedPitch_) < kSettledRadians) {
            share = 1.0f;
        }
        const float yaw = lookOwedYaw_ * share;
        const float pitch = lookOwedPitch_ * share;
        lookOwedYaw_ = share >= 1.0f ? 0.0f : lookOwedYaw_ - yaw;
        lookOwedPitch_ = share >= 1.0f ? 0.0f : lookOwedPitch_ - pitch;
        camera_->AddYaw(yaw);
        camera_->AddPitch(pitch);
    }
}

void CameraController::UpdateFromInput(InputManager& input, float dt, bool mouseLook) {
    // Toggle camera rotation via the action
    const bool lookHeld = input.IsActionDown(actions::kLookToggle) && !input.IsKeyDown(VK_MENU);
    if (mouseLook) {
        AddLookInput(input); // the capture is the session's
    } else if (lookHeld) {
        if (!input.IsMouseCaptured()) {
            input.SetMouseCapture(true);
        }
        AddLookInput(input);
    } else {
        if (input.IsMouseCaptured()) {
            input.SetMouseCapture(false);
        }
    }
    TakeOwedLook(dt);

    // Adjust movement speed with the mouse wheel: under 1 by 0.2 a notch, from 1 up by 20 % a notch -- up to 50 (owner,
    // 2026-11-13: 150 m/s over a 2 km race course), which is 22 notches from 1 where 0.2 a notch would be 245.
    const int wheel = input.MouseWheelDelta();
    if (wheel != 0) {
        constexpr float kWheelStep = 0.2f;
        constexpr float kWheelRatio = 1.2f;
        constexpr float kMinMultiplier = 0.2f;
        constexpr float kMaxMultiplier = 50.0f;
        constexpr float kWheelTicks = 120.0f;
        const float ticks = static_cast<float>(wheel) / kWheelTicks;
        if (moveSpeedMultiplier_ > 1.0f || (moveSpeedMultiplier_ >= 1.0f && ticks > 0.0f)) {
            moveSpeedMultiplier_ = std::max(1.0f, moveSpeedMultiplier_ * std::pow(kWheelRatio, ticks));
        } else {
            moveSpeedMultiplier_ += ticks * kWheelStep;
        }
        moveSpeedMultiplier_ = Clamp(moveSpeedMultiplier_, kMinMultiplier, kMaxMultiplier);
    }

    // Movement
    float speed = moveSpeed_ * moveSpeedMultiplier_;
    if (input.IsActionDown(actions::kSprint)) {
        speed *= sprintMultiplier_;
    }
    const float mx = input.GetActionAxis(actions::kMoveX);
    const float my = input.GetActionAxis(actions::kMoveY);
    const float mz = input.GetActionAxis(actions::kMoveZ);

    if (mx != 0.0f) { camera_->MoveRight(mx * speed * dt); }
    if (my != 0.0f) { camera_->MoveUp(my * speed * dt); }
    if (mz != 0.0f) { camera_->MoveForward(mz * speed * dt); }
}
