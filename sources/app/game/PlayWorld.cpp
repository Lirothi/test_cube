#include "app/game/PlayWorld.h"

// Architecture plan R6.2a: the bodies below were Scene's (app/scene/Scene.cpp: SyncRaceBoats and the helm / spectator
// camera of Scene::Sync), moved verbatim; what they used of the scene goes through its public face now.
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "core/logging/Log.h"
#include "ocean/OceanRenderable.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/renderables/RenderableObject.h"
#include "third_party/json/json.hpp"

namespace game
{
namespace
{
PlayWorld* g_activePlayWorld = nullptr;
} // namespace

PlayWorld* ActivePlayWorld() { return g_activePlayWorld; }
void SetActivePlayWorld(PlayWorld* world) { g_activePlayWorld = world; }

void PlayWorld::HandOver(Scene& scene, Renderer* renderer)
{
    SyncRaceBoats(scene, renderer);
    Camera& camera = scene.CameraRef();
    if (vesselCamera.Active() && !game::ScriptCamera().free)
    {
        // The camera of a boat at the helm, from the very pose this frame draws the hull at, and so before the render half takes the camera's.
        const OceanRenderable* ocean = scene.FindOceanRenderable();
        vesselCamera.Apply(camera, game::PoseOf(scene.Physics().GetHelm()), ocean ? ocean->GetWaterLevel() : -1.0e6f);
    }
    else if (RenderableObject* watched = !game::ScriptCamera().free ? race.WatchedVessel() : nullptr)
    {
        // race.watch: a bot's boat, from the pose this frame draws it at -- the helm's chase camera, on another hull. Another
        // boat is a cut.
        std::vector<physics::VesselHull> hulls;
        scene.Physics().CollectHulls(hulls);
        for (const physics::VesselHull& hull : hulls)
        {
            if (hull.object != watched || !hull.settings) { continue; }
            if (spectated != watched)
            {
                spectatorCamera.Begin();
                camera.ResetHistory();
                spectated = watched;
            }
            spectatorCamera.Tick(0.0f, 0.0f, hull.speed, 0.0f, 0.0f, race.LastDeltaTime());
            const OceanRenderable* ocean = scene.FindOceanRenderable();
            spectatorCamera.Apply(camera, game::PoseOf(hull), ocean ? ocean->GetWaterLevel() : -1.0e6f);
            break;
        }
    }
    else { spectated = nullptr; }
}

void PlayWorld::Forget()
{
    race.Forget();       // its boats are
    race.DropSpawned();  // and the race's own boats, made or asked for
}

void PlayWorld::SyncRaceBoats(Scene& scene, Renderer* renderer)
{
    std::vector<const RenderableObjectBase*> gone = race.TakeDespawns();
    std::vector<game::RaceSpawn> spawns = race.TakeSpawns();
    if (!renderer)
    {
        for (const game::RaceSpawn& spawn : spawns) { race.OnSpawned(spawn.ticket, nullptr); }
        return;
    }
    if (!gone.empty())
    {
        const std::size_t removed = scene.RemoveRuntimeObjects(*renderer, gone);
        LOG_INFO(logging::LogCategory::Scene, "race: {} of its boat(s) taken out of the world", removed);
    }
    if (spawns.empty()) { return; }
    UploadBatch uploads;
    if (!uploads.Begin(renderer))
    {
        for (const game::RaceSpawn& spawn : spawns) { race.OnSpawned(spawn.ticket, nullptr); }
        return;
    }
    SceneObjectRegistry registry = SceneObjectRegistry::CreateWithBuiltins();
    std::vector<std::pair<std::uint64_t, RenderableObject*>> made;
    for (const game::RaceSpawn& spawn : spawns)
    {
        // A level object as the editor would place it: the asset, where the grid has it. Its bow (the asset's forward, +x for
        // the boats here) toward the heading -- the race puts it on its place exactly once its hull is in the world.
        const float yawDeg = (spawn.headingRad - 1.5707963f) * 57.2957795f;
        const nlohmann::json object = {
            { "type", "staticMesh" },
            { "name", "race boat" },
            { "mesh", spawn.asset },
            { "position", { spawn.position.x, spawn.position.y, spawn.position.z } },
            { "rotationDeg", { 0.0f, yawDeg, 0.0f } },
            { "scale", { 1.0f, 1.0f, 1.0f } },
        };
        SceneObjectRegistry::CreationContext creation{ scene };
        SceneObjectRegistry::ObjectList objects = registry.Create("staticMesh", creation, object);
        RenderableObject* vessel = nullptr;
        for (std::unique_ptr<RenderableObjectBase>& one : objects)
        {
            if (!one) { continue; }
            RenderableObject* renderable = one->AsRenderableObject();
            if (scene.AddInitializedObject(*renderer, uploads, std::move(one)) && !vessel) { vessel = renderable; }
        }
        made.emplace_back(spawn.ticket, vessel);
    }
    uploads.SubmitAndWait(renderer);
    for (const auto& [ticket, vessel] : made)
    {
        race.OnSpawned(ticket, vessel);
        LOG_INFO(logging::LogCategory::Scene, "race: a boat {} for the grid", vessel ? "made" : "NOT made");
    }
}
} // namespace game
