#pragma once
#include <DirectXMath.h>
#include "core/math/Math.h"

using namespace DirectX;

class Renderer;

class Camera {
public:
    Camera(
        float3 pos = { 0, 0, -4 },
        float pitch = 0, float yaw = 0)
        : position_(pos), pitch_(pitch), yaw_(yaw)
    {}

    void UpdateFromInput(float dt);

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

    void SetHFov(float fov) { HFov_ = fov; }
	float GetHFov() const { return HFov_; }

	void SetZNearFar(float znear, float zfar) { zNear = znear; zFar = zfar; }
	float GetZNear() const { return zNear; }
	float GetZFar() const { return zFar; }

    // Fetch the view matrix
    const mat4& GetViewMatrix() const {return view;}
	const mat4& GetProjMatrix() const { return proj; }
	const mat4& GetInvViewMatrix() const { return invView; }
	const mat4& GetInvProjMatrix() const { return invProj; }

    const float3& GetDirection() const { return dir_; }

    void CalcMatrices(Renderer* r);

    // For passing to constant buffers/shaders
    const float3& GetPosition() const { return position_; }
    float GetMoveSpeedMult() const { return moveSpeedMultiplier_; }

    // Mouse input (screen-space delta -> yaw/pitch)
    void OnMouseMove(float dx, float dy, float sensitivity = 0.01f) {
        AddYaw(dx * sensitivity);
        AddPitch(dy * sensitivity);
    }

private:
    mat4 view;
    mat4 proj;
	mat4 invView;
	mat4 invProj;
    float3 dir_;
    float3 position_;
    float pitch_; // Up/down, clamp to [-pi/2+eps, pi/2-eps]
    float yaw_;   // Left/right, can wrap freely
    float moveSpeed_ = 3.0f;
    float sprintMultiplier_ = 2.5f;
    float moveSpeedMultiplier_ = 1.0f;
    float HFov_;
    float zNear;
    float zFar;

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