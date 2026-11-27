#pragma once
// Architecture plan R6.2a (docs/architecture_review_plan.md, section 11.2, wave 3): the game's part of the world, out of
// Scene -- the race (its gates, its bots, its own boats), the camera of the boat at the helm and the spectator's on a
// bot's boat. The play session owns it (PlaySession::World, alive for the whole run); the scene calls its part of the
// hand-over (HandOver) where Scene::Sync made the race's boats before, and tells it when the level's objects go (Forget).
// Code that has no session at hand -- the settings rows (race.*, frame.vesselView), the readouts -- finds it through
// ActivePlayWorld(), null in a tool that never made a session.
//
// R13.2: what the game does with them goes through its systems (GameSystem.h): the race, the helm's camera, the
// spectator's -- the hand-over, the level going, the session's start and end, its frames, windows, lines and HUD call the
// list (Systems), in its order.
#include <array>
#include <optional>
#include <string>

#include "app/game/GameSystem.h"
#include "app/game/PlayerStart.h"
#include "app/game/Race.h"
#include "app/game/RaceCourse.h"
#include "app/game/VesselCamera.h"
#include "app/levels/LevelGameSink.h"

class RenderableObject;
class Renderer;
class RuntimeObjects; // R13.1
class Scene;

namespace game
{
// R13.2: the race as a system -- its own boats in and out at the hand-over, the bots' orders every frame, its windows,
// labels, lines and HUD. The race itself is PlayWorld::race.
class RaceSystem final : public GameSystem
{
public:
    explicit RaceSystem(Race& race) : race_(race) {}
    const char* Name() const override { return "race"; }
    void OnSessionBegin(Scene& scene, const RaceCourse& course) override;
    void OnSessionEnd(Scene& scene) override;
    void OnFrame(Scene& scene, float deltaTime) override;
    // The race's own boats made where its grid is or taken out after (Race::TakeSpawns / TakeDespawns).
    void OnHandOver(Scene& scene, Renderer* renderer, RuntimeObjects& objects) override;
    void OnLevelGone() override;
    void OnDrawUi(const SessionFrame& frame) override;
    void OnDebugDraw(DebugDrawSystem& draw) const override;
    std::wstring HudLine() const override;
    std::wstring BigText() const override;
    bool WantsCursor() const override;

private:
    Race& race_;
};

// R13.2: the camera of the boat at the helm (the session takes and leaves the helm), from the pose this frame draws
// the hull at.
class HelmCameraSystem final : public GameSystem
{
public:
    explicit HelmCameraSystem(VesselCamera& camera) : camera_(camera) {}
    const char* Name() const override { return "helm camera"; }
    void OnHandOver(Scene& scene, Renderer* renderer, RuntimeObjects& objects) override;
    // The hand-over gives the frame's camera to the helm's view (the spectator's waits).
    bool Applies() const;

private:
    VesselCamera& camera_;
};

// R13.2: race.watch -- the same chase camera on a bot's boat, the boat it is on, and the small window that picks them.
class SpectatorSystem final : public GameSystem
{
public:
    SpectatorSystem(VesselCamera& camera, RenderableObject*& spectated, Race& race, const HelmCameraSystem& helm)
        : camera_(camera), spectated_(spectated), race_(race), helm_(helm) {}
    const char* Name() const override { return "spectator"; }
    void OnHandOver(Scene& scene, Renderer* renderer, RuntimeObjects& objects) override;
    // On foot with the camera on a bot's boat: which boat (the racers in their places), the view (behind, cockpit,
    // cinema), the bots' vision (bot.debug). The mouse is free meanwhile.
    void OnDrawUi(const SessionFrame& frame) override;

private:
    VesselCamera& camera_;
    RenderableObject*& spectated_;
    Race& race_;
    const HelmCameraSystem& helm_;
};

struct PlayWorld final : LevelGameSink
{
    PlayWorld() = default;
    PlayWorld(const PlayWorld&) = delete; // its systems hold references into it
    PlayWorld& operator=(const PlayWorld&) = delete;

    // R6.2b: what the level the world holds gives Play -- the Player Start it loaded with (for a build without the editor;
    // with one, the document is its truth, it moves) and the race course as the level's file has it.
    std::optional<PlayerStart> levelPlayerStart;
    RaceCourse levelRaceCourse;

    Race race;
    VesselCamera vesselCamera;
    // race.watch: the same chase camera on a bot's boat, and the boat it is on now.
    VesselCamera spectatorCamera;
    RenderableObject* spectated = nullptr;

    // R13.2: the systems, in the order every call runs them: the race first (its boats, the bots' orders before the helm
    // reads its keys), then the cameras -- the helm's, else the spectator's.
    std::array<GameSystem*, 3> Systems() { return { &raceSystem_, &helmCameraSystem_, &spectatorSystem_ }; }
    std::array<const GameSystem*, 3> Systems() const { return { &raceSystem_, &helmCameraSystem_, &spectatorSystem_ }; }

    // The game's part of the hand-over (Scene::Sync, right after the physics poses are synced, the render half parked):
    // every system's (GameSystem::OnHandOver), objects by the hand-over's road (R13.1, `objects`).
    void HandOver(Scene& scene, Renderer* renderer, RuntimeObjects& objects);
    // The level's objects went (Scene::Clear): the race's boats were among them (GameSystem::OnLevelGone).
    void Forget();

    // LevelGameSink (R6.2b): the loader hands every object of the level's file; the race marks and the Player Start are read here.
    void BeginLevel(const std::string& path) override;
    void LevelObject(const nlohmann::json& object, bool enabled) override;
    void EndLevel() override;

private:
    RaceSystem raceSystem_{ race };
    HelmCameraSystem helmCameraSystem_{ vesselCamera };
    SpectatorSystem spectatorSystem_{ spectatorCamera, spectated, race, helmCameraSystem_ };
    RaceCourse loadingCourse_; // the course the level being loaded builds, the level's once it was read to its end
    std::string loadingPath_;
};

// The session's world while a PlaySession exists (it registers it), else null.
PlayWorld* ActivePlayWorld();
void SetActivePlayWorld(PlayWorld* world);
} // namespace game
