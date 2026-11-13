#pragma once

#include <string>

#include "app/game/PlayerController.h"
#include "app/game/PlayerStart.h"
#include "app/game/RaceCourse.h"

class InputManager;
class Renderer;
class Scene;

// The play session: UE's Play In Editor, without the copy of the world, because nothing a session changes is the level's -- a body's pose is a render offset (AGENTS.md, Physics) and
// the camera is put back here. While it runs: the world simulates (AppController says so every frame), the camera is the player's or, after F8, a free one that keeps the mouse.
// A build without the editor has no Stop. docs/play_mode_plan.md.
class PlaySession
{
public:
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
    bool AtHelm() const { return playing_ && atHelm_; }
    // After a level load: the session goes on from the new level's start, or ends if it was the editor's (returns false, nothing restored: the camera is the new level's).
    bool OnLevelLoaded(Renderer& renderer, Scene& scene, InputManager& input, const game::PlayerStart* start, const game::RaceCourse* course);
    // After a level load, the session ends where the new level put the camera (nothing put back: the old level's pose
    // means nothing here) -- the editor's session, or Shipping's level rule on a level without a Player Start.
    void StopForLevel(Scene& scene, InputManager& input);
    void SetStatus(std::string status, double untilSeconds);
    // The HUD's line.
    std::wstring HudLine(const Scene& scene, double nowSeconds) const;
    // And the race's under it: the player's laps at a helm, else the bot's. Empty without a course.
    std::wstring RaceHudLine(const Scene& scene) const;
    // A boat's card (ImGui, the frame's): on foot within a few metres of a boat, what it does -- its top speed, how fast
    // it gets there, the circle it turns in, its weight and engine -- drawn over it. Takes no mouse and no key.
    void DrawBoatCard(Scene& scene);
    // On foot with the camera on a bot's boat (race.watch): a small window -- which boat (the racers in their places), the
    // view (behind, cockpit, cinema), the bots' vision (bot.debug). The mouse is free meanwhile.
    void DrawSpectatorPanel(Scene& scene);

private:
    void PlaceAtStart(Renderer& renderer, Scene& scene, const game::PlayerStart* start);
    // A boat (docs/play_mode_plan.md): E within reach of its hull takes the helm, E leaves it over the side.
    void TakeHelm(Scene& scene, RenderableObject* vessel);
    void LeaveHelm(Scene& scene, bool overTheSide);
    // `pressTaken`: this frame's E already took the helm. A press is read, not consumed, so the same frame's TickHelm saw it again and left over the side.
    void TickHelm(InputManager& input, Scene& scene, bool controls, float deltaTime, bool pressTaken = false);

    PlayerController controller_;
    physics::PlayerSettings settingsBefore_;
    Math::float3 cameraPosition_;
    float cameraYaw_ = 0.0f;
    float cameraPitch_ = 0.0f;
    float cameraRoll_ = 0.0f;
    std::string status_;
    double statusUntil_ = 0.0;
    bool playing_ = false;
    bool possessed_ = false;
    bool mouseFree_ = false;
    bool mouseTaken_ = false;
    bool justSpawned_ = false; // the body comes in this frame's Tick: till then "no body" does not mean no world
    bool stopOnLevelChange_ = true;
    bool atHelm_ = false;
    bool vesselInReach_ = false; // for the HUD's line
};
