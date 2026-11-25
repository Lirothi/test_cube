#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include "core/math/Math.h"
#include "rendering/RenderLayers.h"

using namespace DirectX;

class Renderer;

// The camera's view (architecture plan R1.4): where it stands, how it looks, its lens and layers, and the matrices the
// renderer reads -- with the TAA jitter and last frame's history. The free-fly and look input, the mouse smoothing and
// the player's look settings are app/camera/CameraController's; the scene's main view (its queue, its frustum) is the
// Scene's, which copies this camera's matrices into it (Scene::SyncCameraView).
class Camera {
public:
    Camera(
        float3 pos = { 0, 0, -4 },
        float pitch = 0, float yaw = 0, float roll = 0)
        : position_(pos), pitch_(pitch), yaw_(yaw), roll_(roll)
    {
        UpdateRotation();
        hfov_ = XMConvertToRadians(90.0f);
        zNear_ = 0.01f;
        zFar_ = 10000.0f;
    }

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
    // Setting the orientation drops whatever the smoothed look input still owed (CameraController): a camera put
    // somewhere by code must not glide on from there. The controller sees it by GetLookResets.
    void SetYawPitch(float yaw, float pitch) {
        yaw_ = yaw; pitch_ = pitch; ClampPitch(); WrapYaw(); UpdateRotation(); ++lookResets_;
    }
    // Roll is deliberately NOT clamped the way pitch is: banking past vertical is a legitimate
    // pose, and the gimbal-lock reason for clamping pitch does not apply to the last angle.
    void SetYawPitchRoll(float yaw, float pitch, float roll) {
        yaw_ = yaw; pitch_ = pitch; roll_ = roll; ClampPitch(); WrapYaw(); WrapRoll(); UpdateRotation();
        ++lookResets_;
    }
    void SetRoll(float roll) { roll_ = roll; WrapRoll(); UpdateRotation(); }
    float GetYaw() const { return yaw_; }
    float GetPitch() const { return pitch_; }
    float GetRoll() const { return roll_; }
    // THE camera orientation. Cached (rebuilt only when an angle changes) and shared, so the view
    // matrix, movement, the HUD readout and --cam-rot can never disagree about the rotation order
    // and none of them pays for a matrix build per call.
    const mat4& GetRotationMatrix() const { return rotation_; }

    void SetHFov(float fov) { hfov_ = fov; }
    // The field of view the projection uses: the level's (SetHFov), or the player's at this aspect (SetPlayerFovDeg),
    // as of the last CalcMatrices.
    float GetHFov() const { return effectiveHfov_ > 0.0f ? effectiveHfov_ : hfov_; }
    // The level's own (SetHFov), whatever the player's: what the scene's main view carries.
    float GetBaseHFov() const { return hfov_; }
    // 0 = the level's; otherwise degrees horizontal at 16:9, Hor+ (g_cameraPlayerFovDeg).
    void SetPlayerFovDeg(float deg) { playerFovDeg_ = deg; }

    void SetZNearFar(float znear, float zfar) { zNear_ = znear; zFar_ = zfar; }
    float GetZNear() const { return zNear_; }
    float GetZFar() const { return zFar_; }

    // Fetch the view matrix
    const mat4& GetViewMatrix() const { return view_; }
    const mat4& GetProjMatrix() const { return proj_; }
    const mat4& GetInvViewMatrix() const { return invView_; }
    const mat4& GetInvProjMatrix() const { return invProj_; }
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
    // Counts the orientation set by code (SetYawPitch, SetYawPitchRoll) and the cuts (ResetHistory): the look input a
    // CameraController still owes this camera is dropped when it changes.
    uint64_t GetLookResets() const { return lookResets_; }

    const float3& GetDirection() const { return dir_; }

    void CalcMatrices(Renderer* r);

    // Game thread plan G2: the render half's camera takes the GAME camera's pose at the hand-over -- where it stands,
    // how it looks, its lens and layers -- and keeps its own matrices, jitter and history. A cut (the game camera's
    // ResetHistory, which every caller makes right after its CalcMatrices) is carried by the revision: TakePose returns
    // true once per cut, and the render camera repeats the cut after ITS CalcMatrices, as the game side did.
    bool TakePose(const Camera& game);

    // For passing to constant buffers/shaders
    const float3& GetPosition() const { return position_; }

    uint32_t GetRenderLayerMask() const { return renderLayerMask_; }
    void SetRenderLayerMask(uint32_t mask) { renderLayerMask_ = mask; }
    void EnableRenderLayer(RenderLayer layer) { EnableLayer(renderLayerMask_, layer); }
    void DisableRenderLayer(RenderLayer layer) { DisableLayer(renderLayerMask_, layer); }

private:
    mat4 view_ = mat4::Identity();
    mat4 proj_ = mat4::Identity();
    mat4 invView_ = mat4::Identity();
    mat4 invProj_ = mat4::Identity();
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
    float hfov_ = 0.0f;  // the level's (SetHFov)
    float zNear_ = 0.0f;
    float zFar_ = 0.0f;
    uint32_t renderLayerMask_ = kRenderLayerAll;
    float playerFovDeg_ = 0.0f;  // SetPlayerFovDeg
    float effectiveHfov_ = 0.0f; // CalcMatrices: what the projection took (0 before the first)
    bool hasPrevViewProj_ = false;
    uint64_t historyRevision_ = 0u;
    uint64_t takenRevision_ = 0u; // TakePose: the game camera's revision last taken
    uint64_t lookResets_ = 0u;    // GetLookResets

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
