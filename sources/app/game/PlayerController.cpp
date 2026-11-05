#include "app/game/PlayerController.h"

#include <cmath>

#include "app/camera/Camera.h"
#include "input/InputManager.h"
#include "physics/PhysicsWorld.h"

void PlayerController::Tick(InputManager& input, Camera& camera, physics::World& world, bool hasControls, float deltaTime)
{
    physics::PlayerInput out;
    if (hasControls)
    {
        camera.AddLookInput(input);
        // Along the ground by the yaw alone: looking down at the sand must not slow the walk.
        const float yaw = camera.GetYaw();
        const float forward = input.GetActionAxis("MoveZ");
        const float right = input.GetActionAxis("MoveX");
        Math::float3 move(std::sin(yaw) * forward + std::cos(yaw) * right, 0.0f, std::cos(yaw) * forward - std::sin(yaw) * right);
        const float length = std::sqrt(move.x * move.x + move.z * move.z);
        if (length > 1.0f) { move = Math::float3(move.x / length, 0.0f, move.z / length); } // a diagonal is no faster
        out.move = move;
        out.jump = input.WasActionPressed("Jump");
        out.crouch = input.IsActionDown("Crouch");
        out.sprint = input.IsActionDown("Sprint");
    }
    camera.TakeOwedLook(deltaTime); // the smoothing's tail runs out even as the mouse goes free
    world.SetPlayerInput(out);
}
