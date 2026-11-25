#pragma once
#include <cstdint>

#include "rendering/view/Camera.h"

class InputManager;

// Mouse smoothing (Developer Controls > Frame "Mouse smoothing", graphics_settings.json frame.mouseSmoothingMs,
// --set=camera.mouseSmoothing:<ms>): the time constant the look input eases in with (CameraController::TakeOwedLook);
// 0 = every frame's mouse motion is applied at once.
inline float g_cameraMouseSmoothingMs = 0.0f;
inline constexpr float kCameraMouseSmoothingMaxMs = 500.0f;
// The player's look (docs/scalability_plan.md §6.2 item 4; the menu's Controls, user_settings.json controls.*, `--set
// controls.sensitivity|invertY`): the look's speed x this, over the bindings' own sensitivity (input/bindings.json), and
// its vertical turned the other way. CameraController::AddLookInput -- the one road every look takes. Game half only.
inline float g_cameraLookSensitivity = 1.0f;
inline bool g_cameraInvertY = false;
// The player's field of view (§6.2 item 3, §10 item 15; the menu's Image > Field of view, user_settings.json image.fovDeg,
// `--set image.fovDeg`): degrees HORIZONTAL AT 16:9, and the vertical that gives is kept -- a wider screen sees wider,
// not less high (Hor+). 0 = the level's own (its hfovDeg, horizontal at any aspect, as before). The game half sets it on
// its camera every frame (AppController); the render camera takes it with the pose.
inline float g_cameraPlayerFovDeg = 0.0f;

// The input that drives a camera (architecture plan R1.4: moved out of Camera, which is the renderer's view): the free
// fly, the look and its smoothing, the wheel's speed multiplier. One per camera the player steers -- the Scene's, beside
// its game camera (Scene::CameraControllerRef).
class CameraController {
public:
    explicit CameraController(Camera& camera) : camera_(&camera), lookResetsSeen_(camera.GetLookResets()) {}

    // The free camera. `mouseLook`: the mouse turns it without the button (Play, ejected: the session holds the mouse).
    void UpdateFromInput(InputManager& input, float dt, bool mouseLook = false);
    // The look alone, for a camera something else moves (PlayerController): this frame's mouse joins what is owed, and the smoothing pays it.
    void AddLookInput(InputManager& input);
    void TakeOwedLook(float dt);

    float GetMoveSpeedMult() const { return moveSpeedMultiplier_; }

private:
    // The camera's orientation was set by code or cut since the last call (Camera::GetLookResets): what was owed goes.
    void DropOwedLookOnReset();

    Camera* camera_ = nullptr;
    // The look input not applied yet under mouse smoothing (radians; TakeOwedLook).
    float lookOwedYaw_ = 0.0f;
    float lookOwedPitch_ = 0.0f;
    uint64_t lookResetsSeen_ = 0u;
    float moveSpeed_ = 3.0f;
    float sprintMultiplier_ = 2.5f;
    float moveSpeedMultiplier_ = 1.0f;
};
