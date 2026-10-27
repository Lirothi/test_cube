#include "app/camera/Camera.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"

#include <algorithm>
#include <cmath>

void Camera::UpdateFromInput(InputManager& input, float dt) {
    // Toggle camera rotation via the action
    const bool lookHeld = input.IsActionDown("LookToggle") && !input.IsKeyDown(VK_MENU);
    if (lookHeld) {
        if (!input.IsMouseCaptured()) {
            input.SetMouseCapture(true);
        }
        // Scale already includes sensitivity/invert
        lookOwedYaw_ += input.GetActionAxis("LookX");
        lookOwedPitch_ += input.GetActionAxis("LookY");
    } else {
        if (input.IsMouseCaptured()) {
            input.SetMouseCapture(false);
        }
    }
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
        AddYaw(yaw);
        AddPitch(pitch);
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
    // Snapshot last frame's matrices BEFORE anything below overwrites them. Copied straight across
    // rather than through `last*` locals: nothing here reads view_ again until it is rebuilt below,
    // so the locals were a second copy of all five matrices for no one's benefit.
    if (hasPrevViewProj_)
    {
        prevView_ = view_.view;
        prevProj_ = view_.proj;
        prevViewProj_ = viewProj_;
        prevNonJitteredProj_ = nonJitteredProj_;
        prevNonJitteredViewProj_ = nonJitteredViewProj_;
    }

    mat4 rot = GetRotationMatrix();
    float3 look = rot.TransformPoint({ 0, 0, 1 }); // forward
    float3 up = rot.TransformPoint({ 0, 1, 0 }); // up
    mat4 newView = mat4::LookAtLH(position_, position_ + look, up);
    view_.view = newView;
    view_.invView = mat4::Inverse(newView);
    const float aspect = float(r->GetWidth()) / float(r->GetHeight());
    const float vfov = 2.f * atan(tan(view_.hfov * 0.5f) / aspect);
    mat4 baseProj = mat4::PerspectiveFovLHReverseZ(vfov, aspect, view_.zNear, view_.zFar);

    nonJitteredProj_ = baseProj;
    invNonJitteredProj_ = mat4::Inverse(baseProj);

    Math::float2 jitter(0.0f, 0.0f);
	if (r)
    {
        jitter = r->GetCameraJitter();
    }

    mat4 jitteredProj = baseProj;
    if (jitter.x != 0.0f || jitter.y != 0.0f)
    {
        jitteredProj.m._31 += jitter.x * 2.0f;
        jitteredProj.m._32 -= jitter.y * 2.0f;
    }

    view_.proj = jitteredProj;
    view_.invProj = mat4::Inverse(jitteredProj);
    view_.position = position_;
    dir_ = look.Normalized();

    viewProj_ = view_.view * view_.proj;
    nonJitteredViewProj_ = view_.view * nonJitteredProj_;
    // The prev COMBINED matrices are not recomputed: prevView_ * prevProj_ multiplies exactly the
    // operands last frame already multiplied, so it reproduces the value copied above bit for bit.

    if (!hasPrevViewProj_)
    {
        prevView_ = view_.view;
        prevProj_ = view_.proj;
        prevViewProj_ = viewProj_;
        prevNonJitteredProj_ = nonJitteredProj_;
        prevNonJitteredViewProj_ = nonJitteredViewProj_;
        hasPrevViewProj_ = true;
    }
}

void Camera::ResetHistory()
{
    lookOwedYaw_ = lookOwedPitch_ = 0.0f; // a cut: no smoothed look input carries over
    prevView_ = view_.view;
    prevProj_ = view_.proj;
    prevViewProj_ = viewProj_;
    prevNonJitteredProj_ = nonJitteredProj_;
    prevNonJitteredViewProj_ = nonJitteredViewProj_;
    hasPrevViewProj_ = true;
    ++historyRevision_;
}
