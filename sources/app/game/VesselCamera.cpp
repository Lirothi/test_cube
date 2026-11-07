#include "app/game/VesselCamera.h"

#include <algorithm>
#include <cmath>

#include "app/camera/Camera.h"

namespace game
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        constexpr float kHeadingTau = 0.6f;   // s: how slowly the chase camera follows the bow
        constexpr float kHeightTau = 0.5f;    // s: and the hull's rise and fall
        constexpr float kPullTau = 1.2f;      // s: and its speed
        constexpr float kPullPerSpeed = 0.25f; // m further back per m/s: the speed reads in the frame, with the hull still at its middle
        constexpr float kChasePitch = 0.2f;   // rad: looking this far down at the hull
        constexpr float kReturnAfter = 2.0f;  // s without the mouse, under way: the view swings back behind the boat
        constexpr float kReturnTau = 1.0f;    // s
        constexpr float kEyeOverWater = 1.5f; // m: a chase camera does not go under, and a wave stands higher than still water
        constexpr float kMinUnderWay = 1.0f;  // m/s
        constexpr float kHelmLookDown = 0.1f; // rad: a helmsman looks at the water ahead, over the bow

        float WrapPi(float angle)
        {
            while (angle > kPi) { angle -= 2.0f * kPi; }
            while (angle < -kPi) { angle += 2.0f * kPi; }
            return angle;
        }
    }

    void VesselCamera::Begin()
    {
        active_ = true;
        first_ = true;
        orbitYaw_ = 0.0f;
        orbitPitch_ = 0.0f;
        sinceLook_ = 0.0f;
        pull_ = 0.0f;
    }

    void VesselCamera::Tick(float lookYaw, float lookPitch, float speed, float scriptOrbit, float scriptOrbitPitch, float deltaTime)
    {
        dt_ = deltaTime;
        if (lookYaw != 0.0f || lookPitch != 0.0f) { sinceLook_ = 0.0f; }
        else { sinceLook_ += deltaTime; }
        orbitYaw_ = WrapPi(orbitYaw_ + WrapPi(lookYaw));
        orbitPitch_ = std::clamp(orbitPitch_ + lookPitch, -1.2f, 1.2f);
        pull_ += (kPullPerSpeed * std::max(speed, 0.0f) - pull_) * (1.0f - std::exp(-deltaTime / kPullTau));
        if (scriptOrbit != 0.0f)
        {
            orbitYaw_ = scriptOrbit;
            sinceLook_ = 0.0f;
        }
        if (scriptOrbitPitch != 0.0f)
        {
            orbitPitch_ = std::clamp(scriptOrbitPitch, -1.2f, 1.2f);
            sinceLook_ = 0.0f;
        }
        if (view_ == View::Chase && sinceLook_ > kReturnAfter && std::fabs(speed) > kMinUnderWay)
        {
            const float share = 1.0f - std::exp(-deltaTime / kReturnTau);
            orbitYaw_ -= orbitYaw_ * share;
            orbitPitch_ -= orbitPitch_ * share;
        }
    }

    void VesselCamera::Apply(Camera& camera, const physics::HelmState& helm, float waterLevel)
    {
        if (!helm.settings) { return; }
        const physics::VesselSettings& settings = *helm.settings;
        const Math::float3 forward = helm.forward;
        const float hullHeading = std::atan2(forward.x, forward.z);
        if (first_)
        {
            heading_ = hullHeading;
            targetY_ = helm.position.y + settings.cameraHeight;
            first_ = false;
        }
        heading_ = WrapPi(heading_ + WrapPi(hullHeading - heading_) * (1.0f - std::exp(-dt_ / kHeadingTau)));
        targetY_ += (helm.position.y + settings.cameraHeight - targetY_) * (1.0f - std::exp(-dt_ / kHeightTau));

        if (view_ == View::Chase)
        {
            const float yaw = heading_ + orbitYaw_;
            const float pitch = std::clamp(kChasePitch + orbitPitch_, -0.3f, 1.3f);
            const Math::float3 look(std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch));
            const Math::float3 target(helm.position.x, targetY_, helm.position.z);
            Math::float3 eye = target - look * (settings.cameraDistance + pull_);
            eye.y = std::max(eye.y, waterLevel + kEyeOverWater);
            camera.SetPosition(eye);
            camera.SetYawPitchRoll(yaw, pitch, 0.0f);
            return;
        }

        // At the helm: the eye rides with the hull, its pitch and its roll too.
        const Math::float3 local(settings.helmEye.x * helm.scale.x, settings.helmEye.y * helm.scale.y, settings.helmEye.z * helm.scale.z);
        camera.SetPosition(helm.position + Math::float3::FromXM(DirectX::XMVector3Rotate(local.xm(), helm.rotation.xm())));
        const float hullPitch = std::atan2(-forward.y, std::sqrt(forward.x * forward.x + forward.z * forward.z));
        camera.SetYawPitchRoll(hullHeading + orbitYaw_, hullPitch + kHelmLookDown + orbitPitch_, std::atan2(helm.starboardY, helm.upY));
    }
}
