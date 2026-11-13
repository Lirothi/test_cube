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
        constexpr float kCinemaOverWater = 0.6f; // m: the lowest a cinema shot stands over the still water
        constexpr float kLookTau = 0.12f;     // s: a cinema shot's aim follows the boat this smoothly (the heave does not shake it)

        float WrapPi(float angle)
        {
            while (angle > kPi) { angle -= 2.0f * kPi; }
            while (angle < -kPi) { angle += 2.0f * kPi; }
            return angle;
        }

        float Follow(float dt, float tau) { return 1.0f - std::exp(-dt / std::max(tau, 1e-4f)); }

        // A world offset of the mesh's local point `local` (already scaled): the hull's frame from its forward and up -- the
        // asset's own forward axis (settings.forward) and up (+Y) map onto them.
        Math::float3 LocalToWorld(const Math::float3& local, const Math::float3& localForward, const Math::float3& forward, const Math::float3& up)
        {
            // Two orthonormal frames (starboard, up, forward): the mesh's own and the hull's in the world.
            const Math::float3 lf = localForward.Normalized();
            const Math::float3 lr = Math::float3(0.0f, 1.0f, 0.0f).Cross(lf).Normalized();
            const Math::float3 lu = lf.Cross(lr);
            const Math::float3 wf = forward.Normalized();
            const Math::float3 wr = up.Cross(wf).Normalized();
            const Math::float3 wu = wf.Cross(wr);
            return wr * local.Dot(lr) + wu * local.Dot(lu) + wf * local.Dot(lf);
        }

        void Aim(Camera& camera, const Math::float3& eye, const Math::float3& at)
        {
            const Math::float3 d = at - eye;
            const float flat = std::sqrt(d.x * d.x + d.z * d.z);
            camera.SetPosition(eye);
            camera.SetYawPitchRoll(std::atan2(d.x, d.z), std::atan2(-d.y, std::max(flat, 1e-3f)), 0.0f);
        }
    }

    VesselPose PoseOf(const physics::HelmState& helm)
    {
        VesselPose pose;
        pose.settings = helm.settings;
        pose.position = helm.position;
        pose.forward = helm.forward;
        pose.up = Math::float3::FromXM(DirectX::XMVector3Rotate(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), helm.rotation.xm()));
        pose.scale = helm.scale;
        pose.speed = helm.vessel.speed;
        return pose;
    }

    VesselPose PoseOf(const physics::VesselHull& hull)
    {
        VesselPose pose;
        pose.settings = hull.settings;
        pose.position = hull.position;
        pose.forward = hull.forward;
        pose.up = hull.up;
        pose.scale = hull.scale;
        pose.speed = hull.speed;
        return pose;
    }

    const char* VesselCamera::ViewName(View view)
    {
        switch (view)
        {
        case View::Helm: return "cockpit";
        case View::Cinema: return "cinema";
        default: return "behind";
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
        cinemaFresh_ = true;
    }

    void VesselCamera::SetView(View view)
    {
        view_ = view;
        orbitYaw_ = 0.0f;
        orbitPitch_ = 0.0f;
        cinemaFresh_ = true;
    }

    void VesselCamera::Tick(float lookYaw, float lookPitch, float speed, float scriptOrbit, float scriptOrbitPitch, float deltaTime)
    {
        dt_ = deltaTime;
        shotTime_ += deltaTime;
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

    void VesselCamera::Apply(Camera& camera, const VesselPose& pose, float waterLevel)
    {
        if (!pose.settings) { return; }
        const physics::VesselSettings& settings = *pose.settings;
        const Math::float3 forward = pose.forward;
        const float hullHeading = std::atan2(forward.x, forward.z);
        if (first_)
        {
            heading_ = hullHeading;
            targetY_ = pose.position.y + settings.cameraHeight;
            first_ = false;
        }
        heading_ = WrapPi(heading_ + WrapPi(hullHeading - heading_) * (1.0f - std::exp(-dt_ / kHeadingTau)));
        targetY_ += (pose.position.y + settings.cameraHeight - targetY_) * (1.0f - std::exp(-dt_ / kHeightTau));

        if (view_ == View::Cinema)
        {
            ApplyCinema(camera, pose, waterLevel);
            return;
        }
        if (view_ == View::Chase)
        {
            const float yaw = heading_ + orbitYaw_;
            const float pitch = std::clamp(kChasePitch + orbitPitch_, -0.3f, 1.3f);
            const Math::float3 look(std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch));
            const Math::float3 target(pose.position.x, targetY_, pose.position.z);
            Math::float3 eye = target - look * (settings.cameraDistance + pull_);
            eye.y = std::max(eye.y, waterLevel + kEyeOverWater);
            camera.SetPosition(eye);
            camera.SetYawPitchRoll(yaw, pitch, 0.0f);
            return;
        }

        // At the helm: the eye rides with the hull, its pitch and its roll too.
        const Math::float3 local(settings.helmEye.x * pose.scale.x, settings.helmEye.y * pose.scale.y, settings.helmEye.z * pose.scale.z);
        camera.SetPosition(pose.position + LocalToWorld(local, settings.forward, forward, pose.up));
        const Math::float3 starboard = pose.up.Cross(forward.Normalized()).Normalized();
        const Math::float3 deckUp = forward.Normalized().Cross(starboard);
        const float hullPitch = std::atan2(-forward.y, std::sqrt(forward.x * forward.x + forward.z * forward.z));
        camera.SetYawPitchRoll(hullHeading + orbitYaw_, hullPitch + kHelmLookDown + orbitPitch_, std::atan2(starboard.y, deckUp.y));
    }

    void VesselCamera::NewShot(Camera& camera, const VesselPose& pose, float waterLevel)
    {
        // The next shot: never the same kind twice running, a side and a length of its own.
        const auto next = [this]()
        {
            seed_ = seed_ * 1664525u + 1013904223u;
            return seed_ >> 8;
        };
        const unsigned count = static_cast<unsigned>(Shot::Count);
        Shot shot = static_cast<Shot>(next() % count);
        if (shot == shot_ && !cinemaFresh_) { shot = static_cast<Shot>((static_cast<unsigned>(shot) + 1u + next() % (count - 1u)) % count); }
        shot_ = shot;
        side_ = (next() & 1u) ? 1.0f : -1.0f;
        shotLength_ = 5.0f + static_cast<float>(next() % 1000u) * 0.003f; // 5-8 s
        shotTime_ = 0.0f;
        orbit_ = 0.0f;
        cinemaFresh_ = false;

        const Math::float3 way = Math::float3(std::sin(heading_), 0.0f, std::cos(heading_));
        const Math::float3 across(way.z, 0.0f, -way.x); // starboard of the way
        const float speed = std::max(std::fabs(pose.speed), 0.0f);
        const Math::float3 at = pose.position;
        switch (shot_)
        {
        case Shot::Pass:
            // A camera standing ahead, off to one side, a little over the water: the boat comes at it and storms past.
            eye_ = at + way * (25.0f + 3.0f * speed) + across * (side_ * (10.0f + static_cast<float>(next() % 8u)));
            eye_.y = waterLevel + 2.0f + static_cast<float>(next() % 3u);
            shotLength_ = std::clamp((25.0f + 3.0f * speed + 30.0f) / std::max(speed, 4.0f), 5.0f, 10.0f);
            break;
        case Shot::Abeam:
            eye_ = at + across * (side_ * 16.0f) + way * 4.0f;
            eye_.y = waterLevel + 3.0f;
            break;
        case Shot::Drone:
            eye_ = at - way * 22.0f + across * (side_ * 8.0f);
            eye_.y = waterLevel + 30.0f;
            break;
        case Shot::Low:
            eye_ = at - way * 10.0f + across * (side_ * 3.5f);
            eye_.y = waterLevel + 0.9f;
            break;
        default: // Bow: ahead of it, looking back at the boat coming on
            eye_ = at + way * 14.0f + across * (side_ * 2.5f);
            eye_.y = waterLevel + 2.2f;
            break;
        }
        look_ = at;
        camera.ResetHistory(); // a cut, not a pan: DLSS and every temporal filter start again
    }

    void VesselCamera::ApplyCinema(Camera& camera, const VesselPose& pose, float waterLevel)
    {
        const Math::float3 way = Math::float3(std::sin(heading_), 0.0f, std::cos(heading_));
        const Math::float3 across(way.z, 0.0f, -way.x);
        const Math::float3 at = pose.position;
        // A pass ends once the boat is well past the camera, any shot when its time is up.
        const bool passed = shot_ == Shot::Pass && (at - eye_).Dot(way) > 35.0f;
        if (cinemaFresh_ || shotTime_ >= shotLength_ || passed || (at - eye_).Length() > 400.0f) { NewShot(camera, pose, waterLevel); }

        Math::float3 target = at;
        Math::float3 want = eye_;
        float tau = 0.0f; // 0 = the camera stands
        switch (shot_)
        {
        case Shot::Pass:
            break;
        case Shot::Abeam:
            want = at + across * (side_ * 16.0f) + way * 4.0f;
            want.y = waterLevel + 3.0f;
            target = at + way * 6.0f;
            tau = 0.4f;
            break;
        case Shot::Drone:
        {
            orbit_ += 0.12f * dt_ * side_;
            const float c = std::cos(orbit_);
            const float s = std::sin(orbit_);
            const Math::float3 back = (way * -22.0f + across * (side_ * 8.0f));
            want = at + Math::float3(back.x * c - back.z * s, 0.0f, back.x * s + back.z * c);
            want.y = waterLevel + 30.0f;
            target = at + way * 10.0f;
            tau = 1.0f;
            break;
        }
        case Shot::Low:
            want = at - way * 10.0f + across * (side_ * 3.5f);
            want.y = waterLevel + 0.9f;
            target = at + way * 12.0f;
            tau = 0.25f;
            break;
        default:
            want = at + way * 14.0f + across * (side_ * 2.5f);
            want.y = waterLevel + 2.2f;
            target = at;
            tau = 0.3f;
            break;
        }
        if (tau > 0.0f) { eye_ = eye_ + (want - eye_) * Follow(dt_, tau); }
        eye_.y = std::max(eye_.y, waterLevel + kCinemaOverWater);
        target.y += 0.8f; // the deck, not the keel
        look_ = look_ + (target - look_) * Follow(dt_, kLookTau);
        Aim(camera, eye_, look_);
    }
}
