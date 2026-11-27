#include "app/game/PlaySession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

#include <DirectXMath.h>
#include <utility>

#include "imgui.h"

#include "rendering/view/Camera.h"
#include "app/game/Race.h"
#include "app/game/RaceBot.h"
#include "app/game/VesselCamera.h"
#include "app/game/VesselStats.h"
#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "input/InputManager.h"
#include "input/InputActions.h"
#include "rendering/core/Renderer.h"

namespace
{
// R13.2: the session's clock for its status lines -- a steady clock's seconds, what ImGui::GetTime() was read for (the
// wall's time, not the world's: frame.timeScale does not shorten a status).
double SessionClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void PlaySession::AttachTo(Scene& scene)
{
    // R6.2a: what Scene::Sync did with the race's boats and the boat cameras, and what its Clear dropped, is the game's.
    scene.SetGameHooks([this, &scene](Renderer* renderer, RuntimeObjects& objects) { world_.HandOver(scene, renderer, objects); },
                       [this]() { world_.Forget(); });
}

void PlaySession::PlaceAtStart(Renderer& renderer, Scene& scene, const game::PlayerStart* start)
{
    Camera& camera = scene.CameraRef();
    physics::PlayerSettings& settings = physics::GameSettings().player;
    settings = settingsBefore_;
    game::ScriptCamera() = scriptCameraBefore_; // as the player's numbers (its test* fields until R12.3)
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
    scriptCameraBefore_ = game::ScriptCamera();
    playing_ = true;
    mouseFree_ = !takeMouse;
    mouseTaken_ = false;
    stopOnLevelChange_ = stopOnLevelChange;
    status_.clear();
    if (input.IsMouseCaptured()) { input.SetMouseCapture(false); }
    PlaceAtStart(renderer, scene, start);
    scene.OpenSessionJournal(); // R13.3: what the session makes is written down, and taken out after Stop if left
    const game::RaceCourse none{};
    for (game::GameSystem* system : world_.Systems()) { system->OnSessionBegin(scene, course ? *course : none); } // R13.2
}

void PlaySession::End(Renderer& renderer, Scene& scene, InputManager& input)
{
    if (!playing_) { return; }
    playing_ = false;
    possessed_ = false;
    LeaveHelm(scene, false);
    for (game::GameSystem* system : world_.Systems()) { system->OnSessionEnd(scene); } // R13.2: the race's pilots off, its boats to go
    scene.CloseSessionJournal(); // R13.3: what the session made and left goes at the next hand-over, after the systems' own
    scene.Physics().RemovePlayer();
    physics::GameSettings().player = settingsBefore_;
    game::ScriptCamera() = scriptCameraBefore_;
    input.ClearInjected(); // a script's keys end with the session, as the player's test* fields did until R12.3
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
    world_.vesselCamera.Begin();
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
    world_.vesselCamera.End();
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
        world_.vesselCamera.End();
        return;
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
        if (!race.HasTrack()) { SetStatus("No race course in this level", 3.0); }
        else { SetStatus(race.ToggleAutopilot() ? "Autopilot on" : "Autopilot off", 3.0); }
    }
    if (race.Autopilot()) { order = race.HelmOrder(); }
    if (race.HoldsHelm()) { order = physics::VesselInput{}; } // on the grid, the countdown running
    world.SetHelmInput(order);
    game::VesselCamera& view = world_.vesselCamera;
    view.Tick(camera.GetYaw() - yawBefore, camera.GetPitch() - pitchBefore, helm.vessel.speed, game::ScriptCamera().orbit,
              game::ScriptCamera().orbitPitch, deltaTime);
    if (input.TakeActionPress(actions::kToggleView))
    {
        // V: behind -> the cockpit -> cinema -> behind.
        view.NextView();
        camera.ResetHistory();
        SetStatus(std::string("Camera: ") + game::VesselCamera::ViewName(view.GetView()) + " (V: the next)", 3.0);
    }
    // The anchor: down under the bow, or up. It holds the boat when nobody is at the helm too; Stop weighs it.
    if (input.TakeActionPress(actions::kAnchor))
    {
        const bool down = world.ToggleAnchor(helm.object);
        SetStatus(down ? "Anchor down" : "Anchor up", 3.0);
    }
    if (input.TakeActionPress(actions::kInteract)) { LeaveHelm(scene, true); }
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
    // On foot with the camera on a bot's boat the mouse stays free: it is the spectator panel's (game::SpectatorSystem::OnDrawUi).
    const bool watching = !atHelm_ && world_.race.WatchedVessel() != nullptr;
    const bool take = controls && !watching && input.WindowIsForeground(); // the mouse is the desktop's: a window in the background keeps the keys and leaves it alone
    if (take != input.IsMouseCaptured()) { input.SetMouseCapture(take); }
    mouseTaken_ = take;
    // The keys are this session's only with `controls`; a script's injected input counts either way (R12.3).
    InputManager::KeysScope keys(input, controls);

    Camera& camera = scene.CameraRef();
    physics::World& world = scene.Physics();
    // R13.2: the systems' frame (the race: the bots' orders, and the helm's on autopilot -- before the helm reads its keys;
    // the boats put on the grid are a cut).
    for (game::GameSystem* system : world_.Systems()) { system->OnFrame(scene, deltaTime); }
    if (atHelm_)
    {
        TickHelm(input, scene, controls, deltaTime);
        return;
    }
    // On foot, Tab hangs the chase camera on the next racer (the next bot boat outside a race), past the last one gives it back.
    if (input.TakeActionPress(actions::kWatchNext))
    {
        const std::string on = world_.race.WatchNext();
        SetStatus(on.empty() ? std::string("Camera: yours") : "Camera: " + on + " (Tab: the next)", 3.0);
    }
    // And V its view: behind -> the cockpit -> cinema.
    if (watching && input.TakeActionPress(actions::kToggleView))
    {
        game::VesselCamera& view = world_.spectatorCamera;
        view.NextView();
        camera.ResetHistory();
        SetStatus(std::string("Camera: ") + game::VesselCamera::ViewName(view.GetView()) + " (V: the next)", 3.0);
    }
    // The way aboard: within a few metres of a hull, the key takes the helm.
    constexpr float kHelmReach = 3.0f;
    const physics::PlayerState onFoot = world.GetPlayerState();
    RenderableObject* vessel = possessed_ && onFoot.exists ? world.FindVessel(onFoot.feet, kHelmReach) : nullptr;
    vesselInReach_ = vessel != nullptr;
    const bool interact = input.TakeActionPress(actions::kInteract); // taken on foot, a hull in reach or not
    if (vessel && interact)
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
    // Watching a bot the player stands: the mouse moving over the panel does not turn their head meanwhile.
    if (hasBody)
    {
        InputManager::KeysScope playerKeys(input, controls && !watching);
        controller_.Tick(input, camera, scene.CameraControllerRef(), world, controls && !watching, deltaTime);
    }
    else
    {
        world.SetPlayerInput(physics::PlayerInput{});
        if (controls && !watching) { scene.CameraControllerRef().UpdateFromInput(input, deltaTime, true); }
    }
}

bool PlaySession::OnLevelLoaded(Renderer& renderer, Scene& scene, InputManager& input, const game::PlayerStart* start, const game::RaceCourse* course)
{
    if (!playing_) { return true; }
    atHelm_ = false; // the boat went with the level
    vesselInReach_ = false;
    scene.Physics().SetHelm(nullptr);
    world_.vesselCamera.End();
    if (stopOnLevelChange_)
    {
        StopForLevel(scene, input);
        return false;
    }
    PlaceAtStart(renderer, scene, start);
    input.ClearInjected(); // the player starts again: a script's keys are let go, as PlaceAtStart reset its test* fields
    const game::RaceCourse none{};
    for (game::GameSystem* system : world_.Systems()) { system->OnSessionBegin(scene, course ? *course : none); } // R13.2
    return true;
}

void PlaySession::StopForLevel(Scene& scene, InputManager& input)
{
    if (!playing_) { return; }
    atHelm_ = false; // the boat went with the level
    vesselInReach_ = false;
    scene.Physics().SetHelm(nullptr);
    world_.vesselCamera.End();
    playing_ = false;
    possessed_ = false;
    scene.CloseSessionJournal(); // R13.3: the level took what the session made with it; nothing is left to write down
    scene.Physics().RemovePlayer();
    physics::GameSettings().player = settingsBefore_;
    game::ScriptCamera() = scriptCameraBefore_;
    input.ClearInjected();
    input.SetMouseCapture(false);
    LOG_INFO(logging::LogCategory::App, "play: stopped, another level");
}

void PlaySession::SetStatus(std::string status, double forSeconds)
{
    status_ = std::move(status);
    statusUntil_ = SessionClock() + forSeconds;
}

std::wstring PlaySession::SystemsHudLine() const
{
    if (!playing_) { return {}; }
    std::wstring text;
    for (const game::GameSystem* system : world_.Systems())
    {
        const std::wstring line = system->HudLine();
        if (line.empty()) { continue; }
        if (!text.empty()) { text += L"   "; }
        text += line;
    }
    return text;
}

std::wstring PlaySession::SystemsBigText() const
{
    if (!playing_) { return {}; }
    for (const game::GameSystem* system : world_.Systems())
    {
        std::wstring text = system->BigText();
        if (!text.empty()) { return text; }
    }
    return {};
}

bool PlaySession::SystemsWantCursor() const
{
    if (!playing_) { return false; }
    for (const game::GameSystem* system : world_.Systems())
    {
        if (system->WantsCursor()) { return true; }
    }
    return false;
}

void PlaySession::DrawUi(Scene& scene)
{
    if (!playing_) { return; }
    const game::SessionFrame frame{ scene, atHelm_, possessed_ };
    for (game::GameSystem* system : world_.Systems()) { system->OnDrawUi(frame); } // the race's windows and labels, the spectator's
    DrawBoatCard(scene); // on foot beside a boat: what it does
}

void PlaySession::DebugDraw(DebugDrawSystem& draw) const
{
    if (!playing_) { return; }
    for (const game::GameSystem* system : world_.Systems()) { system->OnDebugDraw(draw); } // bot.debug
}

void PlaySession::DrawBoatCard(Scene& scene)
{
    if (!playing_ || atHelm_ || !possessed_ || world_.race.WatchedVessel()) { return; }
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
        std::snprintf(value, sizeof(value), "a %.0f m circle flat out", 2.0f * stats.turnRadius);
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

std::wstring PlaySession::HudLine(const Scene& scene) const
{
    if (!playing_) { return {}; }
    const double nowSeconds = SessionClock(); // R13.2: the session's clock (the caller's ImGui::GetTime() before)
    wchar_t line[256];
    const physics::PlayerState state = scene.Physics().GetPlayerState();
    if (atHelm_)
    {
        const physics::HelmState& helm = scene.Physics().GetHelm();
        const physics::VesselState& vessel = helm.vessel;
        std::swprintf(line, sizeof(line) / sizeof(line[0]), L"PLAY: at the helm, %.1f kn, throttle %+.0f %%, rudder %+.0f %%%ls   W S throttle   A D rudder   V view: %hs   %ls   E leave",
                      vessel.speed * 1.94384f, vessel.throttle * 100.0f, vessel.rudder * 100.0f, helm.anchored ? L", ANCHORED" : L"",
                      game::VesselCamera::ViewName(world_.vesselCamera.GetView()), helm.anchored ? L"X weigh anchor" : L"X anchor");
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
    // Every build without the editor has the game's menu on Esc (docs/scalability_plan.md §6.1).
    text += atHelm_ ? L"   Esc menu" : L"   Esc menu   F8 eject";
#endif
    if (mouseFree_) { text += L"   CLICK FOR THE MOUSE"; }
    if (nowSeconds < statusUntil_ && !status_.empty())
    {
        text += L"   ";
        text.append(status_.begin(), status_.end());
    }
    return text;
}
