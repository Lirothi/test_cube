#pragma once
#include <DirectXMath.h>
#include "core/Math.h"

using namespace DirectX;
class ActionMap;  // forward
class InputManager;

class Camera {
public:
    Camera(
        float3 pos = { 0, 0, -4 },
        float pitch = 0, float yaw = 0)
        : position_(pos), pitch_(pitch), yaw_(yaw)
    {}

    void UpdateFromActions(InputManager& input, const ActionMap& map, float dt);

    // Camera movement helpers
    void MoveForward(float d)   { MoveRelative(0, 0, d); }
    void MoveRight(float d)     { MoveRelative(d, 0, 0); }
    void MoveUp(float d)        { MoveRelative(0, d, 0); }

    // Angle controls (in radians)
    void AddPitch(float dp) { pitch_ += dp; ClampPitch(); }
    void AddYaw(float dy)   { yaw_   += dy; WrapYaw();    }

    // Setters
    void SetPosition(const float3& pos) { position_ = pos; }
    void SetYawPitch(float yaw, float pitch) {
        yaw_ = yaw; pitch_ = pitch; ClampPitch(); WrapYaw();
    }

    // Fetch the view matrix
    mat4 GetViewMatrix() const {
        mat4 rot = mat4::RotationRollPitchYaw(pitch_, yaw_, 0);
		float3 look = rot.TransformPoint({ 0, 0, 1 }); // forward
		float3 up = rot.TransformPoint({ 0, 1, 0 }); // up
        return mat4::LookAtLH(position_, position_ + look, up);
    }

    // For passing to constant buffers/shaders
    const float3& GetPosition() const { return position_; }

    // Mouse input (screen-space delta -> yaw/pitch)
    void OnMouseMove(float dx, float dy, float sensitivity = 0.01f) {
        AddYaw(dx * sensitivity);
        AddPitch(dy * sensitivity);
    }

private:
    float3 position_;
    float pitch_; // Up/down, clamp to [-pi/2+eps, pi/2-eps]
    float yaw_;   // Left/right, can wrap freely
    float moveSpeed_ = 3.0f;
    float sprintMultiplier_ = 2.5f;
    float moveSpeedMultiplier_ = 1.0f;

    void ClampPitch() {
        const float limit = XM_PIDIV2 - 0.01f;
        pitch_ = Clamp(pitch_, -limit, limit);
    }
    void WrapYaw() {
        while (yaw_ > XM_PI) { yaw_ -= XM_2PI; }
        while (yaw_ < -XM_PI) { yaw_ += XM_2PI; }
    }

    // Local offset (forward/right/up in camera space)
    void MoveRelative(float dx, float dy, float dz) {
		float3 move = { dx, dy, dz };
		mat4 rot = mat4::RotationRollPitchYaw(pitch_, yaw_, 0);
        move = rot.TransformPoint(move);
        position_ = position_ + move;
    }
};