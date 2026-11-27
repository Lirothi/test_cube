#include "app/game/PlayerController.h"

#include <cmath>

#include "app/camera/CameraController.h"
#include "input/InputManager.h"
#include "input/InputActions.h"
#include "app/scene/PlayerSystem.h"

void PlayerController::Tick(InputManager& input, Camera& camera, CameraController& look, PlayerSystem& player, bool hasControls, float deltaTime)
{
    physics::PlayerInput out;
    if (hasControls) { look.AddLookInput(input); }
    // The keys (InputManager counts them only when the caller's KeysScope says so) and what a script injected in their
    // place (R12.3), one road. Along the ground by the yaw alone: looking down at the sand must not slow the walk.
    const float yaw = camera.GetYaw();
    const float forward = input.GetActionAxis(actions::kMoveZ);
    const float right = input.GetActionAxis(actions::kMoveX);
    Math::float3 move(std::sin(yaw) * forward + std::cos(yaw) * right, 0.0f, std::cos(yaw) * forward - std::sin(yaw) * right);
    const float length = std::sqrt(move.x * move.x + move.z * move.z);
    if (length > 1.0f) { move = Math::float3(move.x / length, 0.0f, move.z / length); } // a diagonal is no faster
    // A script's move in the WORLD's axes, after the clamp, as physics added player.testMoveX / Z before R12.3.
    out.move = move + Math::float3(input.GetActionAxis(actions::kMoveWorldX), 0.0f, input.GetActionAxis(actions::kMoveWorldZ));
    out.jump = input.TakeActionPress(actions::kJump);
    out.crouch = input.IsActionDown(actions::kCrouch);
    out.sprint = input.IsActionDown(actions::kSprint) || input.IsActionDown(actions::kScriptSprint);
    look.TakeOwedLook(deltaTime); // the smoothing's tail runs out even as the mouse goes free
    player.SetInput(out);
}
