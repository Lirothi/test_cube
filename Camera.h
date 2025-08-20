#pragma once
#include <DirectXMath.h>
#include "Math.h"

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

    // Управление положением камеры
    void MoveForward(float d)   { MoveRelative(0, 0, d); }
    void MoveRight(float d)     { MoveRelative(d, 0, 0); }
    void MoveUp(float d)        { MoveRelative(0, d, 0); }

    // Управление углами (в радианах)
    void AddPitch(float dp) { pitch_ += dp; ClampPitch(); }
    void AddYaw(float dy)   { yaw_   += dy; WrapYaw();    }

    // Сеттеры
    void SetPosition(const float3& pos) { position_ = pos; }
    void SetYawPitch(float yaw, float pitch) {
        yaw_ = yaw; pitch_ = pitch; ClampPitch(); WrapYaw();
    }

    // Получить view-матрицу
    mat4 GetViewMatrix() const {
        mat4 rot = mat4::RotationRollPitchYaw(pitch_, yaw_, 0);
		float3 look = rot.TransformPoint({ 0, 0, 1 }); // forward
		float3 up = rot.TransformPoint({ 0, 1, 0 }); // up
        return mat4::LookAtLH(position_, position_ + look, up);
    }

    // Для передачи в CB/шедер
    float3 GetPosition() const { return position_; }

    // Управление мышью (ScreenSpace -> DeltaYaw/Pitch)
    void OnMouseMove(float dx, float dy, float sensitivity = 0.01f) {
        AddYaw(dx * sensitivity);
        AddPitch(dy * sensitivity);
    }

private:
    float3 position_;
    float pitch_; // вверх/вниз, ограничить [-pi/2+eps, pi/2-eps]
    float yaw_;   // влево/вправо, можно не ограничивать

    void ClampPitch() {
        const float limit = XM_PIDIV2 - 0.01f;
        pitch_ = Clamp(pitch_, -limit, limit);
    }
    void WrapYaw() {
        if (yaw_ > XM_PI) {yaw_ -= XM_2PI;}
        if (yaw_ < -XM_PI) {yaw_ += XM_2PI;}
    }

    // Локальное смещение (forward/right/up в системе камеры)
    void MoveRelative(float dx, float dy, float dz) {
		float3 move = { dx, dy, dz };
		mat4 rot = mat4::RotationRollPitchYaw(pitch_, yaw_, 0);
        move = rot.TransformPoint(move);
        position_ = position_ + move;
    }
};