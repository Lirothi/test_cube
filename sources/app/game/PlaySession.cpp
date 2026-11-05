#include "app/game/PlaySession.h"

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

void PlaySession::TogglePossess(Scene& scene)
{
    if (!playing_) { return; }
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
    if (possessed_ && state.exists)
    {
        std::swprintf(line, sizeof(line) / sizeof(line[0]), L"PLAY: %hs%ls, %.1f m/s", physics::PlayerModeName(state.mode), state.crouched ? L", crouched" : L"", state.speed);
    }
    else
    {
        std::swprintf(line, sizeof(line) / sizeof(line[0]), L"PLAY: the camera is free");
    }
    std::wstring text = line;
#if WITH_EDITOR
    text += L"   Esc stop   F8 eject   K keep   Shift+F1 mouse";
#else
    text += L"   Esc mouse   F8 eject";
#endif
    if (mouseFree_) { text += L"   CLICK FOR THE MOUSE"; }
    if (nowSeconds < statusUntil_ && !status_.empty())
    {
        text += L"   ";
        text.append(status_.begin(), status_.end());
    }
    return text;
}
