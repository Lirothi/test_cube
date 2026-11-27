#include "app/game/PlayWorld.h"

// Architecture plan R6.2a: the bodies below were Scene's (app/scene/Scene.cpp: SyncRaceBoats and the helm / spectator
// camera of Scene::Sync), moved verbatim; what they used of the scene goes through its public face now.
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "imgui.h" // R13.2: the spectator's window (PlaySession::DrawSpectatorPanel before)

#include "app/game/PlayerStartJson.h" // R6.2b: ReadPlayerKeys, kPlayerStartType
#include "app/game/RaceBot.h"         // R13.2: the spectator's window switches the bots' vision (BotSettingsRef)
#include "app/game/RaceCourseJson.h"  // ReadRaceKeys
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "core/logging/Log.h"
#include "ocean/OceanRenderable.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/RuntimeObjects.h" // R13.1
#include "third_party/json/json.hpp"

namespace game
{
namespace
{
PlayWorld* g_activePlayWorld = nullptr;
} // namespace

PlayWorld* ActivePlayWorld() { return g_activePlayWorld; }
void SetActivePlayWorld(PlayWorld* world) { g_activePlayWorld = world; }

void PlayWorld::HandOver(Scene& scene, Renderer* renderer, RuntimeObjects& objects)
{
    // R13.2: every system's part, in the list's order -- the race's boats first, then the helm's camera, else the
    // spectator's (R6.2a's bodies, moved verbatim into them).
    for (GameSystem* system : Systems()) { system->OnHandOver(scene, renderer, objects); }
}

bool HelmCameraSystem::Applies() const { return camera_.Active() && !game::ScriptCamera().free; }

void HelmCameraSystem::OnHandOver(Scene& scene, Renderer* /*renderer*/, RuntimeObjects& /*objects*/)
{
    if (!Applies()) { return; }
    // The camera of a boat at the helm, from the very pose this frame draws the hull at, and so before the render half takes the camera's.
    Camera& camera = scene.CameraRef();
    const OceanRenderable* ocean = scene.FindOceanRenderable();
    camera_.Apply(camera, game::PoseOf(scene.Physics().GetHelm()), ocean ? ocean->GetWaterLevel() : -1.0e6f);
}

void SpectatorSystem::OnHandOver(Scene& scene, Renderer* /*renderer*/, RuntimeObjects& /*objects*/)
{
    if (helm_.Applies()) { return; } // the helm's camera has the frame (the spectator's boat stays as it was)
    Camera& camera = scene.CameraRef();
    if (RenderableObject* watched = !game::ScriptCamera().free ? race_.WatchedVessel() : nullptr)
    {
        // race.watch: a bot's boat, from the pose this frame draws it at -- the helm's chase camera, on another hull. Another
        // boat is a cut.
        std::vector<physics::VesselHull> hulls;
        scene.Physics().CollectHulls(hulls);
        for (const physics::VesselHull& hull : hulls)
        {
            if (hull.object != watched || !hull.settings) { continue; }
            if (spectated_ != watched)
            {
                camera_.Begin();
                camera.ResetHistory();
                spectated_ = watched;
            }
            camera_.Tick(0.0f, 0.0f, hull.speed, 0.0f, 0.0f, race_.LastDeltaTime());
            const OceanRenderable* ocean = scene.FindOceanRenderable();
            camera_.Apply(camera, game::PoseOf(hull), ocean ? ocean->GetWaterLevel() : -1.0e6f);
            break;
        }
    }
    else { spectated_ = nullptr; }
}

void SpectatorSystem::OnDrawUi(const SessionFrame& frame)
{
    // PlaySession::DrawSpectatorPanel, moved verbatim (R13.2): only in Play, so `playing_` is gone from the test.
    game::Race& race = race_;
    if (frame.atHelm || !race.WatchedVessel()) { return; }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, 96.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("Watching a boat", nullptr, flags))
    {
        ImGui::End();
        return;
    }
    // Which boat: the racers in their places (the bot boats outside a race).
    // A radio row each (four at the most on a grid): no popup, which a frame of Play could take the focus from.
    const std::vector<std::string> names = race.WatchNames();
    const int index = race.WatchIndex();
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::RadioButton(names[i].c_str(), static_cast<int>(i) + 1 == index)) { race.SetWatch(static_cast<int>(i) + 1); }
        ImGui::PopID();
    }
    if (ImGui::Button("My camera")) { race.SetWatch(0); }
    ImGui::Separator();

    // The view: behind it, from its cockpit, a director's shots.
    game::VesselCamera& view = camera_;
    const game::VesselCamera::View views[] = { game::VesselCamera::View::Chase, game::VesselCamera::View::Helm, game::VesselCamera::View::Cinema };
    const char* labels[] = { "Behind", "Cockpit", "Cinema" };
    for (int i = 0; i < 3; ++i)
    {
        if (i > 0) { ImGui::SameLine(); }
        if (ImGui::RadioButton(labels[i], view.GetView() == views[i]) && view.GetView() != views[i])
        {
            view.SetView(views[i]);
            frame.scene.CameraRef().ResetHistory();
        }
    }
    bool vision = game::BotSettingsRef().debug >= 0.5f;
    if (ImGui::Checkbox("Bots' vision", &vision)) { game::BotSettingsRef().debug = vision ? 1.0f : 0.0f; }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("bot.debug: each bot's plan coloured by its speed limit, the buoys and boats it plans round, its aim, and a\n"
                          "card over it with its speed, target, throttle and rudder and why.");
    }
    ImGui::TextDisabled("Tab: the next boat   V: the next view   Esc: stop");
    ImGui::End();
}

void RaceSystem::OnSessionBegin(Scene& /*scene*/, const RaceCourse& course) { race_.Begin(course); }
void RaceSystem::OnSessionEnd(Scene& scene) { race_.End(scene.Physics()); }

void RaceSystem::OnFrame(Scene& scene, float deltaTime)
{
    race_.Tick(scene.Physics(), deltaTime); // the bots' orders, and the helm's on autopilot: before the helm reads its keys
    if (race_.TakeCameraCut()) { scene.CameraRef().ResetHistory(); } // the boats were put on the grid: a jump, not a pan
}

void RaceSystem::OnLevelGone()
{
    race_.Forget();       // its boats are
    race_.DropSpawned();  // and the race's own boats, made or asked for
}

void RaceSystem::OnDrawUi(const SessionFrame& frame)
{
    // A race's results (a window of this frame's, the mouse and Enter its own while it is up); on the grid who is who, a
    // bot's level over its boat; bot.debug: what each bot decides and why.
    race_.DrawResults();
    race_.DrawLabels(frame.scene.CameraRef());
    race_.DrawBotCards(frame.scene.CameraRef());
}

void RaceSystem::OnDebugDraw(DebugDrawSystem& draw) const { race_.DebugDraw(draw); } // bot.debug
std::wstring RaceSystem::HudLine() const { return race_.HudLine(); }
std::wstring RaceSystem::BigText() const { return race_.BigText(); }
bool RaceSystem::WantsCursor() const { return race_.ResultsOpen(); }

void PlayWorld::BeginLevel(const std::string& path)
{
    // R6.2b: was JsonLevel's -- the Player Start reset on every load, the course built anew.
    levelPlayerStart.reset();
    loadingCourse_ = RaceCourse{};
    loadingPath_ = path;
}

void PlayWorld::LevelObject(const nlohmann::json& o, bool enabled)
{
    // R6.2b: the loader's own reads of these keys, moved verbatim (under its StrictScope, value() still throws and the
    // load stops with the file named). The race: a mark of a gate ("raceGate"), a boat a bot helms ("raceBot") --
    // whatever the object is.
    const auto toFloat3 = [](const nlohmann::json& j) {
        if (!j.is_array() || j.size() < 3) { return Math::float3(0.0f, 0.0f, 0.0f); }
        return Math::float3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    };
    if (enabled) { ReadRaceKeys(o, toFloat3(o.value("position", nlohmann::json::array())), loadingCourse_); }
    // The Player Start: the first enabled one of the level.
    if (enabled && o.value("type", std::string{}) == kPlayerStartType && !levelPlayerStart)
    {
        const Math::float3 rotationDeg = toFloat3(o.value("rotationDeg", nlohmann::json::array()));
        PlayerStart start;
        start.feet = toFloat3(o.value("position", nlohmann::json::array()));
        start.pitchRad = rotationDeg.x * Math::DEG2RAD;
        start.yawRad = rotationDeg.y * Math::DEG2RAD;
        ReadPlayerKeys(o, start);
        levelPlayerStart = std::move(start);
    }
}

void PlayWorld::EndLevel()
{
    loadingCourse_.level = loadingPath_;
    levelRaceCourse = std::move(loadingCourse_);
    loadingCourse_ = RaceCourse{};
}

void PlayWorld::Forget()
{
    for (GameSystem* system : Systems()) { system->OnLevelGone(); } // R13.2: the race's boats were among them
}

void RaceSystem::OnHandOver(Scene& scene, Renderer* renderer, RuntimeObjects& objects)
{
    // PlayWorld::SyncRaceBoats until R13.2, moved verbatim (`race` is the system's race_).
    Race& race = race_;
    std::vector<const RenderableObjectBase*> gone = race.TakeDespawns();
    std::vector<game::RaceSpawn> spawns = race.TakeSpawns();
    if (!renderer)
    {
        for (const game::RaceSpawn& spawn : spawns) { race.OnSpawned(spawn.ticket, nullptr); }
        return;
    }
    // R13.1: in and out of the world by the hand-over's road (RuntimeObjects) -- its one upload batch, sent once the
    // hand-over's objects are made; the grid's own batch and GPU wait before.
    if (!gone.empty())
    {
        const std::size_t removed = objects.Despawn(gone);
        LOG_INFO(logging::LogCategory::Scene, "race: {} of its boat(s) taken out of the world", removed);
    }
    if (spawns.empty()) { return; }
    SceneObjectRegistry registry = SceneObjectRegistry::CreateWithBuiltins();
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
        SceneObjectRegistry::ObjectList made = registry.Create("staticMesh", creation, object);
        RenderableObject* vessel = nullptr;
        for (std::unique_ptr<RenderableObjectBase>& one : made)
        {
            if (!one) { continue; }
            RenderableObject* renderable = one->AsRenderableObject();
            // R13.3: the session's -- the race takes its boats out itself (Race::End), the journal after it if it did not.
            if (objects.Spawn(std::move(one), RuntimeObjects::Lifetime::Session) && !vessel) { vessel = renderable; }
        }
        race.OnSpawned(spawn.ticket, vessel);
        LOG_INFO(logging::LogCategory::Scene, "race: a boat {} for the grid", vessel ? "made" : "NOT made");
    }
}
} // namespace game
