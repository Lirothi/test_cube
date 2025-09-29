#include "Camera.h"
#include "InputManager.h"
#include "ActionMap.h"

void Camera::UpdateFromActions(InputManager& input, const ActionMap& map, float dt) {
    // toggle «вращение камерой» по действию
    const bool lookHeld = map.IsActionDown("LookToggle", input) && !input.IsKeyDown(VK_MENU);
    if (lookHeld) {
        if (!input.IsMouseCaptured()) {
            input.SetMouseCapture(true);
        }
        const float dx = map.GetAxis("LookX", input);
        const float dy = map.GetAxis("LookY", input);
        if (dx != 0.0f || dy != 0.0f) {
            // шкала уже включает sensitivity/invert
            AddYaw(dx);
            AddPitch(dy);
        }
    } else {
        if (input.IsMouseCaptured()) {
            input.SetMouseCapture(false);
        }
    }

    // корректировка скорости перемещения колесом мыши
    const int wheel = input.MouseWheelDelta();
    if (wheel != 0) {
        constexpr float kWheelStep = 0.2f;
        constexpr float kMinMultiplier = 0.2f;
        constexpr float kMaxMultiplier = 5.0f;
        const float ticks = static_cast<float>(wheel) / 120.0f;
        moveSpeedMultiplier_ += ticks * kWheelStep;
        moveSpeedMultiplier_ = Clamp(moveSpeedMultiplier_, kMinMultiplier, kMaxMultiplier);
    }

    // ходьба
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