#include "app/Camera.h"
#include "input/InputManager.h"
#include "input/ActionMap.h"

void Camera::UpdateFromActions(InputManager& input, const ActionMap& map, float dt) {
    // Toggle camera rotation via the action
    const bool lookHeld = map.IsActionDown("LookToggle", input) && !input.IsKeyDown(VK_MENU);
    if (lookHeld) {
        if (!input.IsMouseCaptured()) {
            input.SetMouseCapture(true);
        }
        const float dx = map.GetAxis("LookX", input);
        const float dy = map.GetAxis("LookY", input);
        if (dx != 0.0f || dy != 0.0f) {
            // Scale already includes sensitivity/invert
            AddYaw(dx);
            AddPitch(dy);
        }
    } else {
        if (input.IsMouseCaptured()) {
            input.SetMouseCapture(false);
        }
    }

    // Adjust movement speed with the mouse wheel
    const int wheel = input.MouseWheelDelta();
    if (wheel != 0) {
        constexpr float kWheelStep = 0.2f;
        constexpr float kMinMultiplier = 0.2f;
        constexpr float kMaxMultiplier = 10.0f;
        constexpr float kWheelTicks = 120.0f;
        const float ticks = static_cast<float>(wheel) / kWheelTicks;
        moveSpeedMultiplier_ += ticks * kWheelStep;
        moveSpeedMultiplier_ = Clamp(moveSpeedMultiplier_, kMinMultiplier, kMaxMultiplier);
    }

    // Movement
    float speed = moveSpeed_ * moveSpeedMultiplier_;
    if (map.IsActionDown("Sprint", input)) {
        speed *= sprintMultiplier_;
    }
    const float mx = map.GetAxis("MoveX", input);
    const float my = map.GetAxis("MoveY", input);
    const float mz = map.GetAxis("MoveZ", input);

    if (mx != 0.0f) { MoveRight(mx * speed * dt); }
    if (my != 0.0f) { MoveUp(my * speed * dt); }
    if (mz != 0.0f) { MoveForward(mz * speed * dt); }
}