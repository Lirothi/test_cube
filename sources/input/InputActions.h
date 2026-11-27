#pragma once

#include <string_view>

// The input actions the code reads, by the names input/bindings.json binds (architecture plan R12.1): a name is written
// once, here. InputManager::LoadActions checks the file against kAll: an action read here that the file does not bind is
// an Error (its key would silently do nothing), an action the file binds that nothing reads is a Warning (the controls
// overlay would show a key that does nothing).
namespace actions
{
// Movement and look: the free camera (CameraController), the player (PlayerController), the helm (PlaySession).
inline constexpr std::string_view kMoveX = "MoveX";
inline constexpr std::string_view kMoveY = "MoveY";
inline constexpr std::string_view kMoveZ = "MoveZ";
inline constexpr std::string_view kLookX = "LookX";
inline constexpr std::string_view kLookY = "LookY";
inline constexpr std::string_view kLookToggle = "LookToggle";
inline constexpr std::string_view kSprint = "Sprint";
inline constexpr std::string_view kJump = "Jump";
inline constexpr std::string_view kCrouch = "Crouch";

// Play mode (AppController, PlaySession).
inline constexpr std::string_view kPlay = "Play";
inline constexpr std::string_view kStopPlay = "StopPlay";
inline constexpr std::string_view kPossessEject = "PossessEject";
inline constexpr std::string_view kFreeMouse = "FreeMouse";
inline constexpr std::string_view kKeepSimulation = "KeepSimulation";
inline constexpr std::string_view kInteract = "Interact";
inline constexpr std::string_view kToggleView = "ToggleView";
inline constexpr std::string_view kAnchor = "Anchor";
inline constexpr std::string_view kAutopilot = "Autopilot";
inline constexpr std::string_view kWatchNext = "WatchNext";

// Developer keys (AppController, ProfilerHud); a shipped build reads none of them (docs/distribution_build_plan.md §7).
inline constexpr std::string_view kToggleDeveloperPanel = "ToggleDeveloperPanel";
inline constexpr std::string_view kToggleInstancing = "ToggleInstancing";
inline constexpr std::string_view kToggleLOD = "ToggleLOD";
inline constexpr std::string_view kToggleIndirectShadows = "ToggleIndirectShadows";
inline constexpr std::string_view kToggleGiIndirectShadows = "ToggleGiIndirectShadows";
inline constexpr std::string_view kToggleVsmPageRequest = "ToggleVsmPageRequest";
inline constexpr std::string_view kCycleReflectionSource = "CycleReflectionSource";
inline constexpr std::string_view kToggleRTDebugView = "ToggleRTDebugView";
inline constexpr std::string_view kToggleOceanControls = "ToggleOceanControls";
inline constexpr std::string_view kWireframe = "Wireframe";
inline constexpr std::string_view kToggleTextureInspector = "ToggleTextureInspector";
inline constexpr std::string_view kToggleProfiler = "ToggleProfiler";
inline constexpr std::string_view kCaptureTrace = "CaptureTrace";
inline constexpr std::string_view kSetDlssQualityOff = "SetDlssQualityOff";
inline constexpr std::string_view kSetDlssQualityMaxPerformance = "SetDlssQualityMaxPerformance";
inline constexpr std::string_view kSetDlssQualityBalanced = "SetDlssQualityBalanced";
inline constexpr std::string_view kSetDlssQualityMaxQuality = "SetDlssQualityMaxQuality";
inline constexpr std::string_view kSetDlssQualityUltraPerformance = "SetDlssQualityUltraPerformance";
inline constexpr std::string_view kSetDlssQualityUltraQuality = "SetDlssQualityUltraQuality";
inline constexpr std::string_view kSetDlssQualityDLAA = "SetDlssQualityDLAA";

// A script's alone (R12.3: --set player.testMoveX / testMoveZ, InputManager::InjectAxis): the player's move in the
// WORLD's axes, not turned with the camera as MoveX / MoveZ are. No key binds them, so they are not in kAll.
inline constexpr std::string_view kMoveWorldX = "MoveWorldX";
inline constexpr std::string_view kMoveWorldZ = "MoveWorldZ";
// The helm's throttle and rudder, and the player's sprint, as a script says them (--set player.testThrottle /
// testRudder / testSprint): added to W S / A D and Shift by their one reader (game::BoatHelm::Tick, PlayerController)
// -- not injected into MoveZ / MoveX / Sprint, which the free camera reads too.
inline constexpr std::string_view kHelmThrottle = "HelmThrottle";
inline constexpr std::string_view kHelmRudder = "HelmRudder";
inline constexpr std::string_view kScriptSprint = "ScriptSprint";

// Every action a key binds: what the bindings file is checked against.
inline constexpr std::string_view kAll[] = {
    kMoveX, kMoveY, kMoveZ, kLookX, kLookY, kLookToggle, kSprint, kJump, kCrouch,
    kPlay, kStopPlay, kPossessEject, kFreeMouse, kKeepSimulation, kInteract, kToggleView, kAnchor, kAutopilot, kWatchNext,
    kToggleDeveloperPanel, kToggleInstancing, kToggleLOD, kToggleIndirectShadows, kToggleGiIndirectShadows,
    kToggleVsmPageRequest, kCycleReflectionSource, kToggleRTDebugView, kToggleOceanControls, kWireframe,
    kToggleTextureInspector, kToggleProfiler, kCaptureTrace,
    kSetDlssQualityOff, kSetDlssQualityMaxPerformance, kSetDlssQualityBalanced, kSetDlssQualityMaxQuality,
    kSetDlssQualityUltraPerformance, kSetDlssQualityUltraQuality, kSetDlssQualityDLAA,
};
}
