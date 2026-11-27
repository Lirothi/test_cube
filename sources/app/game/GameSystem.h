#pragma once
// Architecture plan R13.2 (docs/architecture_review_plan.md): a piece of the game the play session runs -- the race, the
// camera of the boat at the helm, the spectator's camera on a bot's boat. The session, the app and the scene's hand-over
// call the list of them (game::PlayWorld::Systems) where they called the race by name: a new mechanic is one system more,
// not a call more in AppController, PlaySession and Scene::Sync each.
#include <string>

class DebugDrawSystem;
class Renderer;
class RuntimeObjects;
class Scene;

namespace game
{
struct RaceCourse;

// What a system sees of the session on a frame of Play.
struct SessionFrame
{
    Scene& scene;
    bool atHelm = false;    // the player is at a helm
    bool possessed = false; // the player has their body (not ejected)
};

class GameSystem
{
public:
    virtual const char* Name() const = 0;
    // Play starts, or the session goes on in a new level: the level's game data as it stands (its race course).
    virtual void OnSessionBegin(Scene& /*scene*/, const RaceCourse& /*course*/) {}
    // Stop (the player already off any helm, still in the world).
    virtual void OnSessionEnd(Scene& /*scene*/) {}
    // Every frame of a session, before the helm reads its keys (PlaySession::Tick).
    virtual void OnFrame(Scene& /*scene*/, float /*deltaTime*/) {}
    // The game's part of the hand-over (Scene::Sync, the physics poses synced, the render half parked): objects in and out
    // by the hand-over's road (R13.1), cameras from the poses this frame draws -- before the render half takes the camera.
    virtual void OnHandOver(Scene& /*scene*/, Renderer* /*renderer*/, RuntimeObjects& /*objects*/) {}
    // The level's objects went (Scene::Clear): nothing of them is held any more.
    virtual void OnLevelGone() {}
    // In Play, every frame: windows and labels (ImGui, the frame's), and the debug lines.
    virtual void OnDrawUi(const SessionFrame& /*frame*/) {}
    virtual void OnDebugDraw(DebugDrawSystem& /*draw*/) const {}
    // The HUD in Play: a line under the session's, the big centre text (a countdown). Empty = none.
    virtual std::wstring HudLine() const { return {}; }
    virtual std::wstring BigText() const { return {}; }
    // A window of its own is up that takes the mouse and the keys (a race's results).
    virtual bool WantsCursor() const { return false; }

protected:
    ~GameSystem() = default; // the world's (PlayWorld's members): never deleted through this type
};
} // namespace game
