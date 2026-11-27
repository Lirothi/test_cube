#pragma once

#include <array>
#include <string>

#include "app/game/BoatHelm.h" // R13.5
#include "app/game/Control.h"
#include "app/game/PlayerController.h"
#include "app/game/PlayerStart.h"
#include "app/game/PlayWorld.h" // R6.2a
#include "app/game/RaceCourse.h"
#include "app/game/VesselCamera.h" // ScriptCameraHold

class DebugDrawSystem;
class InputManager;
class Renderer;
class Scene;

// The play session: UE's Play In Editor, without the copy of the world, because nothing a session changes is the level's -- a body's pose is a render offset (AGENTS.md, Physics) and
// the camera is put back here. While it runs: the world simulates (AppController says so every frame), the camera is the player's or, after F8, a free one that keeps the mouse.
// A build without the editor has no Stop. docs/play_mode_plan.md.
class PlaySession
{
public:
    // R6.2a: the session owns the game's part of the world (game::PlayWorld: the race, the helm's and the spectator's
    // cameras) for the whole run, and registers it for code that has no session at hand (game::ActivePlayWorld).
    PlaySession() { game::SetActivePlayWorld(&world_); }
    ~PlaySession() { if (game::ActivePlayWorld() == &world_) { game::SetActivePlayWorld(nullptr); } }
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;
    game::PlayWorld& World() { return world_; }
    const game::PlayWorld& World() const { return world_; }
    // The scene calls the game's part of its hand-over and tells it when its objects go (Scene::SetGameHooks).
    void AttachTo(Scene& scene);

    bool Playing() const { return playing_; }
    bool Possessed() const { return playing_ && possessed_; }
    // `start` null = from where the camera stands. `takeMouse` false: a script's Play (--play, MCP) leaves the owner's mouse alone until a click in the window.
    // `course`: the level's race as it stands now (its gates, the boats a bot helms); null = none.
    void Begin(Renderer& renderer, Scene& scene, InputManager& input, const game::PlayerStart* start, const game::RaceCourse* course, bool takeMouse,
               bool stopOnLevelChange);
    void End(Renderer& renderer, Scene& scene, InputManager& input);
    // F8: UE's Possess / Eject. The body stands and lives; back in it the camera is where the body is.
    void TogglePossess(Scene& scene);
    // Shift+F1: the mouse goes free (a click in the window takes it back).
    void ToggleFreeMouse() { mouseFree_ = !mouseFree_; }
    // Shipping's Esc menu: the mouse is the menu's while it is up, the game's again when it closes.
    void SetMouseFree(bool free) { mouseFree_ = free; }
    // Once a frame, before Scene::Tick: the mouse and the keys. `uiWantsCursor`: a window is open that wants the mouse.
    void Tick(InputManager& input, Scene& scene, float deltaTime, bool uiWantsCursor, bool uiCapturingMouse);
    // R13.5: the player controls something (a boat's helm): their body is out of the world meanwhile.
    bool Controlling() const { return playing_ && controlled_ != nullptr; }
    // After a level load: the session goes on from the new level's start, or ends if it was the editor's (returns false, nothing restored: the camera is the new level's).
    bool OnLevelLoaded(Renderer& renderer, Scene& scene, InputManager& input, const game::PlayerStart* start, const game::RaceCourse* course);
    // After a level load, the session ends where the new level put the camera (nothing put back: the old level's pose
    // means nothing here) -- the editor's session, or Shipping's level rule on a level without a Player Start.
    void StopForLevel(Scene& scene, InputManager& input);
    // A status after the HUD's line for `forSeconds` (R13.2: on the session's own clock, a steady one -- ImGui's before).
    void SetStatus(std::string status, double forSeconds);
    // The HUD's line.
    std::wstring HudLine(const Scene& scene) const;
    // R13.2: the game's systems (game::PlayWorld::Systems), in Play -- their lines under the HUD's (the race's: the player's
    // laps at a helm, else the bot's), their big centre text (a countdown), whether a window of theirs takes the mouse
    // (a race's results), their windows and labels (then the boat card), their debug lines. Empty / false / nothing
    // outside Play.
    std::wstring SystemsHudLine() const;
    std::wstring SystemsBigText() const;
    bool SystemsWantCursor() const;
    void DrawUi(Scene& scene);
    void DebugDraw(DebugDrawSystem& draw) const;
    // A boat's card (ImGui, the frame's): on foot within a few metres of a boat, what it does -- its top speed, how fast
    // it gets there, the circle it turns in, its weight and engine -- drawn over it. Takes no mouse and no key.
    void DrawBoatCard(Scene& scene);

private:
    void PlaceAtStart(Renderer& renderer, Scene& scene, const game::PlayerStart* start);
    // R13.5 (game/Control.h): what within reach the interact key hands the player to control -- a boat's helm today
    // (docs/play_mode_plan.md: E within reach of its hull takes the helm, E leaves it over the side).
    std::array<game::Interactable*, 1> Interactables() { return { &boatHelm_ }; }
    void Take(Scene& scene, game::Controllable& controllable, RenderableObject* target);
    // Let go of what the player controls; `overTheSide`: their body comes back where it says.
    void LetGo(Scene& scene, bool overTheSide);
    // Its keys are taken (InputManager::TakeActionPress): the E that took the helm this frame is the on-foot listener's,
    // and the helm's Tick right after Take does not see it again as leave (R12.2; until 2026-11-09 it left over the side).
    void TickControlled(InputManager& input, Scene& scene, bool controls, float deltaTime);

    PlayerController controller_;
    physics::PlayerSettings settingsBefore_;
    game::ScriptCameraHold scriptCameraBefore_; // the script's camera (VesselCamera.h) before the session, as settingsBefore_
    game::PlayWorld world_; // R6.2a
    Math::float3 cameraPosition_;
    float cameraYaw_ = 0.0f;
    float cameraPitch_ = 0.0f;
    float cameraRoll_ = 0.0f;
    std::string status_;
    double statusUntil_ = 0.0; // on the session's clock (PlaySession.cpp: a steady clock's seconds)
    bool playing_ = false;
    bool possessed_ = false;
    bool mouseFree_ = false;
    bool mouseTaken_ = false;
    bool justSpawned_ = false; // the body comes in this frame's Tick: till then "no body" does not mean no world
    bool stopOnLevelChange_ = true;
    // R13.5: a boat's helm (game::BoatHelm), what the player controls now (null: on foot or the camera free), and the
    // prompt of what is within reach (for the HUD's line; null: nothing).
    game::BoatHelm boatHelm_{ world_, [this](std::string status, double forSeconds) { SetStatus(std::move(status), forSeconds); } };
    game::Controllable* controlled_ = nullptr;
    const wchar_t* prompt_ = nullptr;
};
