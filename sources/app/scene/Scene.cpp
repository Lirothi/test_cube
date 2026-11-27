#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "rendering/lighting/Astronomy.h" // T3: Scene::SetSunToward solves the time of day
#include "vfx/ParticleTypes.h" // the spray's runtime ceilings (vfx::g_sprayRateMax / g_sprayMistMax)

#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "rendering/view/Camera.h"
#include "app/Systems.h"
#include "core/Helpers.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderConstants.h" // g_cleanCapture
#include "rendering/core/UploadBatch.h"
#include "app/scene/SceneObjectRegistry.h"
#include "third_party/json/json.hpp"
#include "rendering/meshes/LodSelect.h" // render::g_shadowLodBias (shadow caster LOD)
#include "rendering/debug/LodDebugView.h" // render::DrawLodDebug (LOD selection debug view)
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/renderables/InstancedDrawBatch.h" // S14 readout: count a batch's members, not the batch
#include "rendering/renderables/GBufferRenderable.h"  // plan L: the glowing meshes' emissive scale
#include "rendering/core/VisibilityStats.h" // occlusion plan S0: per-view visibility counters
#include "rendering/core/RenderStats.h"     // occlusion plan S0: draw/primitive totals in the readout
#include "core/diagnostics/ArtifactWriter.h" // S7: headless cascade readout dump
#include "core/diagnostics/BootProfile.h"    // game thread plan G10d: the level load's phases
#include "ocean/BuoyancyLayout.h" // the boats' hull footprints, for their wake
#include "ocean/OceanSimulation.h"
#include "ocean/OceanRenderable.h"
#include "core/task/TaskSystem.h"
#include "core/diagnostics/ThrowSelfTest.h" // --crash-selftest=view-throw: a culling task throws once
#include "core/threading/ThreadRole.h" // game thread plan: who may touch what, and when
#include "rhi/DeferredRelease.h" // R16.10: a removed object's GPU resources (RetireRemovedObject)
#include "core/threading/Knob.h"       // G5: the knobs cross at the hand-over
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/math/AABB.h"
#include "core/math/Frustum.h"
#include "core/containers/inl_vector.h"

void Scene::InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    render_.InitializeCommonResources(renderer, uploadCmdList, uploadKeepAlive);
}

void Scene::FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    render_.BeginLevelLoad(renderer, uploadCmdList, uploadKeepAlive); // the scene renderer's level resources
    SyncObjectsForRender(SceneObjectSyncReason::LevelLoad);
    // Local-light plan S9e: a fresh level starts from the initial shadow capacity -- the last level's lights do not keep
    // their view buffers resident; its own selection grows it again at the first ReconcileShadowMode. GPU idle here, and
    // the caster Rebuild below makes the cull's rows for it.
    lightManager_.ForgetWantedShadowed();
    lightManager_.SyncToRender(); // G3: the level's lights as loaded, before anything builds from them
    // The shadow capacity, the casters' buffers and the mega buffer, VSM's pool (RenderScene::FinishLevelLoad). R6.4a:
    // VSM's part now runs before the version bump below, which only drops the render half's queues.
    render_.FinishLevelLoad(renderer, uploadCmdList);
    BumpStaticSetVersion(); // Step 11: a fresh level = a new static caster set
}

void Scene::SyncObjectsForRender(SceneObjectSyncReason reason)
{
    for (const auto& obj : objects_)
    {
        if (obj)
        {
            obj->SyncSceneState(reason);
        }
    }
}

void Scene::BumpStaticSetVersion()
{
    ++staticSetVersion_; // the game's (the editor's command stack compares it)
    // Game thread plan G4b': the render half's queues hold raw object pointers and its version keys the caster set; both
    // are its own. A membership change happens with the render half parked (its objects die right after), so they go
    // at once; a visibility edit from the game half waits for the hand-over (Sync).
    if (threading::GameParkedOrSync()) { render_.ApplyStaticSet(staticSetVersion_); }
}

void Scene::SetDirectionalLight(DirectionalLight light)
{
    light.ContinueTransformVersion(dirLight_); // a fresh object must not reset the shadow caches' key
    dirLight_ = light;
}

void Scene::SetSkybox(std::unique_ptr<Skybox> skybox)
{
    // R16.10: frames in flight may still sample the old sky (its cube, its SRVs) -- it goes through render::deferred,
    // so the editor's sky edit parks instead of idling the GPU (EnvironmentRuntime::ApplySkybox / Remove).
    if (skyBox_)
    {
        std::shared_ptr<Skybox> old(std::move(skyBox_)); // std::function copies its callable
        render::deferred::Retire([old]() mutable { old.reset(); }, 0, "scene.skybox");
    }
    skyBox_ = std::move(skybox);
}

void Scene::AddObject(std::unique_ptr<RenderableObjectBase> obj) {
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::AddObject");
#if WITH_EDITOR
    if (obj)
    {
        obj->SetEditorObjectId(0);
    }
#endif
    objects_.push_back(std::move(obj));
#if WITH_EDITOR
    objectIds_.push_back(0); // runtime object: no editor identity
#endif
    BumpStaticSetVersion();
}

bool Scene::AddInitializedObject(Renderer& renderer, UploadBatch& uploads, std::unique_ptr<RenderableObjectBase> obj)
{
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::AddInitializedObject");
    if (!obj || !uploads.IsOpen())
    {
        return false;
    }

    obj->Init(&renderer, uploads.CommandList(), uploads.KeepAlive());
    obj->SyncSceneState(SceneObjectSyncReason::RuntimeSpawn);
#if WITH_EDITOR
    obj->SetEditorObjectId(0);
#endif
    objects_.push_back(std::move(obj));
#if WITH_EDITOR
    objectIds_.push_back(0);
#endif
    BumpStaticSetVersion();
    return true;
}

void Scene::RetireRemovedObject(std::unique_ptr<RenderableObjectBase> object)
{
    if (!object) { return; }
    object->DetachFromWorld();
    std::shared_ptr<RenderableObjectBase> held(std::move(object)); // std::function copies its callable
    render::deferred::Retire([held]() mutable { held.reset(); }, 0, "scene.object");
}

std::size_t Scene::RemoveRuntimeObjects(Renderer& renderer, const std::vector<const RenderableObjectBase*>& gone)
{
    (void)renderer;
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::RemoveRuntimeObjects");
    if (gone.empty()) { return 0; }
    // R16.10: was a WaitForPreviousFrame ("the frames in flight still read their buffers"); the buffers go through
    // render::deferred now (RetireRemovedObject), and what the render thread walks is the caller's park.
    threading::ParkRenderForGame("Scene::RemoveRuntimeObjects");
    std::size_t removed = 0;
    for (size_t i = 0; i < objects_.size();)
    {
        if (objects_[i] && std::find(gone.begin(), gone.end(), objects_[i].get()) != gone.end())
        {
            RetireRemovedObject(std::move(objects_[i]));
            objects_.erase(objects_.begin() + static_cast<ptrdiff_t>(i));
#if WITH_EDITOR
            objectIds_.erase(objectIds_.begin() + static_cast<ptrdiff_t>(i));
#endif
            ++removed;
            continue;
        }
        ++i;
    }
    if (removed > 0)
    {
        BumpStaticSetVersion();
        physics_.ForgetRemoved(); // their bodies and entries now: nothing below may be handed a pointer to them
    }
    return removed;
}

bool Scene::RemoveOceanObjects()
{
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::RemoveOceanObjects");
    bool removed = false;
    for (size_t i = 0; i < objects_.size();)
    {
        if (objects_[i] && objects_[i]->AsOceanRenderable())
        {
            objects_.erase(objects_.begin() + static_cast<ptrdiff_t>(i));
#if WITH_EDITOR
            objectIds_.erase(objectIds_.begin() + static_cast<ptrdiff_t>(i));
#endif
            removed = true;
            continue;
        }
        ++i;
    }
    if (removed)
    {
        BumpStaticSetVersion();
        physics_.ForgetRemoved(); // the rule of every removal here: see RemoveEditorObject
    }
    return removed;
}

void Scene::SetOceanVisible(bool visible)
{
    bool changed = false;
    for (std::unique_ptr<RenderableObjectBase>& obj : objects_)
    {
        if (obj && obj->AsOceanRenderable())
        {
            changed |= obj->IsVisibleGame() != visible;
            obj->SetVisible(visible);
        }
    }
    if (changed) { BumpStaticSetVersion(); }
}

#if WITH_EDITOR
Scene::SceneObjectId Scene::AddEditorObject(std::unique_ptr<RenderableObjectBase> obj)
{
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::AddEditorObject");
    const SceneObjectId id = nextEditorId_++;
    if (obj)
    {
        obj->SetEditorObjectId(id);
    }
    objects_.push_back(std::move(obj));
    objectIds_.push_back(id);
    BumpStaticSetVersion();
    return id;
}

void Scene::AddObjectWithEditorId(std::unique_ptr<RenderableObjectBase> obj, SceneObjectId id)
{
    assert(id != 0 && "AddObjectWithEditorId requires a non-zero editor id");
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::AddObjectWithEditorId");

    if (obj)
    {
        obj->SetEditorObjectId(id);
    }
    objects_.push_back(std::move(obj));
    objectIds_.push_back(id);
    BumpStaticSetVersion();

    // Keep the auto-allocator ahead of level-supplied ids so AddEditorObject
    // never hands out a colliding id later.
    if (id >= nextEditorId_)
    {
        nextEditorId_ = id + 1;
    }
}

bool Scene::AddInitializedEditorObject(Renderer& renderer, UploadBatch& uploads, SceneObjectId id,
    std::unique_ptr<RenderableObjectBase> obj, bool visible)
{
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::AddInitializedEditorObject");
    if (!obj || !uploads.IsOpen())
    {
        return false;
    }

    obj->SetVisible(visible);
    obj->Init(&renderer, uploads.CommandList(), uploads.KeepAlive());
    obj->SetEditorObjectId(id);
    obj->SyncSceneState(SceneObjectSyncReason::EditorSpawn);

    // Keep the auto-allocator ahead of editor-supplied ids so AddEditorObject
    // never hands out a colliding id later.
    if (id >= nextEditorId_)
    {
        nextEditorId_ = id + 1;
    }

    objects_.push_back(std::move(obj));
    objectIds_.push_back(id);
    BumpStaticSetVersion();
    return true;
}

bool Scene::RemoveEditorObject(SceneObjectId id)
{
    if (id == 0) // 0 is shared by all non-editor objects; never match on it
    {
        return false;
    }
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::RemoveEditorObject");
    bool removed = false;
    for (size_t i = 0; i < objectIds_.size();)
    {
        if (objectIds_[i] == id)
        {
            RetireRemovedObject(std::move(objects_[i])); // R16.10: its GPU resources once its frames are done
            objects_.erase(objects_.begin() + static_cast<ptrdiff_t>(i));
            objectIds_.erase(objectIds_.begin() + static_cast<ptrdiff_t>(i));
            removed = true;
            continue;
        }
        ++i;
    }
    if (removed)
    {
        BumpStaticSetVersion();
        // The physics world's entry of a removed object goes NOW, as in RemoveRuntimeObjects: an object's destructor only
        // posts the removal, and until the world's next Drain its entry holds the dead pointer -- which FindVessel and
        // CollectHulls hand out and this frame's Sync dereferences (the hulls for the wake and the spray). It used to
        // hold only because an editor delete runs in the UI's half and Scene::Tick (a Drain) follows it in the same
        // frame; a removal after the Tick -- an MCP call served at the hand-over, a deferred command -- had no such luck.
        physics_.ForgetRemoved();
    }
    return removed;
}

void Scene::RefreshShadowGpuForEditor(Renderer& renderer)
{
    // An editor spawn/delete changed the caster set, so ShadowGpuData's next UpdateForFrame will
    // Rebuild and drop megaReady_ — but nothing rebuilds the consolidated mega VB/IB mid-game (it is
    // only built at level load). Without it, VirtualShadowMap::RecordPageRender falls back to per-group
    // binding: 1024 pool pages × mesh-groups × (bind VB/IB + ExecuteIndirect) → ~10ms CPU, forever.
    // Game thread plan G4b': park the render half first, then hand it the edit this refresh is for (an object shown,
    // hidden or switched out of the shadows) -- the rebuild reads the render half's switches.
    renderer.WaitForPreviousFrame();
    for (const std::unique_ptr<RenderableObjectBase>& obj : objects_)
    {
        if (obj) { obj->SyncSwitchesToRender(); }
    }
    if (render_.StaticSetVersion() != staticSetVersion_) { render_.ApplyStaticSet(staticSetVersion_); }
    render_.RebuildShadowCasters(renderer);
}

RenderableObjectBase* Scene::FindEditorObject(SceneObjectId id)
{
    if (id == 0)
    {
        return nullptr;
    }
    for (size_t i = 0; i < objectIds_.size(); ++i)
    {
        if (objectIds_[i] == id)
        {
            return objects_[i].get();
        }
    }
    return nullptr;
}

const RenderableObjectBase* Scene::FindEditorObject(SceneObjectId id) const
{
    if (id == 0)
    {
        return nullptr;
    }
    for (size_t i = 0; i < objectIds_.size(); ++i)
    {
        if (objectIds_[i] == id)
        {
            return objects_[i].get();
        }
    }
    return nullptr;
}

void Scene::SetSelectedEditorObjectIds(const std::vector<SceneObjectId>& ids)
{
    // Sort + unique rather than the old scan-for-duplicates: that was O(n^2) on a list that is
    // now unbounded, and the render side needs it sorted anyway to test membership by bisection.
    selectedEditorObjectIds_.clear();
    selectedEditorObjectIds_.reserve(ids.size());
    for (const SceneObjectId id : ids)
    {
        if (id != 0) { selectedEditorObjectIds_.push_back(id); }
    }
    std::sort(selectedEditorObjectIds_.begin(), selectedEditorObjectIds_.end());
    selectedEditorObjectIds_.erase(
        std::unique(selectedEditorObjectIds_.begin(), selectedEditorObjectIds_.end()),
        selectedEditorObjectIds_.end());
}

Scene::SceneObjectId Scene::RaycastEditorObject(const Math::float3& origin,
    const Math::float3& dir,
    float* outDistance,
    SceneObjectId ignoredObjectId,
    const std::vector<SceneObjectId>* ignoredObjectIds,
    const std::vector<SceneObjectId>* onlyObjectIds) const
{
    SceneObjectId best = 0;
    float bestT = FLT_MAX;
    if (outDistance)
    {
        *outDistance = bestT;
    }
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };

    for (size_t i = 0; i < objects_.size(); ++i)
    {
        // Checked FIRST and before anything expensive: this is the whole point of the
        // restriction -- an object that is not in the set costs one comparison, not a
        // bounding-box test and a walk through its triangles.
        if (onlyObjectIds &&
            std::find(onlyObjectIds->begin(), onlyObjectIds->end(), objectIds_[i]) ==
                onlyObjectIds->end())
        {
            continue;
        }
        const bool ignoredBySet = ignoredObjectIds &&
            std::find(ignoredObjectIds->begin(), ignoredObjectIds->end(), objectIds_[i]) != ignoredObjectIds->end();
        if (objectIds_[i] == 0 || objectIds_[i] == ignoredObjectId || ignoredBySet ||
            !objects_[i] || !objects_[i]->IsVisibleGame() || !objects_[i]->IsRaycastPickable())
        {
            continue; // editor-owned + visible + pickable only (skip emitters/helpers)
        }

        const AABB& bounds = objects_[i]->GetWorldBounds();
        if (!bounds.IsValid())
        {
            continue;
        }

        const Math::float3 mn = bounds.GetMin();
        const Math::float3 mx = bounds.GetMax();
        const float lo[3] = { mn.x, mn.y, mn.z };
        const float hi[3] = { mx.x, mx.y, mx.z };

        // Slab test.
        float tmin = 0.0f;
        float tmax = FLT_MAX;
        bool hit = true;
        for (int a = 0; a < 3; ++a)
        {
            if (std::fabs(d[a]) < 1e-8f)
            {
                if (o[a] < lo[a] || o[a] > hi[a]) { hit = false; break; }
            }
            else
            {
                float inv = 1.0f / d[a];
                float t1 = (lo[a] - o[a]) * inv;
                float t2 = (hi[a] - o[a]) * inv;
                if (t1 > t2) { std::swap(t1, t2); }
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax) { hit = false; break; }
            }
        }

        if (!hit || tmin >= bestT)
        {
            continue;
        }

        float candidateT = tmin;
        RtInstanceDesc instance{};
        if (objects_[i]->GetRtInstance(instance) && instance.mesh &&
            instance.mesh->HasRaycastTriangles())
        {
            const Math::mat4 inverseWorld = Math::mat4::Inverse(instance.world);
            const Math::float3 localOrigin = inverseWorld.TransformPoint(origin);
            const Math::float3 localDirection = inverseWorld.TransformDirection(dir);
            const bool validLocalRay =
                std::isfinite(localOrigin.x) && std::isfinite(localOrigin.y) && std::isfinite(localOrigin.z) &&
                std::isfinite(localDirection.x) && std::isfinite(localDirection.y) && std::isfinite(localDirection.z) &&
                localDirection.Dot(localDirection) >= 1.0e-16f;
            if (validLocalRay)
            {
                if (!instance.mesh->RaycastLocal(localOrigin, localDirection, &candidateT))
                {
                    continue; // The ray crossed the AABB but missed the actual mesh surface.
                }
            }
        }

        if (candidateT < bestT)
        {
            bestT = candidateT;
            best = objectIds_[i];
        }
    }

    if (outDistance)
    {
        *outDistance = bestT;
    }
    return best;
}
#endif // WITH_EDITOR

OceanRenderable* Scene::FindOceanRenderable()
{
    // W1: the ocean's clock lives on the OceanRenderable, which enters objects_ through several
    // paths (level registry, editor spawn), and Clear() doesn't bump staticSetVersion_ — so a
    // cached pointer would be fragile. A once-per-frame scan is robust and negligible next to the
    // per-object Tick that just ran.
    return RenderScene::FindOcean(objects_);
}

Math::float3 Scene::GetSunToward() const
{
    return timeOfDay_.Present() ? timeOfDay_.SunToward() : (-dirLight_.GetDirection()).Normalized();
}

void Scene::SetSunToward(const Math::float3& toward)
{
    if (!timeOfDay_.Present())
    {
        dirLight_.SetDirection(-toward.Normalized());
        return;
    }
    // World -> the clock's local frame (x east, z north): undo the north yaw, then solve.
    TimeOfDaySettings& t = timeOfDay_.SettingsRef();
    const double world[3] = {toward.x, toward.y, toward.z};
    double local[3];
    astro::LocalToWorld(world, -t.northYawDeg, local);
    astro::SimplePath path;
    path.riseHours = t.dawnHours;
    path.setHours = t.duskHours;
    path.tiltDeg = t.pathTiltDeg;
    t.timeHours = static_cast<float>(astro::SolveSimpleTime(local, path));
    t.pathYawDeg = static_cast<float>(path.yawDeg);
    t.pathTiltDeg = static_cast<float>(path.tiltDeg);
    t.heightOffset = 0.0f;
    t.extendTwilight = 0.0f;
    t.realPositions = false;
    timeOfDay_.Tick(0.0f, dirLight_, skySun_);
}

void Scene::Tick(float deltaTime) {
    TC_CHECK_GAME("Scene::Tick");
    CPU_SCOPE(ProfilerScopes::kSceneTick);

    {
        CPU_SCOPE(ProfilerScopes::kSceneTickPointLights);
        for (PointLight& light : lightManager_.PointLights())
        {
            light.Tick(deltaTime);
        }
    }

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    const size_t batchSize = 64;
    {
        CPU_SCOPE(ProfilerScopes::kSceneTickObjects);
        TaskSystem::ParallelFor(objects_.size(),
            [this, deltaTime](size_t index) {
                if (index >= objects_.size()) {
                    return;
                }
                objects_[index]->Tick(deltaTime);
            }, batchSize);
    }

    // Buoyancy after the objects' Tick (the ocean clock is this frame's now); the render offsets it writes are folded
    // into this frame's model matrices at the hand-over (Scene::Sync, SyncSceneState).
    buoyancy_.Tick(FindOceanRenderable(), Systems::GetRenderer(), deltaTime);
    TickPhysics(deltaTime);

    {
        CPU_SCOPE(ProfilerScopes::kSceneTickPostObjects);
        TaskSystem::ParallelFor(objects_.size(),
            [this, deltaTime](size_t index) {
                if (index >= objects_.size()) {
                    return;
                }
                objects_[index]->PostTick(deltaTime);
            }, batchSize);
    }
#else
    {
        CPU_SCOPE(ProfilerScopes::kSceneTickObjects);
        for (auto& obj : objects_)
        {
            obj->Tick(deltaTime);
        }
    }

    buoyancy_.Tick(FindOceanRenderable(), Systems::GetRenderer(), deltaTime);
    TickPhysics(deltaTime);

    {
        CPU_SCOPE(ProfilerScopes::kSceneTickPostObjects);
        for (auto& obj : objects_)
        {
            obj->PostTick(deltaTime);
        }
    }
#endif

    // W1: advance the global wind from the SAME clock the ocean uses (its elapsedTime_), so waves
    // and foliage sway stay phase-coherent. No ocean -> standalone monotonic accumulator (coherence
    // is moot). Runs after the object-Tick barrier above, so the ocean's clock is current.
    {
        CPU_SCOPE(ProfilerScopes::kSceneTickWind);
        OceanRenderable* ocean = FindOceanRenderable();
        const float clock = ocean ? ocean->GetElapsedTime() : (windState_.time + deltaTime);
        windState_.Tick(clock);
        // W8: the fade origin (the camera) and the particles' drift push go to the render half at Sync.

        // W1/W2 verify (self-limiting): the wind clock tracks the ocean, windDirXZ is unit, and (W2)
        // the ocean's wind dir/force reflect the authored wind entity when it is active.
        static int s_windLogFrames = 0;
        if (s_windLogFrames < 8)
        {
            ++s_windLogFrames;
            const float len = std::sqrt(windState_.windDirXZ.x * windState_.windDirXZ.x +
                                        windState_.windDirXZ.y * windState_.windDirXZ.y);
            float oceanDir = -1.0f, oceanForce = -1.0f;
            if (ocean && ocean->GetSimulation())
            {
                oceanDir = ocean->GetSimulation()->GetLocalWindDirectionDegrees();
                oceanForce = ocean->GetSimulation()->GetWindForce01();
            }
            LOG_DEBUG(logging::LogCategory::Vfx,
                "wind verify frame={} time={:.4f} active={} windDir={:.1f} strength={:.2f} swayAmp={:.3f} | ocean={} oceanDir={:.1f} oceanForce={:.2f} |dir|={:.4f}",
                s_windLogFrames, windState_.time, windState_.active ? 1 : 0,
                windState_.directionDeg, windState_.strength, windState_.swayAmplitude,
                ocean ? "yes" : "none", oceanDir, oceanForce, len);
        }
    }

    // Time-of-day plan T2: its own clock (the wind's runs at another rate), then the sun or the moon into
    // dirLight_ -- after everything else this Tick wrote the light (the editor's apply runs before it), so
    // a level with a `timeOfDay` section always lights with the entity's light.
    timeOfDay_.Tick(deltaTime, dirLight_, skySun_);
    // Plan L: then the lamps, by where the time of day has just put the sun.
    TickNightLamps(deltaTime);
}

void Scene::TickPhysics(float deltaTime)
{
    physics_.SetOcean(FindOceanRenderable());
    physics_.SetCamera(camera_.GetPosition());
    physics_.SetDebugDraw(Systems::GetRenderer().GetDebugDrawSystem());
    physics_.Tick(deltaTime);
    Math::float3 eye;
    if (physics_.PlayerEye(eye)) { camera_.SetPosition(eye); } // the player: the hand-over takes this frame's pose
}

void Scene::TickNightLamps(float deltaTime)
{
    // The sun's height: the time of day's, else the level's own directional light's (a level without a clock
    // still has a day or a night).
    float sunElevationDeg = 90.0f;
    bool clockSetByHand = false;
    if (timeOfDay_.Present())
    {
        sunElevationDeg = timeOfDay_.SunElevationDeg();
        clockSetByHand = timeOfDay_.TakeClockSetByHand();
    }
    else
    {
        const Math::float3 d = -dirLight_.GetDirection();
        const float len = std::sqrt(std::max(1.e-12f, d.x * d.x + d.y * d.y + d.z * d.z));
        sunElevationDeg = std::asin(std::clamp(d.y / len, -1.0f, 1.0f)) * 57.2957795f;
    }
    const TimeOfDaySettings& t = timeOfDay_.Settings();
    NightLamps::Params params;
    params.sunElevationDeg = t.lampSunElevationDeg;
    params.randomDelaySec = t.lampRandomDelaySec;
    params.turnOnSec = t.lampTurnOnSec;
    params.turnOnFlicker = t.lampTurnOnFlicker;
    params.turnOnExponent = t.lampTurnOnExponent;
    const bool switched = nightLamps_.Tick(deltaTime, sunElevationDeg, clockSetByHand, params);

    // A NIGHT lamp is night light, and takes Night Brightness as the moon and the night sky do
    // (TimeOfDaySettings::nightBrightness): at UDS's moon (x11364, ~3600 lux of moonlight) a 4000-lm lamp's own
    // ~35 lux under it was 1 % of the moonlight and lit nothing visible; scaled, lamp and moon keep the ratio they
    // have outdoors (~100x under the lamp), and the lumens in the Inspector stay a bulb's. 1 = physics.
    // It comes in with the NIGHT, not with the switch: at sunset the scene is lit by the sun's sky, which is not
    // multiplied, and a lamp at x11364 there outshone the dusk (measured: blown pools at -0.3 degrees). So it rises in
    // stops by the night's share (TimeOfDay::NightShare, sun -6 -> -12): a physical lamp at sunset, faint against
    // the sky as street lights are, the full multiplier once the night is the moon's.
    const float nightLight = timeOfDay_.Present() ? timeOfDay_.NightGain() : 1.0f;
    // An ALWAYS-ON lamp (2026-10-21, the hundred lamps of wind_test) is the night's light by night -- Night Brightness,
    // as above -- and by day weighed against the day as Ultra Dynamic Sky weighs it (lampDayBrightness: its sun is 5
    // lux, ours 120000), from one to the other in stops by the night's share. Both 1 = physics.
    const float alwaysLight = timeOfDay_.Present()
        ? nightLight * std::pow(std::max(t.lampDayBrightness, 1.0e-6f), 1.0f - timeOfDay_.NightShare()) : 1.0f;
    const auto scaleOf = [&](LampSwitch lampSwitch, const Math::float3& position) {
        const float level = nightLamps_.Level(lampSwitch, position); // Always = 1, a no-op once there
        return lampSwitch == LampSwitch::Night ? level * nightLight
             : lampSwitch == LampSwitch::Always ? level * alwaysLight : level;
    };
    // A light's OWN day multiplier ("dayBrightness" on the light, 1 = none; 2026-10-27, owner: "give the lights a
    // brightness coefficient by day"): full while the sun is up, gone in stops by the night's share as Day Brightness
    // above is, so it multiplies with that one for an always-on lamp. Without a time of day it is always day. 0 = dark
    // until the night is the moon's.
    const float dayShare = timeOfDay_.Present() ? 1.0f - timeOfDay_.NightShare() : 1.0f;
    const auto dayOf = [&](float dayBrightness) {
        if (dayBrightness == 1.0f || dayShare <= 0.0f) { return 1.0f; }
        return dayBrightness <= 0.0f ? 0.0f : std::pow(dayBrightness, dayShare);
    };
    std::size_t lights = 0;
    for (PointLight& light : lightManager_.PointLights())
    {
        const PointLightDesc& desc = light.GetDesc();
        if (desc.onAt != LampSwitch::Always) { ++lights; }
        light.SetLampLevel(scaleOf(desc.onAt, desc.position) * dayOf(desc.dayBrightness));
    }
    for (SpotLight& light : lightManager_.SpotLights())
    {
        const SpotLightDesc& desc = light.GetDesc();
        if (desc.onAt != LampSwitch::Always) { ++lights; }
        light.SetLampLevel(scaleOf(desc.onAt, desc.position) * dayOf(desc.dayBrightness));
    }
    const std::vector<GBufferRenderable*>& glowing = NightLamps::Registered();
    for (GBufferRenderable* object : glowing)
    {
        object->SetEmissiveScale(scaleOf(object->GetLampSwitch(), object->GetPosition()));
    }
    if (switched && (lights > 0 || !glowing.empty()))
    {
        LOG_INFO(logging::LogCategory::Scene, "lamps: {} (sun {:.1f} deg), {} lights and {} glowing meshes switch{}",
                 nightLamps_.Night() ? "night" : "day", sunElevationDeg, lights, glowing.size(),
                 clockSetByHand ? " at once (clock set)" : "");
    }
}

AABB Scene::ComputeStaticBounds() const
{
    // Union of every renderable's world box except the ocean (its sheet spans the world). Used by
    // the replicate stress knob for its grid step; nothing hot calls this.
    bool any = false;
    float3 mn(0, 0, 0), mx(0, 0, 0);
    for (const auto& obj : objects_)
    {
        if (!obj || obj->AsOceanRenderable()) { continue; }
        const AABB& b = obj->GetWorldBounds();
        if (!b.IsValid()) { continue; }
        const float3 bmn = b.GetMin(), bmx = b.GetMax();
        if (!any) { mn = bmn; mx = bmx; any = true; continue; }
        mn = float3(std::min(mn.x, bmn.x), std::min(mn.y, bmn.y), std::min(mn.z, bmn.z));
        mx = float3(std::max(mx.x, bmx.x), std::max(mx.y, bmx.y), std::max(mx.z, bmx.z));
    }
    return any ? AABB(mn, mx) : AABB::Empty();
}

namespace
{
std::uint64_t FrozenHashBytes(const void* data, std::size_t size, std::uint64_t h = 1469598103934665603ull)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        h = (h ^ p[i]) * 1099511628211ull; // FNV-1a
    }
    return h;
}
} // namespace

void Scene::HashFrozenState(FrozenStateHash& out) const
{
    out.hashes.clear();
    out.objects = objects_.size();
    out.pointLights = lightManager_.PointLights().size();
    out.spotLights = lightManager_.SpotLights().size();
    out.hashes.reserve(out.objects * kFrozenObjectFields + out.pointLights + out.spotLights);
    for (const auto& obj : objects_)
    {
        const RenderableObject* r = obj ? obj->AsRenderableObject() : nullptr;
        out.hashes.push_back(r ? FrozenHashBytes(&r->GetModelMatrix(), sizeof(Math::mat4)) : 0u);
        out.hashes.push_back(r ? FrozenHashBytes(&r->GetPreviousModelMatrix(), sizeof(Math::mat4)) : 0u);
        out.hashes.push_back(r ? FrozenHashBytes(&r->GetRenderPosition(), sizeof(Math::float3)) : 0u);
        out.hashes.push_back(obj && obj->IsVisible() ? 1u : 0u);
        out.hashes.push_back(obj && obj->CastsShadow() ? 1u : 0u);
    }
    for (const PointLight& light : lightManager_.PointLights())
    {
        const PointLightDesc& d = light.GetRenderDesc();
        std::uint64_t h = FrozenHashBytes(&d.position, sizeof(d.position));
        h = FrozenHashBytes(&d.radius, sizeof(d.radius), h);
        h = FrozenHashBytes(&d.color, sizeof(d.color), h);
        out.hashes.push_back(FrozenHashBytes(&d.luminousFluxLm, sizeof(d.luminousFluxLm), h));
    }
    for (const SpotLight& light : lightManager_.SpotLights())
    {
        const SpotLightDesc& d = light.GetRenderDesc();
        const float flux = light.EffectiveFluxLm();
        std::uint64_t h = FrozenHashBytes(&d.position, sizeof(d.position));
        h = FrozenHashBytes(&d.range, sizeof(d.range), h);
        h = FrozenHashBytes(&d.color, sizeof(d.color), h);
        out.hashes.push_back(FrozenHashBytes(&flux, sizeof(flux), h));
    }
}

std::string Scene::FrozenFieldName(const FrozenStateHash& layout, std::size_t index) const
{
    static const char* const kObjectFields[kFrozenObjectFields] = {
        "model matrix", "previous model matrix", "render position", "visibility", "cast shadow" };
    const std::size_t objectHashes = layout.objects * kFrozenObjectFields;
    if (index < objectHashes)
    {
        const std::size_t object = index / kFrozenObjectFields;
        std::string name = "object #" + std::to_string(object);
#if WITH_EDITOR
        if (object < objectIds_.size() && objectIds_[object] != 0u)
        {
            name += " (editor id " + std::to_string(objectIds_[object]) + ")";
        }
#endif
        return name + " " + kObjectFields[index % kFrozenObjectFields];
    }
    index -= objectHashes;
    if (index < layout.pointLights)
    {
        return "point light #" + std::to_string(index) + " render desc";
    }
    return "spot light #" + std::to_string(index - layout.pointLights) + " render desc";
}

bool Scene::ScribbleFrozenStateForAudit()
{
    for (const auto& obj : objects_)
    {
        RenderableObject* r = obj ? obj->AsRenderableObject() : nullptr;
        if (r)
        {
            Math::mat4 m = r->GetModelMatrix();
            m.m._42 += 0.001f; // a millimetre up (row-vector translation): a game-half write into the frozen copy
            r->SetModelMatrix(m);
            return true;
        }
    }
    return false;
}

// Game thread plan G1: the frame's values that come from GAME state -- the level's settings, the time of day's
// tints and night camera, the sun's transmittance, the selection -- copied into frameData_ at the hand-over, where
// nothing else runs (docs/game_thread_plan.md section 3.3). PrepareViews publishes the render side's own pointers.
void Scene::Sync(Renderer* renderer)
{
    TC_CHECK_SYNC("Scene::Sync");
    CPU_SCOPE(ProfilerScopes::kSceneSync);
    threading::ClearRenderParked(); // the render half runs again after this
    render_.syncedThisFrame_ = true;
    render_.oceanSimulation_ = Systems::GetOceanSimulation(); // R6.4a: PrepareViews' (it read Systems there)
    knob::Exchange(); // G5: the knobs the game half set (and the ones the render half consumed) cross here
    if (renderer) { renderer->FlushHandOverSubmits(); } // G7: the game's GPU work (thumbnails, previews) to the queue

    // G2: the camera. The render half takes the game camera's pose (and a cut, by its revision); the game camera's own
    // matrices are refreshed for the pose about to render -- the gizmo, picking and the HUD read them next frame, as
    // they read the frame's CalcMatrices before.
    physics_.SyncPoses(); // what the step found while this thread waited for the frame before; the loop below takes them
    // The race's own boats (game::Race): made for a race where its grid is, gone when it is over -- here, the render half parked.
    if (gameHandOver_) { gameHandOver_(renderer); } // R6.2a: the game's part (game::PlayWorld::HandOver)
    // The boats' wake on the water (ocean/OceanWake): every hull as this frame draws it, with its footprint (resolved from
    // its geometry once a shape, then a cache hit), to the ocean's render half -- parked here.
    if (OceanRenderable* ocean = FindOceanRenderable())
    {
        physics_.CollectHulls(vesselHulls_);
        // Their spray (vfx/VesselSpray): emitters made on a boat's first frame, placed and driven here, before the
        // objects' SyncSceneState below takes their positions and rates; where each hull's bow wave breaks goes to the
        // wake below (its foam arms).
        vfx::SprayParams spray;
        if (const OceanSimulation* sim = ocean->GetSimulation())
        {
            const OceanRenderConfig& cfg = sim->GetRenderConfig();
            spray.enabled = cfg.sprayEnabled;
            spray.rate = std::min(cfg.sprayRate, vfx::g_sprayRateMax); // the Effects group's ceilings (docs/scalability_plan.md)
            spray.mist = std::min(cfg.sprayMist, vfx::g_sprayMistMax);
            spray.out = cfg.sprayOut;
            spray.up = cfg.sprayUp;
            spray.forward = cfg.sprayForward;
            spray.slam = cfg.spraySlam;
            // Over the water foam's tint: the drops are the same white water (the Inspector's Foam Tint moves both).
            spray.color = Math::float3(cfg.sprayColor.x * cfg.foamTint.x, cfg.sprayColor.y * cfg.foamTint.y,
                                       cfg.sprayColor.z * cfg.foamTint.z);
            spray.foamLight = cfg.sprayFoamLight;
            spray.forwardScatter = cfg.sprayForwardScatter;
            spray.opacity = cfg.sprayOpacity;
            spray.softFade = cfg.spraySoftFade;
        }
        vesselSpray_.Sync(*this, renderer, vesselHulls_, spray);
        std::vector<ocean::WakeHull> hulls;
        hulls.reserve(vesselHulls_.size());
        for (const physics::VesselHull& vessel : vesselHulls_)
        {
            ocean::WakeHull hull;
            hull.position = Math::float2(vessel.position.x, vessel.position.z);
            hull.forward = Math::float2(vessel.forward.x, vessel.forward.z);
            hull.scale = 0.5f * (vessel.scale.x + vessel.scale.z);
            // The asset's waterline: VesselSettings::draft is where still water stands on the mesh, over its origin.
            hull.footprint = buoyancy::ResolveFootprint(vessel.object->GetGeometryPath(),
                Math::float2(vessel.settings->forward.x, vessel.settings->forward.z), vessel.settings->draft);
            // Its breaking bow wave (the wake's two foam arms): the spray's root on each side and its share.
            hull.id = vessel.object;
            hull.speed = vessel.speed;
            vesselSpray_.BowWaveOf(vessel.object, hull.bowRoot, hull.bowShare, hull.length);
            if (hull.footprint)
            {
                hulls.push_back(std::move(hull));
            }
        }
        ocean->SetWakeHulls(std::move(hulls));
        // The hulls the water stays out of (ocean/WaterMask): every marked mesh at the pose this frame draws it. Collected
        // whatever ocean.waterMask says (the surface's switch): the particles collide with the same columns.
        std::vector<ocean::WaterMaskInstance> masks;
        ocean::watermask::Collect(masks);
        ocean->SetWaterMasks(std::move(masks));
    }
    render_.renderCameraCut_ |= render_.renderCamera_.TakePose(camera_);
    if (renderer) { camera_.CalcMatrices(renderer); }
    // G4: every object's transform, bounds and move flag (SyncSceneState -- the step PostTick used to run), its
    // material params and the render-facing animation of emitters, spinners and the ocean, and its switches.
    {
        CPU_SCOPE(ProfilerScopes::kSceneSyncObjects);
        TaskSystem::ParallelFor(objects_.size(),
            [this](size_t index) {
                if (index >= objects_.size() || !objects_[index]) { return; }
                objects_[index]->SyncSceneState(SceneObjectSyncReason::Frame);
                objects_[index]->SyncSwitchesToRender();
                // The sky occlusion map's (SkyOcclusionMap): a static caster moved (an editor drag) -- not a mover
                // that animates or rides a render offset (physics, buoyancy), which would redraw it every frame.
                if (const RenderableObject* ro = objects_[index]->AsRenderableObject();
                    ro && ro->MovedThisFrame() && !ro->HasRenderOffset() && !ro->IsDynamicCaster())
                {
                    render_.staticCasterMoved_.store(true, std::memory_order_relaxed);
                }
            }, 64);
    }
    if (render_.StaticSetVersion() != staticSetVersion_) { render_.ApplyStaticSet(staticSetVersion_); }
    if (renderer) { buoyancy_.SyncReadback(FindOceanRenderable(), *renderer); }
    // W8: the wind's fade origin is the CAMERA, shared by the G-buffer and every shadow view; and the drift push of the
    // GPU particle emitters. Read by the recording and by the emitters' tick (which ran before and reads last frame's,
    // as it always did).
    vfx::g_windFadeOriginWS = camera_.GetPosition();
    vfx::g_windDriftXZ = Math::float2(
        windState_.windDirXZ.x * windState_.strength * windState_.gustMul,
        windState_.windDirXZ.y * windState_.strength * windState_.gustMul);
    // G3: the lights' game descs (the editor's edits, the night lamps' levels, the candles' flicker) to the render half.
    lightManager_.SyncToRender();

    render_.frameData_.skyMoon = timeOfDay_.Present() ? timeOfDay_.GetSkyMoon() : SkyMoonFrame{};
    render_.frameData_.nightSky = timeOfDay_.Present() ? timeOfDay_.GetNightSky() : NightSkyFrame{};
    render_.frameData_.udsSky = timeOfDay_.Present() ? timeOfDay_.UdsSky() : UdsSkyFrameData{};
    // The stars twinkle on the scene's wind clock, so --wind-freeze stills them with everything else.
    render_.frameData_.nightSky.timeSeconds = windState_.time;
    render_.frameData_.settings = renderSettings_;
    // The atmosphere's authoritative copy is `heightFog_` (HeightFogRef: level load, Inspector,
    // --set). SetRenderSettings mirrors it into renderSettings_, but nothing guarantees a caller
    // between an HeightFogRef edit and this frame -- the scene-stress harness has none, and its
    // GBV gate ran with `volumetric` applied yet never rendered (volumetric fog plan A). The frame
    // takes the source directly.
    render_.frameData_.settings.heightFog = heightFog_;
    render_.frameData_.settings.skyAtmosphere = skyAtmosphere_;
    // Ultra Dynamic Sky's night Rayleigh and twilight ozone (TimeOfDaySettings, "twilight"), on the FRAME's
    // atmosphere: the Inspector, --set and the level file keep the scales as written.
    if (timeOfDay_.Present())
    {
        const float* rayleigh = timeOfDay_.RayleighTint();
        const float* ozone = timeOfDay_.OzoneTint();
        // ...and the night's aerosol (TimeOfDaySettings::mieNightScale): scattering and absorption, the anisotropy (.w)
        // kept.
        const float mie = timeOfDay_.MieScale();
        for (int c = 0; c < 3; ++c)
        {
            render_.frameData_.settings.skyAtmosphere.parameters.RayleighScattering[c] *= rayleigh[c];
            render_.frameData_.settings.skyAtmosphere.parameters.AbsorptionExtinction[c] *= ozone[c];
            render_.frameData_.settings.skyAtmosphere.parameters.MieScattering[c] *= mie;
            render_.frameData_.settings.skyAtmosphere.parameters.MieAbsorption[c] *= mie;
        }
        // ...and the day sky's brightness (TimeOfDaySettings::daySkyBrightness), the same way.
        render_.frameData_.settings.skyAtmosphere.luminanceScale *= timeOfDay_.SkyLuminanceScale();
        // ...and the height fog's density by the sun's height (TimeOfDaySettings::fogDayDensity ..), the frame's copy.
        render_.frameData_.settings.heightFog.density *= timeOfDay_.FogDensityScale();
    }
    // The sky's light in the height fog (UE's SkyAtmosphere HeightFogContribution) and Ultra Dynamic Sky's fog colour by
    // the sun's height (TimeOfDaySettings::fogUdsColor), on the frame's copy: HeightFogSettings' frame-only fields.
    {
        HeightFogSettings& fog = render_.frameData_.settings.heightFog;
        fog.frameSkyContribution = std::max(skyAtmosphere_.heightFogContribution, 0.0f);
        fog.frameSunScatterTint = float3(1.0f, 1.0f, 1.0f);
        fog.frameInscatterFloor = float3(0.0f, 0.0f, 0.0f);
        if (timeOfDay_.Present())
        {
            const float* tint = timeOfDay_.FogSunScatterTint();
            const float* lift = timeOfDay_.FogInscatterFloor();
            fog.frameSkyContribution *= timeOfDay_.FogSkyContribution();
            fog.frameSunScatterTint = float3(tint[0], tint[1], tint[2]);
            fog.frameInscatterFloor = float3(lift[0], lift[1], lift[2]);
        }
    }
    render_.frameData_.settings.volumetricCloud = volumetricCloud_;
    // B6.2: THE SUN'S TRANSMITTANCE, here because this is where the frame's light is still mutable
    // and because every consumer downstream reads it through GetEffectiveColor(). UE do the same
    // thing in PrepareSunLightProxy and hand it to the light proxy, which each lighting path then
    // multiplies in (LightGridInjection.cpp:1064, SceneRendering.cpp:1477); their fallback with no
    // atmosphere is white (SkyAtmosphereRendering.cpp:610), which is our HDRI mode, so those levels
    // do not move by a bit.
    //
    // Without it the sun keeps its full outer-space illuminance at any elevation and only N.L dims
    // it -- 4166 lux on horizontal ground at a 2.8 degree sun, and never reddening. The sky's own
    // LUTs must NOT get this: they integrate the extinction themselves, which is why the light
    // exposes GetOuterSpaceIlluminance() separately and MakeSkyView takes that one.
    {
        const auto d = -dirLight_.GetDirection();
        const float len = std::sqrt(std::max(1.e-12f, d.x * d.x + d.y * d.y + d.z * d.z));
        const float elevation = std::asin(std::clamp(d.y / len, -1.0f, 1.0f));
        // THE SUN SETS (time-of-day plan T0), in either sky mode: the share of its disc still above the
        // sea-level horizon, linear across the disc's angular diameter. Every shipped level's sun stands
        // 13-65 degrees up, so this is 1 for all of them; it matters once the sun moves -- before it, a
        // cubemap level kept a full white sun at any depth below the horizon and the procedural one
        // relied on the transmittance march's accident (now an explicit planet test).
        //
        // The atmosphere is crossed by the part of the disc that is still UP: once the centre is down,
        // the ray to it meets the planet and its transmittance is 0, though the disc's top still shines
        // until the whole disc is under (T2: that is where the time of day hands the light to the moon,
        // so there is no stretch in which neither lights). So it is taken at the middle of the visible
        // part -- between the horizon and the disc's top -- which is the centre itself once the disc is
        // wholly up.
        const float radius = std::max(dirLight_.GetSunHalfApexRadians(), 1.0e-4f);
        const float above = std::clamp(0.5f + 0.5f * elevation / radius, 0.0f, 1.0f);
        Math::float3 transmittance{1.0f, 1.0f, 1.0f};
        if (skyAtmosphere_.mode != 0u)
        {
            const float throughElevation = elevation >= radius ? elevation : std::max(elevation, 0.5f * (elevation + radius));
            float t[3] = {1.0f, 1.0f, 1.0f};
            SkyTransmittanceTowardSun(render_.frameData_.settings.skyAtmosphere.parameters, throughElevation, t);
            transmittance = Math::float3(t[0], t[1], t[2]);
        }
        transmittance = transmittance * above;
        dirLight_.SetAtmosphereTransmittance(transmittance);
        // The overcast deck takes the sun off the ground (the clouds still get it, GetAboveCloudsColor).
        // The clouds need the procedural sky: on a cubemap sky their pass does not run, so nor does this.
        dirLight_.SetCloudVisibility(skyAtmosphere_.mode != 0u ? CloudSunGroundVisibility(volumetricCloud_) : 1.0f);
    }
    render_.frameData_.cameraExposure = cameraExposure_;
    // The owner's night exposure (TimeOfDaySettings::nightExposureEv), on the FRAME's camera after the curve (the
    // curve stays keyed on the scene's own EV), by the frame's open-sky share like the steady camera below: the open
    // night darker, a closed room as by day (exposure_solve_cs.hlsl).
    if (timeOfDay_.Present())
    {
        render_.frameData_.cameraExposure.nightExposureEv = timeOfDay_.NightExposureEv();
        render_.frameData_.cameraExposure.nightOpenSkyFrom = timeOfDay_.Settings().nightCameraOpenFrom;
        render_.frameData_.cameraExposure.nightOpenSkyFull = timeOfDay_.Settings().nightCameraOpenFull;
        render_.frameData_.cameraExposure.nightIndoorLift = timeOfDay_.Settings().nightCameraIndoorLift;
    }
    // ...and the steady night camera (TimeOfDaySettings::nightCameraEv100), by the same night share. Its EV is the
    // FULL night's (the light at x nightBrightness); the night's light comes in by TimeOfDay::NightGain, so the
    // camera is held that many stops under it until then -- held at 13.13 while the moon was still x4310, the sun
    // 10.8 degrees down was 1.4 stops darker than both the dusk and the night (2026-10-31).
    if (timeOfDay_.Present() && timeOfDay_.Settings().nightCameraEv100 != 0.0f)
    {
        // nightCameraEv100 is the camera for the full moon 45 degrees up; another night's light moves it as the exposure
        // compensation CURVE says, read at that light's PHYSICAL EV (TimeOfDay::NightEv100: middle grey under the moon,
        // the twilight and the night sky, before Night Brightness -- -3.3 full moon, -6.9 quarter, -11 none): the camera
        // follows the light less the curve's change, (1 - slope) of it, as the auto exposure follows a scene. The
        // physical key keeps the night's section of the curve under the day's EVs: with Night Brightness in it a
        // quarter-moon beach (eye 7.56) and wind_test's bathroom by day (6.74) sat on the same point (owner, 2026-11-01:
        // "we have a special correction curve for the night, edit it"). Steady all the same: computed from the sky's
        // bodies, not the meter, so a lamp or a turn of the view does not move it. A curve flat there = the light in full.
        const float lightEv = timeOfDay_.NightLightEv();
        const float curveShift = render::EvaluateExposureCurve(render_.frameData_.cameraExposure, timeOfDay_.NightEv100()) -
                                 render::EvaluateExposureCurve(render_.frameData_.cameraExposure, timeOfDay_.NightRefEv100());
        render_.frameData_.cameraExposure.nightSteadyEv100 = timeOfDay_.Settings().nightCameraEv100 +
            std::log2(timeOfDay_.NightGain() / std::max(timeOfDay_.Settings().nightBrightness, 1.0e-6f)) +
            lightEv - curveShift;
        render_.frameData_.cameraExposure.nightSteadyFollow = timeOfDay_.Settings().nightCameraFollow;
        render_.frameData_.cameraExposure.nightSteadyWeight = timeOfDay_.NightShare();
    }
    render_.frameData_.colorPipeline = colorPipeline_;
#if WITH_EDITOR
    render_.renderSelection_ = selectedEditorObjectIds_; // the outline and the LOD view draw THIS frame's selection
    render_.frameData_.selectedEditorObjectIds = render_.renderSelection_.empty() ? nullptr : render_.renderSelection_.data();
    // A clean MCP capture draws no selection outline (render::g_cleanCapture).
    render_.frameData_.selectedEditorObjectCount = render::g_cleanCapture.load(std::memory_order_relaxed)
        ? 0u : static_cast<std::uint32_t>(render_.renderSelection_.size());
    render_.frameData_.selectionOutlineRadius = std::clamp<std::uint32_t>(selectionOutlineRadius_, 1u, 8u);
#else
    render_.frameData_.selectedEditorObjectIds = nullptr;
    render_.frameData_.selectedEditorObjectCount = 0;
    render_.frameData_.selectionOutlineRadius = 1;
#endif

    // G2: the render half's copies of the game's sun, sky sun, wind and cascade knobs -- after the transmittance above
    // went into the game light (the Mesh Editor's preview reads its effective colour), so the copy carries it too. With
    // no time of day the sky is lit by the scene light itself, as before: the same copy.
    render_.renderSun_ = dirLight_;
    if (timeOfDay_.Present()) { render_.renderSkySun_ = skySun_; }
    render_.frameData_.dirLight = &render_.renderSun_;
    render_.frameData_.skyLight = timeOfDay_.Present() ? &render_.renderSkySun_ : &render_.renderSun_;
    render_.frameData_.sunGlintScale = timeOfDay_.Present() ? timeOfDay_.GlintScale() : 1.0f;
    render_.renderWind_ = windState_;
    render_.frameData_.wind = &render_.renderWind_; // W3: gbuffer per-view CB reads this
    render_.renderCascadeConfig_ = cascadeConfig_;
    render_.frameData_.cascadeConfig = &render_.renderCascadeConfig_; // S8: a Developer Controls edit reaches the next frame
}

void Scene::Render(Renderer* renderer)
{
    // R6.4a: the render half draws (RenderScene::Render); the hand-over runs here first if the frame loop skipped it.
    if (renderer && !render_.syncedThisFrame_)
    {
        TC_CHECK_RENDER("Scene::Render");
        // Every frame loop calls Sync before Render (App::Run, the scene-stress harness). One that does not would
        // render last frame's settings: say so, once, and hand over here.
        LOG_ERROR_ONCE(logging::LogCategory::Scene, "Scene::Render without Scene::Sync this frame -- synced late");
        threading::RoleScope syncRole(threading::ThreadRole::Sync);
        Sync(renderer);
    }
    render_.Render(renderer);
}

void Scene::Clear()
{
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::Clear");
    render_.Clear(); // R6.4a: the render half first -- every pointer it holds into the objects below goes
    skyAtmosphere_ = {};
    lightManager_.Reset();
    vesselSpray_.Clear(); // its emitters are objects_ below
    if (gameForget_) { gameForget_(); } // R6.2a: the race's boats were objects_ too (game::PlayWorld::Forget)
    objects_.clear();
    physics_.ForgetRemoved(); // their entries with them (RemoveEditorObject says why): none outlives its object
#if WITH_EDITOR
    objectIds_.clear();
    selectedEditorObjectIds_.clear();
    selectionOutlineRadius_ = 1;
#endif
    skyBox_.reset();
}
