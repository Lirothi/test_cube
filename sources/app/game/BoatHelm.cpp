#include "app/game/BoatHelm.h"

// Architecture plan R13.5: PlaySession::TakeHelm / LeaveHelm / TickHelm and the helm's part of PlaySession::HudLine,
// moved verbatim; the session's state they wrote (atHelm_, possessed_, justSpawned_, the camera at the body) is the
// session's again -- it reads what these return.
#include <cmath>
#include <cwchar>

#include "app/game/PlayWorld.h"
#include "app/game/Race.h"
#include "app/game/VesselCamera.h"
#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "input/InputActions.h"
#include "input/InputManager.h"
#include "rendering/view/Camera.h"

namespace game
{
bool BoatHelm::Take(Scene& scene, RenderableObject* vessel)
{
    if (!scene.Vessels().SetHelm(vessel)) { return false; }
    scene.Physics().RemovePlayer();
    world_.vesselCamera.Begin();
    scene.CameraRef().ResetHistory(); // a jump, not a pan
    LOG_INFO(logging::LogCategory::App, "play: at the helm");
    return true;
}

std::optional<BodyPlacement> BoatHelm::Leave(Scene& scene, bool overTheSide)
{
    VesselSystem& boats = scene.Vessels();
    const physics::HelmState helm = boats.GetHelm(); // SetHelm clears it
    boats.SetHelm(nullptr);
    world_.vesselCamera.End();
    LOG_INFO(logging::LogCategory::App, "play: left the helm");
    if (!overTheSide || !helm.exists) { return std::nullopt; }
    // A stride off the hull to starboard, facing it: the water or the ground takes it from there.
    const float yaw = std::atan2(helm.forward.x, helm.forward.z);
    const float across = helm.halfBeam + 0.8f;
    BodyPlacement body;
    body.feet = Math::float3(helm.position.x + std::cos(yaw) * across, helm.position.y + 0.5f, helm.position.z - std::sin(yaw) * across);
    body.cameraYawRad = yaw - 1.5707963f;
    return body;
}

void BoatHelm::Forget(Scene& scene)
{
    scene.Vessels().SetHelm(nullptr); // the boat went with the level
    world_.vesselCamera.End();
}

Controllable::Held BoatHelm::Tick(InputManager& input, Scene& scene, bool controls, float deltaTime)
{
    VesselSystem& boats = scene.Vessels();
    const physics::HelmState& helm = boats.GetHelm();
    if (!helm.exists) // gone with its level, or physics.enabled:0
    {
        world_.vesselCamera.End();
        return Held::Gone;
    }
    Camera& camera = scene.CameraRef();
    const float yawBefore = camera.GetYaw();
    const float pitchBefore = camera.GetPitch();
    // W S / A D (InputManager: the keys count only with `controls`) and what a script says at the helm.
    physics::VesselInput order;
    order.throttle = input.GetActionAxis(actions::kMoveZ) + input.GetActionAxis(actions::kHelmThrottle);
    order.rudder = input.GetActionAxis(actions::kMoveX) + input.GetActionAxis(actions::kHelmRudder);
    if (controls) { scene.CameraControllerRef().AddLookInput(input); }
    scene.CameraControllerRef().TakeOwedLook(deltaTime); // the mouse's own road, its smoothing and sensitivity: what it turned the camera by is the orbit's
    // The autopilot: B hands this helm to the race's bot (game::Race) and takes it back.
    game::Race& race = world_.race;
    if (input.TakeActionPress(actions::kAutopilot))
    {
        if (!race.HasTrack()) { status_("No race course in this level", 3.0); }
        else { status_(race.ToggleAutopilot() ? "Autopilot on" : "Autopilot off", 3.0); }
    }
    if (race.Autopilot()) { order = race.HelmOrder(); }
    if (race.HoldsHelm()) { order = physics::VesselInput{}; } // on the grid, the countdown running
    boats.SetHelmInput(order);
    game::VesselCamera& view = world_.vesselCamera;
    view.Tick(camera.GetYaw() - yawBefore, camera.GetPitch() - pitchBefore, helm.vessel.speed, game::ScriptCamera().orbit,
              game::ScriptCamera().orbitPitch, deltaTime);
    if (input.TakeActionPress(actions::kToggleView))
    {
        // V: behind -> the cockpit -> cinema -> behind.
        view.NextView();
        camera.ResetHistory();
        status_(std::string("Camera: ") + game::VesselCamera::ViewName(view.GetView()) + " (V: the next)", 3.0);
    }
    // The anchor: down under the bow, or up. It holds the boat when nobody is at the helm too; Stop weighs it.
    if (input.TakeActionPress(actions::kAnchor))
    {
        const bool down = boats.ToggleAnchor(helm.object);
        status_(down ? "Anchor down" : "Anchor up", 3.0);
    }
    if (input.TakeActionPress(actions::kInteract)) { return Held::LetGo; } // over the side (the session lets go)
    return Held::Yes;
}

std::wstring BoatHelm::HudLine(const Scene& scene) const
{
    wchar_t line[256];
    const physics::HelmState& helm = scene.Vessels().GetHelm();
    const physics::VesselState& vessel = helm.vessel;
    std::swprintf(line, sizeof(line) / sizeof(line[0]), L"PLAY: at the helm, %.1f kn, throttle %+.0f %%, rudder %+.0f %%%ls   W S throttle   A D rudder   V view: %hs   %ls   E leave",
                  vessel.speed * 1.94384f, vessel.throttle * 100.0f, vessel.rudder * 100.0f, helm.anchored ? L", ANCHORED" : L"",
                  game::VesselCamera::ViewName(world_.vesselCamera.GetView()), helm.anchored ? L"X weigh anchor" : L"X anchor");
    return line;
}

RenderableObject* BoatHelm::InReach(Scene& scene, const Math::float3& feet) const
{
    return scene.Vessels().FindVessel(feet, kHelmReach); // the way aboard: within a few metres of a hull
}
} // namespace game
