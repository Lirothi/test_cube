#include "app/game/PlaySession.h"

#include <cmath>
#include <cwchar>
#include <utility>

#include "imgui.h"

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"

void PlaySession::PlaceAtStart(Renderer& renderer, Scene& scene, const game::PlayerStart* start)
{
    Camera& camera = scene.CameraRef();
    physics::PlayerSettings& settings = physics::GameSettings().player;
    settings = settingsBefore_;
    if (start) { game::ApplyPlayerKeys(*start, settings); }

    const Math::float3 feet = start ? start->feet : Math::float3(camera.GetPosition().x, camera.GetPosition().y - settings.eyeHeight, camera.GetPosition().z);
    const float yaw = start ? start->yawRad : camera.GetYaw();
    const float pitch = start ? start->pitchRad : camera.GetPitch();
    scene.Physics().SpawnPlayer(feet);
    camera.SetPosition(Math::float3(feet.x, feet.y + settings.eyeHeight, feet.z));
    camera.SetYawPitchRoll(yaw, pitch, 0.0f);
    camera.CalcMatrices(&renderer);
    camera.ResetHistory(); // a jump, not a pan
    possessed_ = true;
    justSpawned_ = true;
    LOG_INFO(logging::LogCategory::App, "play: the player at {:.2f} {:.2f} {:.2f}, {}", feet.x, feet.y, feet.z, start ? "the level's Player Start" : "from the camera");
}

void PlaySession::Begin(Renderer& renderer, Scene& scene, InputManager& input, const game::PlayerStart* start, bool takeMouse, bool stopOnLevelChange)
{
    if (playing_) { return; }
    const Camera& camera = scene.CameraRef();
    cameraPosition_ = camera.GetPosition();
    cameraYaw_ = camera.GetYaw();
    cameraPitch_ = camera.GetPitch();
    cameraRoll_ = camera.GetRoll();
    settingsBefore_ = physics::GameSettings().player;
    playing_ = true;
    mouseFree_ = !takeMouse;
    mouseTaken_ = false;
    stopOnLevelChange_ = stopOnLevelChange;
    status_.clear();
    if (input.IsMouseCaptured()) { input.SetMouseCapture(false); }
    PlaceAtStart(renderer, scene, start);
}

void PlaySession::End(Renderer& renderer, Scene& scene, InputManager& input)
{
    if (!playing_) { return; }
    playing_ = false;
    possessed_ = false;
    LeaveHelm(scene, false);
    scene.Physics().RemovePlayer();
    physics::GameSettings().player = settingsBefore_;
    input.SetMouseCapture(false);
    Camera& camera = scene.CameraRef();
    camera.SetPosition(cameraPosition_);
    camera.SetYawPitchRoll(cameraYaw_, cameraPitch_, cameraRoll_);
    camera.CalcMatrices(&renderer);
    camera.ResetHistory();
    LOG_INFO(logging::LogCategory::App, "play: stopped, the camera back where it stood");
}

void PlaySession::TakeHelm(Scene& scene, RenderableObject* vessel)
{
    physics::World& world = scene.Physics();
    if (!world.SetHelm(vessel)) { return; }
    atHelm_ = true;
    vesselInReach_ = false;
    world.RemovePlayer();
    scene.VesselCameraRef().Begin();
    scene.CameraRef().ResetHistory(); // a jump, not a pan
    LOG_INFO(logging::LogCategory::App, "play: at the helm");
}

void PlaySession::LeaveHelm(Scene& scene, bool overTheSide)
{
    if (!atHelm_) { return; }
    atHelm_ = false;
    physics::World& world = scene.Physics();
    const physics::HelmState helm = world.GetHelm(); // SetHelm clears it
    world.SetHelm(nullptr);
    scene.VesselCameraRef().End();
    LOG_INFO(logging::LogCategory::App, "play: left the helm");
    if (!overTheSide || !helm.exists) { return; }
    // A stride off the hull to starboard, facing it: the water or the ground takes it from there.
    const float yaw = std::atan2(helm.forward.x, helm.forward.z);
    const float across = helm.halfBeam + 0.8f;
    const Math::float3 feet(helm.position.x + std::cos(yaw) * across, helm.position.y + 0.5f, helm.position.z - std::sin(yaw) * across);
    world.SpawnPlayer(feet);
    possessed_ = true;
    justSpawned_ = true;
    Camera& camera = scene.CameraRef();
    camera.SetPosition(Math::float3(feet.x, feet.y + physics::GameSettings().player.eyeHeight, feet.z));
    camera.SetYawPitchRoll(yaw - 1.5707963f, 0.0f, 0.0f);
    camera.ResetHistory();
}

void PlaySession::TickHelm(InputManager& input, Scene& scene, bool controls, float deltaTime)
{
    physics::World& world = scene.Physics();
    const physics::HelmState& helm = world.GetHelm();
    if (!helm.exists) // gone with its level, or physics.enabled:0
    {
        atHelm_ = false;
        scene.VesselCameraRef().End();
        return;
    }
    physics::PlayerSettings& tuning = physics::GameSettings().player;
    Camera& camera = scene.CameraRef();
    const float yawBefore = camera.GetYaw();
    const float pitchBefore = camera.GetPitch();
    physics::VesselInput order;
    if (controls)
    {
        order.throttle = input.GetActionAxis("MoveZ");
        order.rudder = input.GetActionAxis("MoveX");
        camera.AddLookInput(input);
    }
    camera.TakeOwedLook(deltaTime); // the mouse's own road, its smoothing and sensitivity: what it turned the camera by is the orbit's
    order.throttle += tuning.testThrottle;
    order.rudder += tuning.testRudder;
    world.SetHelmInput(order);
    game::VesselCamera& view = scene.VesselCameraRef();
    view.Tick(camera.GetYaw() - yawBefore, camera.GetPitch() - pitchBefore, helm.vessel.speed, tuning.testOrbit, deltaTime);
    const bool scriptView = std::exchange(tuning.testView, false);
    if (scriptView || (controls && input.WasActionPressed("ToggleView")))
    {
        view.ToggleView();
        camera.ResetHistory();
    }
    const bool scriptLeave = std::exchange(tuning.testInteract, false);
    if (scriptLeave || (controls && input.WasActionPressed("Interact"))) { LeaveHelm(scene, true); }
}

void PlaySession::TogglePossess(Scene& scene)
{
    if (!playing_ || atHelm_) { return; }
    possessed_ = !possessed_;
    scene.Physics().SetPlayerPossessed(possessed_);
    LOG_INFO(logging::LogCategory::App, "play: {}", possessed_ ? "possessed" : "ejected, the camera is free");
}

void PlaySession::Tick(InputManager& input, Scene& scene, float deltaTime, bool uiWantsCursor, bool uiCapturingMouse)
{
    // The window lost the focus: InputManager has let the mouse go, and it stays free until a click brings it back.
    if (mouseTaken_ && !input.IsMouseCaptured()) { mouseFree_ = true; }
    if (mouseFree_ && !uiWantsCursor && !uiCapturingMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { mouseFree_ = false; }
    const bool controls = !mouseFree_ && !uiWantsCursor;
    const bool take = controls && input.WindowIsForeground(); // the mouse is the desktop's: a window in the background keeps the keys and leaves it alone
    if (take != input.IsMouseCaptured()) { input.SetMouseCapture(take); }
    mouseTaken_ = take;

    Camera& camera = scene.CameraRef();
    physics::World& world = scene.Physics();
    if (atHelm_)
    {
        TickHelm(input, scene, controls, deltaTime);
        return;
    }
    // The way aboard: within a few metres of a hull, the key takes the helm.
    constexpr float kHelmReach = 3.0f;
    const physics::PlayerState onFoot = world.GetPlayerState();
    RenderableObject* vessel = possessed_ && onFoot.exists ? world.FindVessel(onFoot.feet, kHelmReach) : nullptr;
    vesselInReach_ = vessel != nullptr;
    const bool scriptInteract = std::exchange(physics::GameSettings().player.testInteract, false);
    if (vessel && (scriptInteract || (controls && input.WasActionPressed("Interact"))))
    {
        TakeHelm(scene, vessel);
        if (atHelm_)
        {
            TickHelm(input, scene, controls, deltaTime);
            return;
        }
    }
    // No body and none coming (an empty level has no world, physics.enabled:0): the camera flies, as ejected.
    const bool hasBody = possessed_ && (justSpawned_ || world.GetPlayerState().exists);
    justSpawned_ = false;
    if (hasBody)
    {
        controller_.Tick(input, camera, world, controls, deltaTime);
    }
    else
    {
        world.SetPlayerInput(physics::PlayerInput{});
        if (controls) { camera.UpdateFromInput(input, deltaTime, true); }
    }
}

bool PlaySession::OnLevelLoaded(Renderer& renderer, Scene& scene, InputManager& input, const game::PlayerStart* start)
{
    if (!playing_) { return true; }
    atHelm_ = false; // the boat went with the level
    vesselInReach_ = false;
    scene.Physics().SetHelm(nullptr);
    scene.VesselCameraRef().End();
    if (stopOnLevelChange_)
    {
        playing_ = false;
        possessed_ = false;
        scene.Physics().RemovePlayer();
        physics::GameSettings().player = settingsBefore_;
        input.SetMouseCapture(false);
        LOG_INFO(logging::LogCategory::App, "play: stopped, another level");
        return false;
    }
    PlaceAtStart(renderer, scene, start);
    return true;
}

void PlaySession::SetStatus(std::string status, double untilSeconds)
{
    status_ = std::move(status);
    statusUntil_ = untilSeconds;
}

std::wstring PlaySession::HudLine(const Scene& scene, double nowSeconds) const
{
    if (!playing_) { return {}; }
    wchar_t line[256];
    const physics::PlayerState state = scene.Physics().GetPlayerState();
    if (atHelm_)
    {
        const physics::VesselState& vessel = scene.Physics().GetHelm().vessel;
        std::swprintf(line, sizeof(line) / sizeof(line[0]), L"PLAY: at the helm, %.1f kn, throttle %+.0f %%, rudder %+.0f %%   W S throttle   A D rudder   V view   E leave", vessel.speed * 1.94384f, vessel.throttle * 100.0f, vessel.rudder * 100.0f);
    }
    else if (possessed_ && state.exists)
    {
        std::swprintf(line, sizeof(line) / sizeof(line[0]), L"PLAY: %hs%ls, %.1f m/s", physics::PlayerModeName(state.mode), state.crouched ? L", crouched" : L"", state.speed);
    }
    else
    {
        std::swprintf(line, sizeof(line) / sizeof(line[0]), L"PLAY: the camera is free");
    }
    std::wstring text = line;
    if (vesselInReach_) { text += L"   E TAKE THE HELM"; }
#if WITH_EDITOR
    text += atHelm_ ? L"   Esc stop   K keep   Shift+F1 mouse" : L"   Esc stop   F8 eject   K keep   Shift+F1 mouse";
#else
    text += atHelm_ ? L"   Esc mouse" : L"   Esc mouse   F8 eject";
#endif
    if (mouseFree_) { text += L"   CLICK FOR THE MOUSE"; }
    if (nowSeconds < statusUntil_ && !status_.empty())
    {
        text += L"   ";
        text.append(status_.begin(), status_.end());
    }
    return text;
}
