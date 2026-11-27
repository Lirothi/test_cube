#include "app/scene/World.h"
#include "core/logging/Log.h"
#include "rendering/lighting/Astronomy.h" // T3: World::SetSunToward solves the time of day

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
#include "core/Helpers.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/scene/RenderScene.h" // RenderScene::FindOcean, the walk both halves do
#include "rendering/renderables/GBufferRenderable.h"  // plan L: the glowing meshes' emissive scale
#include "ocean/OceanRenderable.h"
#include "core/task/TaskSystem.h"
#include "core/threading/ThreadRole.h" // game thread plan: who may touch what, and when
#include "rhi/DeferredRelease.h" // R16.10: a removed object's GPU resources (RetireRemovedObject)
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/math/AABB.h"
#include "core/containers/inl_vector.h"

void World::SetDirectionalLight(DirectionalLight light)
{
    light.ContinueTransformVersion(dirLight_); // a fresh object must not reset the shadow caches' key
    dirLight_ = light;
}

void World::SetSkybox(std::unique_ptr<Skybox> skybox)
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

void World::AddObject(std::unique_ptr<RenderableObjectBase> obj) {
    TC_CHECK_GAME_PARKED_OR_SYNC("World::AddObject");
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

bool World::AddInitializedObject(Renderer& renderer, UploadBatch& uploads, std::unique_ptr<RenderableObjectBase> obj)
{
    TC_CHECK_GAME_PARKED_OR_SYNC("World::AddInitializedObject");
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

void World::RetireRemovedObject(std::unique_ptr<RenderableObjectBase> object)
{
    if (!object) { return; }
    object->DetachFromWorld();
    std::shared_ptr<RenderableObjectBase> held(std::move(object)); // std::function copies its callable
    render::deferred::Retire([held]() mutable { held.reset(); }, 0, "scene.object");
}

std::size_t World::RemoveRuntimeObjects(Renderer& renderer, const std::vector<const RenderableObjectBase*>& gone)
{
    (void)renderer;
    TC_CHECK_GAME_PARKED_OR_SYNC("World::RemoveRuntimeObjects");
    if (gone.empty()) { return 0; }
    // R16.10: was a WaitForPreviousFrame ("the frames in flight still read their buffers"); the buffers go through
    // render::deferred now (RetireRemovedObject), and what the render thread walks is the caller's park.
    threading::ParkRenderForGame("World::RemoveRuntimeObjects");
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

bool World::RemoveOceanObjects()
{
    TC_CHECK_GAME_PARKED_OR_SYNC("World::RemoveOceanObjects");
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

void World::SetOceanVisible(bool visible)
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
World::SceneObjectId World::AddEditorObject(std::unique_ptr<RenderableObjectBase> obj)
{
    TC_CHECK_GAME_PARKED_OR_SYNC("World::AddEditorObject");
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

void World::AddObjectWithEditorId(std::unique_ptr<RenderableObjectBase> obj, SceneObjectId id)
{
    assert(id != 0 && "AddObjectWithEditorId requires a non-zero editor id");
    TC_CHECK_GAME_PARKED_OR_SYNC("World::AddObjectWithEditorId");

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

bool World::AddInitializedEditorObject(Renderer& renderer, UploadBatch& uploads, SceneObjectId id,
    std::unique_ptr<RenderableObjectBase> obj, bool visible)
{
    TC_CHECK_GAME_PARKED_OR_SYNC("World::AddInitializedEditorObject");
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

bool World::RemoveEditorObject(SceneObjectId id)
{
    if (id == 0) // 0 is shared by all non-editor objects; never match on it
    {
        return false;
    }
    TC_CHECK_GAME_PARKED_OR_SYNC("World::RemoveEditorObject");
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
        // hold only because an editor delete runs in the UI's half and World::Tick (a Drain) follows it in the same
        // frame; a removal after the Tick -- an MCP call served at the hand-over, a deferred command -- had no such luck.
        physics_.ForgetRemoved();
    }
    return removed;
}

RenderableObjectBase* World::FindEditorObject(SceneObjectId id)
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

const RenderableObjectBase* World::FindEditorObject(SceneObjectId id) const
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

void World::SetSelectedEditorObjectIds(const std::vector<SceneObjectId>& ids)
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

World::SceneObjectId World::RaycastEditorObject(const Math::float3& origin,
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

OceanRenderable* World::FindOceanRenderable()
{
    // W1: the ocean's clock lives on the OceanRenderable, which enters objects_ through several
    // paths (level registry, editor spawn), and Clear() doesn't bump staticSetVersion_ — so a
    // cached pointer would be fragile. A once-per-frame scan is robust and negligible next to the
    // per-object Tick that just ran.
    return RenderScene::FindOcean(objects_);
}

Math::float3 World::GetSunToward() const
{
    return timeOfDay_.Present() ? timeOfDay_.SunToward() : (-dirLight_.GetDirection()).Normalized();
}

void World::SetSunToward(const Math::float3& toward)
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

void World::Tick(float deltaTime, Renderer& renderer) {
    TC_CHECK_GAME("World::Tick");
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
    buoyancy_.Tick(FindOceanRenderable(), renderer, deltaTime);
    TickPhysics(deltaTime, renderer);

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

    buoyancy_.Tick(FindOceanRenderable(), renderer, deltaTime);
    TickPhysics(deltaTime, renderer);

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

void World::TickPhysics(float deltaTime, Renderer& renderer)
{
    physics_.SetOcean(FindOceanRenderable());
    physics_.SetCamera(camera_.GetPosition());
    physics_.SetDebugDraw(renderer.GetDebugDrawSystem());
    physics_.Tick(deltaTime);
    Math::float3 eye;
    if (physics_.PlayerEye(eye)) { camera_.SetPosition(eye); } // the player: the hand-over takes this frame's pose
}

void World::TickNightLamps(float deltaTime)
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

AABB World::ComputeStaticBounds() const
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

void World::BumpStaticSetVersion()
{
    ++staticSetVersion_; // the game's (the editor's command stack compares it)
    // Game thread plan G4b': the render half's queues hold raw object pointers and its version keys the caster set; both
    // are its own. A membership change happens with the render half parked (its objects die right after), so they go
    // at once; a visibility edit from the game half waits for the hand-over (Sync).
    if (threading::GameParkedOrSync()) { StaticSetChanged(); }
}

void World::Clear()
{
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
