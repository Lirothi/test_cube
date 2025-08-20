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

    // ходьба
    float speed = map.MoveSpeed();
    if (map.IsActionDown("Sprint", input)) {
        speed *= map.SprintMultiplier();
    }
    const float mx = map.GetAxis("MoveX", input);
    const float my = map.GetAxis("MoveY", input);
    const float mz = map.GetAxis("MoveZ", input);

    if (mx != 0.0f) { MoveRight(mx * speed * dt); }
    if (my != 0.0f) { MoveUp(my * speed * dt); }
    if (mz != 0.0f) { MoveForward(mz * speed * dt); }
}