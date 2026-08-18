#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include "core/math/Math.h"
#include "rendering/RenderLayers.h"
#include "app/scene/SceneView.h"

using namespace DirectX;

class InputManager;
class Renderer;

class Camera {
public:
    Camera(
        float3 pos = { 0, 0, -4 },
        float pitch = 0, float yaw = 0, float roll = 0)
        : position_(pos), pitch_(pitch), yaw_(yaw), roll_(roll)
    {
        UpdateRotation();
        view_.type = SceneView::Type::Camera;
        view_.renderLayerMask = kRenderLayerAll;
        view_.hfov = XMConvertToRadians(90.0f);
        view_.zNear = 0.01f;
        view_.zFar = 10000.0f;
    }

    void UpdateFromInput(InputManager& input, float dt);

    // Camera movement helpers
    void MoveForward(float d)   { MoveRelative(0, 0, d); }
    void MoveRight(float d)     { MoveRelative(d, 0, 0); }
    void MoveUp(float d)        { MoveRelative(0, d, 0); }

    // Angle controls (in radians)
    // EVERY mutation of pitch/yaw/roll must end in UpdateRotation() — the matrix is cached, and a
    // path that skips it leaves the view, movement and the HUD reading a stale orientation.
    void AddPitch(float dp) { pitch_ += dp; ClampPitch(); UpdateRotation(); }
    void AddYaw(float dy)   { yaw_   += dy; WrapYaw();    UpdateRotation(); }
    void AddRoll(float dr)  { roll_  += dr; WrapRoll();   UpdateRotation(); }

    // Setters
    void SetPosition(const float3& pos) { position_ = pos; }
    void SetYawPitch(float yaw, float pitch) {
        yaw_ = yaw; pitch_ = pitch; ClampPitch(); WrapYaw(); UpdateRotation();
    }
    // Roll is deliberately NOT clamped the way pitch is: banking past vertical is a legitimate
    // pose, and the gimbal-lock reason for clamping pitch does not apply to the last angle.
    void SetYawPitchRoll(float yaw, float pitch, float roll) {
        yaw_ = yaw; pitch_ = pitch; roll_ = roll; ClampPitch(); WrapYaw(); WrapRoll(); UpdateRotation();
    }
    void SetRoll(float roll) { roll_ = roll; WrapRoll(); UpdateRotation(); }
    float GetYaw() const { return yaw_; }
    float GetPitch() const { return pitch_; }
    float GetRoll() const { return roll_; }
    // THE camera orientation. Cached (rebuilt only when an angle changes) and shared, so the view
    // matrix, movement, the HUD readout and --cam-rot can never disagree about the rotation order
    // and none of them pays for a matrix build per call.
    const mat4& GetRotationMatrix() const { return rotation_; }

    void SetHFov(float fov) { view_.hfov = fov; }
    float GetHFov() const { return view_.hfov; }

    void SetZNearFar(float znear, float zfar) { view_.zNear = znear; view_.zFar = zfar; }
    float GetZNear() const { return view_.zNear; }
    float GetZFar() const { return view_.zFar; }

    // Fetch the view matrix
    const mat4& GetViewMatrix() const { return view_.view; }
    const mat4& GetProjMatrix() const { return view_.proj; }
    const mat4& GetInvViewMatrix() const { return view_.invView; }
    const mat4& GetInvProjMatrix() const { return view_.invProj; }
    const mat4& GetPrevViewMatrix() const { return prevView_; }
    const mat4& GetPrevProjMatrix() const { return prevProj_; }
    const mat4& GetProjMatrixNoJitter() const { return nonJitteredProj_; }
    const mat4& GetInvProjMatrixNoJitter() const { return invNonJitteredProj_; }
    const mat4& GetPrevProjMatrixNoJitter() const { return prevNonJitteredProj_; }
    const mat4& GetViewProjMatrix() const { return viewProj_; }
    const mat4& GetPrevViewProjMatrix() const { return prevViewProj_; }
    const mat4& GetViewProjMatrixNoJitter() const { return nonJitteredViewProj_; }
    const mat4& GetPrevViewProjMatrixNoJitter() const { return prevNonJitteredViewProj_; }
    void ResetHistory();
    // Changes whenever an explicit camera cut invalidates all previous-frame reprojection data.
    // Temporal consumers compare this revision instead of trying to infer a cut from a large
    // camera delta (a fast, continuous move is still valid history).
    uint64_t GetHistoryRevision() const { return historyRevision_; }

    const float3& GetDirection() const { return dir_; }

    void CalcMatrices(Renderer* r);

    SceneView& GetView() { return view_; }
    const SceneView& GetView() const { return view_; }

    // For passing to constant buffers/shaders
    const float3& GetPosition() const { return position_; }
    float GetMoveSpeedMult() const { return moveSpeedMultiplier_; }

    uint32_t GetRenderLayerMask() const { return renderLayerMask_; }
    void SetRenderLayerMask(uint32_t mask)
    {
        renderLayerMask_ = mask;
        view_.renderLayerMask = mask;
    }
    void EnableRenderLayer(RenderLayer layer)
    {
        EnableLayer(renderLayerMask_, layer);
        view_.renderLayerMask = renderLayerMask_;
    }
    void DisableRenderLayer(RenderLayer layer)
    {
        DisableLayer(renderLayerMask_, layer);
        view_.renderLayerMask = renderLayerMask_;
    }

    // Mouse input (screen-space delta -> yaw/pitch)
    void OnMouseMove(float dx, float dy, float sensitivity = 0.01f) {
        AddYaw(dx * sensitivity);
        AddPitch(dy * sensitivity);
    }

private:
    SceneView view_{};
    mat4 prevView_ = mat4::Identity();
    mat4 prevProj_ = mat4::Identity();
    mat4 viewProj_ = mat4::Identity();
    mat4 prevViewProj_ = mat4::Identity();
    mat4 nonJitteredProj_ = mat4::Identity();
    mat4 invNonJitteredProj_ = mat4::Identity();
    mat4 nonJitteredViewProj_ = mat4::Identity();
    mat4 prevNonJitteredProj_ = mat4::Identity();
    mat4 prevNonJitteredViewProj_ = mat4::Identity();
    float3 dir_;
    float3 position_;
    float pitch_; // Up/down, clamp to [-pi/2+eps, pi/2-eps]
    float yaw_;   // Left/right, can wrap freely
    float roll_ = 0.0f; // Bank about the view axis; wraps freely, never clamped
    mat4 rotation_ = mat4::Identity(); // cache of RotationRollPitchYaw(pitch_, yaw_, roll_)
    float moveSpeed_ = 3.0f;
    float sprintMultiplier_ = 2.5f;
    float moveSpeedMultiplier_ = 1.0f;
    uint32_t renderLayerMask_ = kRenderLayerAll;
    bool hasPrevViewProj_ = false;
    uint64_t historyRevision_ = 0u;

    void ClampPitch() {
        const float limit = XM_PIDIV2 - 0.01f;
        pitch_ = Clamp(pitch_, -limit, limit);
    }
    void WrapYaw() {
        while (yaw_ > XM_PI) { yaw_ -= XM_2PI; }
        while (yaw_ < -XM_PI) { yaw_ += XM_2PI; }
    }
    void WrapRoll() {
        while (roll_ > XM_PI) { roll_ -= XM_2PI; }
        while (roll_ < -XM_PI) { roll_ += XM_2PI; }
    }
    void UpdateRotation() { rotation_ = mat4::RotationRollPitchYaw(pitch_, yaw_, roll_); }

    // Local offset (forward/right/up in camera space)
    void MoveRelative(float dx, float dy, float dz) {
		float3 move = { dx, dy, dz };
		mat4 rot = GetRotationMatrix(); // rolled too, so "right"/"up" follow a banked camera
        move = rot.TransformPoint(move);
        position_ = position_ + move;
    }
};
