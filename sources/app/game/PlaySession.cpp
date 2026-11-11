#include "app/game/PlaySession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

#include <DirectXMath.h>
#include <utility>

#include "imgui.h"

#include "app/camera/Camera.h"
#include "app/game/VesselStats.h"
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

void PlaySession::Begin(Renderer& renderer, Scene& scene, InputManager& input, const game::PlayerStart* start, const game::RaceCourse* course, bool takeMouse,
                        bool stopOnLevelChange)
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
    scene.RaceRef().Begin(course ? *course : game::RaceCourse{});
}

void PlaySession::End(Renderer& renderer, Scene& scene, InputManager& input)
{
    if (!playing_) { return; }
    playing_ = false;
    possessed_ = false;
    LeaveHelm(scene, false);
    scene.RaceRef().End(scene.Physics());
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

void PlaySession::TickHelm(InputManager& input, Scene& scene, bool controls, float deltaTime, bool pressTaken)
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
    // The autopilot: B hands this helm to the race's bot (game::Race) and takes it back.
    game::Race& race = scene.RaceRef();
    const bool scriptAutopilot = std::exchange(tuning.testAutopilot, false);
    if (scriptAutopilot || (controls && input.WasActionPressed("Autopilot")))
    {
        if (!race.HasTrack()) { SetStatus("No race course in this level", ImGui::GetTime() + 3.0); }
        else { SetStatus(race.ToggleAutopilot() ? "Autopilot on" : "Autopilot off", ImGui::GetTime() + 3.0); }
    }
    if (race.Autopilot()) { order = race.HelmOrder(); }
    if (race.HoldsHelm()) { order = physics::VesselInput{}; } // on the grid, the countdown running
    world.SetHelmInput(order);
    game::VesselCamera& view = scene.VesselCameraRef();
    view.Tick(camera.GetYaw() - yawBefore, camera.GetPitch() - pitchBefore, helm.vessel.speed, tuning.testOrbit,
              tuning.testOrbitPitch, deltaTime);
    const bool scriptView = std::exchange(tuning.testView, false);
    if (scriptView || (controls && input.WasActionPressed("ToggleView")))
    {
        view.ToggleView();
        camera.ResetHistory();
    }
    // The anchor: down under the bow, or up. It holds the boat when nobody is at the helm too; Stop weighs it.
    const bool scriptAnchor = std::exchange(tuning.testAnchor, false);
    if (scriptAnchor || (controls && input.WasActionPressed("Anchor")))
    {
        const bool down = world.ToggleAnchor(helm.object);
        SetStatus(down ? "Anchor down" : "Anchor up", ImGui::GetTime() + 3.0);
    }
    const bool scriptLeave = std::exchange(tuning.testInteract, false);
    if (scriptLeave || (controls && !pressTaken && input.WasActionPressed("Interact"))) { LeaveHelm(scene, true); }
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
    scene.RaceRef().Tick(world, deltaTime); // the bots' orders, and the helm's on autopilot: before the helm reads its keys
    if (scene.RaceRef().TakeCameraCut()) { camera.ResetHistory(); } // the boats were put on the grid: a jump, not a pan
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
            TickHelm(input, scene, controls, deltaTime, true);
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

bool PlaySession::OnLevelLoaded(Renderer& renderer, Scene& scene, InputManager& input, const game::PlayerStart* start, const game::RaceCourse* course)
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
    scene.RaceRef().Begin(course ? *course : game::RaceCourse{});
    return true;
}

void PlaySession::SetStatus(std::string status, double untilSeconds)
{
    status_ = std::move(status);
    statusUntil_ = untilSeconds;
}

std::wstring PlaySession::RaceHudLine(const Scene& scene) const { return playing_ ? scene.RaceRef().HudLine() : std::wstring{}; }

void PlaySession::DrawBoatCard(Scene& scene)
{
    if (!playing_ || atHelm_ || !possessed_) { return; }
    physics::World& world = scene.Physics();
    const physics::PlayerState onFoot = world.GetPlayerState();
    if (!onFoot.exists) { return; }
    constexpr float kCardReach = 16.0f; // m from the hull: nearer than this its card is up
    std::vector<physics::VesselHull> hulls;
    world.CollectHulls(hulls);
    RenderableObject* nearest = world.FindVessel(onFoot.feet, 3.0f); // the one E would take
    int card = 0;
    for (const physics::VesselHull& one : hulls)
    {
        const physics::VesselHull* hull = &one;
        RenderableObject* vessel = one.object;
        if (!hull->settings || !vessel) { continue; }
        // To the rectangle of its hull on the water (World::FindVessel's distance): a boat is long.
        const float dx = onFoot.feet.x - hull->position.x;
        const float dz = onFoot.feet.z - hull->position.z;
        const float pastEnds = std::max(std::fabs(dx * hull->forward.x + dz * hull->forward.z) - hull->halfLength, 0.0f);
        const float pastSides = std::max(std::fabs(dx * hull->forward.z - dz * hull->forward.x) - hull->halfBeam, 0.0f);
        if (pastEnds * pastEnds + pastSides * pastSides > kCardReach * kCardReach) { continue; }

    // Over the boat: a point above its middle, through the frame's camera. One out of the frame has no card.
    const Camera& camera = scene.CameraRef();
    const Math::float3 over(hull->position.x, hull->position.y + 0.3f * hull->halfLength + 1.8f, hull->position.z);
    DirectX::XMFLOAT4 clip{};
    DirectX::XMStoreFloat4(&clip, DirectX::XMVector4Transform(DirectX::XMVectorSet(over.x, over.y, over.z, 1.0f), camera.GetViewProjMatrixNoJitter().xm()));
    if (clip.w <= 0.05f) { continue; } // behind the eye
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 at((clip.x / clip.w * 0.5f + 0.5f) * io.DisplaySize.x, (0.5f - clip.y / clip.w * 0.5f) * io.DisplaySize.y);
    if (at.x < 0.0f || at.x > io.DisplaySize.x) { continue; }

    // "models/fast_boat_13/fast_boat_13.mesh.json" -> "Fast Boat 13"
    std::string name = vessel->GetGeometryPath();
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) { name = name.substr(slash + 1); }
    const std::size_t dot = name.find('.');
    if (dot != std::string::npos) { name = name.substr(0, dot); }
    bool start = true;
    for (char& c : name)
    {
        if (c == '_') { c = ' '; start = true; continue; }
        if (start && c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
        start = false;
    }

    const game::VesselStats stats = game::StatsOf(*hull->settings, hull->halfLength, hull->rudderLever);
    // Its bottom edge on that point, and the whole of it under the HUD's lines: a boat at arm's reach fills the frame.
    const float cardHeight = ImGui::GetFrameHeightWithSpacing() * 4.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f + 40.0f;
    ImGui::SetNextWindowPos(ImVec2(at.x, std::max(at.y, cardHeight + 110.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.78f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    char window[32];
    std::snprintf(window, sizeof(window), "##boat card %d", card++);
    if (ImGui::Begin(window, nullptr, flags))
    {
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f m, %.1f t, %.0f kN", stats.length, stats.mass * 0.001f, stats.thrust * 0.001f);
        ImGui::Separator();
        // A bar each, on one scale for every boat: what is longer is better.
        const float labels = ImGui::CalcTextSize("Acceleration").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        const float bar = ImGui::CalcTextSize("Acceleration").x * 1.6f;
        const auto row = [labels, bar](const char* label, float share, const char* value)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(labels);
            ImGui::ProgressBar(std::clamp(share, 0.02f, 1.0f), ImVec2(bar, 0.0f), "");
            ImGui::SameLine();
            ImGui::TextUnformatted(value);
        };
        char value[64];
        std::snprintf(value, sizeof(value), "%.0f kn", stats.topSpeed * 1.94384f);
        row("Top speed", game::TopSpeedBar(stats.topSpeed), value);
        std::snprintf(value, sizeof(value), "20 kn in %.1f s", stats.toPlane);
        row("Acceleration", game::AccelerationBar(stats.toPlane), value);
        std::snprintf(value, sizeof(value), "a %.0f m circle", 2.0f * stats.turnRadius);
        row("Handling", game::HandlingBar(2.0f * stats.turnRadius), value);
        // In a sea: how deep it sits and how heavy it is, half each. A hull is out of the water -- no keel, no rudder --
        // when a wave has dropped away by about its draft (wind_test's open leg: 0.71 m is out 1-11 % of the time, 0.38-0.39 m
        // 17-22 %), and a wave throws a light hull further (its roll there: 1.9 t 15 degrees, past 15 a third of the time;
        // 5.5 and 7.5 t 9 degrees, 6-8 %). By the draft alone the light boat's bar was the yacht's.
        const float draft = stats.draft * hull->scale.y;
        std::snprintf(value, sizeof(value), "%.1f t, %.2f m deep", stats.mass * 0.001f, draft);
        row("Sea keeping", game::SeaKeeping(stats.mass, draft), value);
        ImGui::Separator();
        if (vessel == nearest) { ImGui::TextUnformatted("E   take the helm"); }
        else { ImGui::TextDisabled("Come alongside for the helm (E)"); }
    }
    ImGui::End();
    }
}

std::wstring PlaySession::HudLine(const Scene& scene, double nowSeconds) const
{
    if (!playing_) { return {}; }
    wchar_t line[256];
    const physics::PlayerState state = scene.Physics().GetPlayerState();
    if (atHelm_)
    {
        const physics::HelmState& helm = scene.Physics().GetHelm();
        const physics::VesselState& vessel = helm.vessel;
        std::swprintf(line, sizeof(line) / sizeof(line[0]), L"PLAY: at the helm, %.1f kn, throttle %+.0f %%, rudder %+.0f %%%ls   W S throttle   A D rudder   V view   %ls   E leave",
                      vessel.speed * 1.94384f, vessel.throttle * 100.0f, vessel.rudder * 100.0f, helm.anchored ? L", ANCHORED" : L"",
                      helm.anchored ? L"X weigh anchor" : L"X anchor");
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
    // K is UE's Keep Simulation Changes: it WRITES the poses into the level (an undo entry). "K keep" beside the boat's keys
    // read as an anchor for this session (owner, 2026-11-09), and the yacht stayed where it was kept after Stop.
    text += atHelm_ ? L"   Esc stop   K write the poses to the level   Shift+F1 mouse"
                    : L"   Esc stop   F8 eject   K write the poses to the level   Shift+F1 mouse";
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
