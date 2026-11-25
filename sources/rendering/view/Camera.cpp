#include "rendering/view/Camera.h"
#include "rendering/core/Renderer.h"

#include <cmath>

void Camera::CalcMatrices(Renderer* r)
{
    // Snapshot last frame's matrices BEFORE anything below overwrites them. Copied straight across
    // rather than through `last*` locals: nothing here reads view_ again until it is rebuilt below,
    // so the locals were a second copy of all five matrices for no one's benefit.
    if (hasPrevViewProj_)
    {
        prevView_ = view_;
        prevProj_ = proj_;
        prevViewProj_ = viewProj_;
        prevNonJitteredProj_ = nonJitteredProj_;
        prevNonJitteredViewProj_ = nonJitteredViewProj_;
    }

    mat4 rot = GetRotationMatrix();
    float3 look = rot.TransformPoint({ 0, 0, 1 }); // forward
    float3 up = rot.TransformPoint({ 0, 1, 0 }); // up
    mat4 newView = mat4::LookAtLH(position_, position_ + look, up);
    view_ = newView;
    invView_ = mat4::Inverse(newView);
    const float aspect = float(r->GetWidth()) / float(r->GetHeight());
    // The player's field of view is horizontal at 16:9 and keeps its vertical at any aspect (Hor+); the level's is
    // horizontal at this aspect, as it always was.
    float vfov = 2.f * atan(tan(hfov_ * 0.5f) / aspect);
    effectiveHfov_ = hfov_; // bit for bit what the level says, without the player's
    if (playerFovDeg_ > 0.0f)
    {
        constexpr float kReferenceAspect = 16.0f / 9.0f;
        vfov = 2.f * atan(tan(XMConvertToRadians(playerFovDeg_) * 0.5f) / kReferenceAspect);
        effectiveHfov_ = 2.f * atan(tan(vfov * 0.5f) * aspect);
    }
    mat4 baseProj = mat4::PerspectiveFovLHReverseZ(vfov, aspect, zNear_, zFar_);

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

    proj_ = jitteredProj;
    invProj_ = mat4::Inverse(jitteredProj);
    dir_ = look.Normalized();

    viewProj_ = view_ * proj_;
    nonJitteredViewProj_ = view_ * nonJitteredProj_;
    // The prev COMBINED matrices are not recomputed: prevView_ * prevProj_ multiplies exactly the
    // operands last frame already multiplied, so it reproduces the value copied above bit for bit.

    if (!hasPrevViewProj_)
    {
        prevView_ = view_;
        prevProj_ = proj_;
        prevViewProj_ = viewProj_;
        prevNonJitteredProj_ = nonJitteredProj_;
        prevNonJitteredViewProj_ = nonJitteredViewProj_;
        hasPrevViewProj_ = true;
    }
}

bool Camera::TakePose(const Camera& game)
{
    position_ = game.position_;
    pitch_ = game.pitch_;
    yaw_ = game.yaw_;
    roll_ = game.roll_;
    rotation_ = game.rotation_;
    hfov_ = game.hfov_;
    playerFovDeg_ = game.playerFovDeg_;
    zNear_ = game.zNear_;
    zFar_ = game.zFar_;
    renderLayerMask_ = game.renderLayerMask_;
    if (game.historyRevision_ == takenRevision_)
    {
        return false;
    }
    takenRevision_ = game.historyRevision_;
    return true;
}

void Camera::ResetHistory()
{
    ++lookResets_; // a cut: no smoothed look input carries over (CameraController)
    prevView_ = view_;
    prevProj_ = proj_;
    prevViewProj_ = viewProj_;
    prevNonJitteredProj_ = nonJitteredProj_;
    prevNonJitteredViewProj_ = nonJitteredViewProj_;
    hasPrevViewProj_ = true;
    ++historyRevision_;
}
