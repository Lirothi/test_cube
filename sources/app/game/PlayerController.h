#pragma once

class Camera;
class CameraController;
class InputManager;
class PlayerSystem;

// The keys and the mouse to the player's body and eye (UE's APlayerController): the mouse turns the camera, the keys say where to, relative to where it looks along the ground.
// How the body answers is physics::PlayerBody; where the eye ends up comes back through PlayerSystem::Eye (app/scene) in Scene::Tick. docs/play_mode_plan.md.
class PlayerController
{
public:
    // `hasControls` false (the mouse is free, a window has it): no look and no keys, the body stops of itself.
    void Tick(InputManager& input, Camera& camera, CameraController& look, PlayerSystem& player, bool hasControls, float deltaTime);
};
