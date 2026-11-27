#pragma once
// Architecture plan R6.2a (docs/architecture_review_plan.md, section 11.2, wave 3): the game's part of the world, out of
// Scene -- the race (its gates, its bots, its own boats), the camera of the boat at the helm and the spectator's on a
// bot's boat. The play session owns it (PlaySession::World, alive for the whole run); the scene calls its part of the
// hand-over (HandOver) where Scene::Sync made the race's boats before, and tells it when the level's objects go (Forget).
// Code that has no session at hand -- the settings rows (race.*, frame.vesselView), the readouts -- finds it through
// ActivePlayWorld(), null in a tool that never made a session.
#include <optional>
#include <string>

#include "app/game/PlayerStart.h"
#include "app/game/Race.h"
#include "app/game/RaceCourse.h"
#include "app/game/VesselCamera.h"
#include "app/levels/LevelGameSink.h"

class RenderableObject;
class Renderer;
class Scene;

namespace game
{
struct PlayWorld final : LevelGameSink
{
    // R6.2b: what the level the world holds gives Play -- the Player Start it loaded with (for a build without the editor;
    // with one, the document is its truth, it moves) and the race course as the level's file has it.
    std::optional<PlayerStart> levelPlayerStart;
    RaceCourse levelRaceCourse;

    Race race;
    VesselCamera vesselCamera;
    // race.watch: the same chase camera on a bot's boat, and the boat it is on now.
    VesselCamera spectatorCamera;
    RenderableObject* spectated = nullptr;

    // The game's part of the hand-over (Scene::Sync, right after the physics poses are synced, the render half parked):
    // the race's own boats made where its grid is or taken out after (game::Race::TakeSpawns / TakeDespawns), then the
    // camera of the boat at the helm or of the watched bot's boat, from the pose this frame draws the hull at -- before
    // the render half takes the camera's.
    void HandOver(Scene& scene, Renderer* renderer);
    // The level's objects went (Scene::Clear): the race's boats were among them.
    void Forget();

    // LevelGameSink (R6.2b): the loader hands every object of the level's file; the race marks and the Player Start are read here.
    void BeginLevel(const std::string& path) override;
    void LevelObject(const nlohmann::json& object, bool enabled) override;
    void EndLevel() override;

private:
    void SyncRaceBoats(Scene& scene, Renderer* renderer);
    RaceCourse loadingCourse_; // the course the level being loaded builds, the level's once it was read to its end
    std::string loadingPath_;
};

// The session's world while a PlaySession exists (it registers it), else null.
PlayWorld* ActivePlayWorld();
void SetActivePlayWorld(PlayWorld* world);
} // namespace game
