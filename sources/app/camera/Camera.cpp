#include "app/camera/Camera.h"
#include "app/Systems.h"
#include "input/InputManager.h"

void Camera::UpdateFromInput(float dt) {
    auto& input = Systems::GetInput();
    // Toggle camera rotation via the action
    const bool lookHeld = input.IsActionDown("LookToggle") && !input.IsKeyDown(VK_MENU);
    if (lookHeld) {
        if (!input.IsMouseCaptured()) {
            input.SetMouseCapture(true);
        }
        const float dx = input.GetActionAxis("LookX");
        const float dy = input.GetActionAxis("LookY");
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
    if (input.IsActionDown("Sprint")) {
        speed *= sprintMultiplier_;
    }
    const float mx = input.GetActionAxis("MoveX");
    const float my = input.GetActionAxis("MoveY");
    const float mz = input.GetActionAxis("MoveZ");

    if (mx != 0.0f) { MoveRight(mx * speed * dt); }
    if (my != 0.0f) { MoveUp(my * speed * dt); }
    if (mz != 0.0f) { MoveForward(mz * speed * dt); }
}

void Camera::CalcMatrices(Renderer* r)
{
    mat4 lastView = view_.view;
    mat4 lastProj = view_.proj;

    if (hasPrevViewProj_)
    {
        prevView_ = lastView;
        prevProj_ = lastProj;
    }

    mat4 rot = mat4::RotationRollPitchYaw(pitch_, yaw_, 0);
    float3 look = rot.TransformPoint({ 0, 0, 1 }); // forward
    float3 up = rot.TransformPoint({ 0, 1, 0 }); // up
    mat4 newView = mat4::LookAtLH(position_, position_ + look, up);
    view_.view = newView;
    view_.invView = mat4::Inverse(newView);
    const float aspect = float(r->GetWidth()) / float(r->GetHeight());
    const float vfov = 2.f * atan(tan(view_.hfov * 0.5f) / aspect);
    mat4 newProj = mat4::PerspectiveFovLHReverseZ(vfov, aspect, view_.zNear, view_.zFar);
    view_.proj = newProj;
    view_.invProj = mat4::Inverse(newProj);
    view_.position = position_;
    dir_ = look.Normalized();

    viewProj_ = view_.view * view_.proj;
    prevViewProj_ = prevView_ * prevProj_;

    if (!hasPrevViewProj_)
    {
        prevView_ = view_.view;
        prevProj_ = view_.proj;
        prevViewProj_ = viewProj_;
        hasPrevViewProj_ = true;
    }
}

void Camera::ResetHistory()
{
    prevView_ = view_.view;
    prevProj_ = view_.proj;
    prevViewProj_ = viewProj_;
    hasPrevViewProj_ = true;
}
