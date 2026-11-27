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

const mat4& Scene::GetCascadeView(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.lightView[index];
}

const mat4& Scene::GetCascadeProj(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.lightProj[index];
}

float2 Scene::GetCascadeScale(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.atlasScale[index];
}

float2 Scene::GetCascadeBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.atlasBias[index];
}

float Scene::GetCascadeTexelWS(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.cascadeTexelWS[index];
}

float Scene::GetCascadeDepthBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.depthBiasNDC[index];
}

void Scene::InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    sceneRenderer_.InitializeCommonResources(renderer, uploadCmdList, uploadKeepAlive);
}

void Scene::FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    {
        BOOT_SCOPE("SceneResources::Finalize");
        sceneRenderer_.FinalizeLevelLoad(renderer, objects_, uploadCmdList, uploadKeepAlive, skyBox_.get());
    }
    SyncObjectsForRender(SceneObjectSyncReason::LevelLoad);
    // Local-light plan S9e: a fresh level starts from the initial shadow capacity -- the last level's lights do not keep
    // their view buffers resident; its own selection grows it again at the first ReconcileShadowMode. GPU idle here, and
    // the caster Rebuild below makes the cull's rows for it.
    lightManager_.ForgetWantedShadowed();
    lightManager_.SyncToRender(); // G3: the level's lights as loaded, before anything builds from them
    if (render::g_shadowSpotCapacity != render::kShadowSpotsInitial ||
        render::g_shadowPointCapacity != render::kShadowPointsInitial)
    {
        ApplyShadowCapacity(renderer, render::kShadowSpotsInitial, render::kShadowPointsInitial);
    }
    // Rung 0 / Steps 1-2: build the persistent per-caster shadow buffers (instance + bounds)
    // once the object transforms are finalized (SyncSceneState above resets motion history so
    // prevWorld == world). Level load is GPU-idle, safe for the alloc.
    // NOTE: anything FillBounds/Rebuild consumes must be identical here and in a mid-session
    // RebuildShadowCasters — a value published elsewhere on a different schedule (like the removed
    // W5 sway-extent global, docs/bug_shadow_lod_bias_perf.md) splits the two builds' cost/behavior.
    {
        BOOT_SCOPE("ShadowGpuData::Rebuild");
        shadowGpu_.Rebuild(renderer, objects_);
    }
    // Rung 2 mega-buffer: concatenate the caster meshes' VB/IB on this GPU-idle upload CL (meshes
    // are all in COMMON here, so the copy uses implicit promotion) for the VSM per-page draws.
    {
        BOOT_SCOPE("ShadowGpuData::EnsureMegaBuffer");
        shadowGpu_.EnsureMegaBuffer(renderer, uploadCmdList);
    }
    BumpStaticSetVersion(); // Step 11: a fresh level = a new static caster set
    // Rung 2 (Step 24b): allocate the persistent VSM page pool + page table only when VSM is the
    // active mode — Legacy mode keeps ZERO VSM resources resident. A runtime Ctrl+V switch reconciles
    // this at GPU idle in Scene::Render. (The mega-buffer above stays built regardless — it's tiny
    // and lets a runtime switch to VSM use the fast per-page draw path immediately.)
    if (render::VsmActive()) { vsm_.EnsureResources(renderer); }
    // Fresh level = every resident VSM page is stale (same view slots, different level content).
    // Drop all mappings so the first frames re-request/re-render cleanly.
    vsm_.InvalidateAllPages();
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

namespace
{
// Occlusion plan S3a.6: a frustum that rejects every finite box -- one unit plane a float-max
// away. Given to the shadow views of a light whose influence volume was occluded: the CPU cull
// keeps nothing, the GPU cull (which takes the same planes) keeps nothing, the pass still clears
// its atlas region so the barrier flow is untouched, and the lighting entry is zeroed beside it.
Frustum RejectAllFrustum()
{
    const float4 plane(0.0f, 1.0f, 0.0f, -1e30f);
    return Frustum::FromPlanes(&plane, 1);
}

// Occlusion plan S0: fill one view's visibility counters from its culled queue (before batching).
// Triangles are the CPU-path estimate: index count / 3 at the object's camera LOD (chunked meshes:
// per chunk at the chunk's tier), times the GI instance count where there is one.
// S1: `chunksDrawn` / `trianglesSubmitted` / `instancesDrawn` count what the view's frustum KEEPS
// below the object level. The camera reads the mask its SelectLod just wrote (and the GI cloud's
// visible-instance count); a cascade re-tests the chunk boxes against its own frustum right here,
// through the same predicate RenderShadow applies at draw time -- it must not read the camera's
// mask, and not only because a chunk behind the camera still casts: this runs on the cascade's
// prepare task, concurrently with the camera task that is writing that mask.
void AccumulateVisibility(render::VisibilityViewCounters& c, const SceneRenderQueue& queue, size_t sourceCount,
                          const Frustum& frustum, bool cameraView, std::uint32_t objectsOccluded,
                          std::uint32_t chunksOccluded)
{
    c.objectsIn = static_cast<std::uint32_t>(sourceCount);
    // S3a: the queue has already lost its occluded objects; `frustum` still means "kept by the
    // frustum test", so they are added back here and reported under `occluded`.
    c.objectsFrustum = static_cast<std::uint32_t>(queue.VisibleObjectCount()) + objectsOccluded;
    c.objectsOccluded = objectsOccluded;
    c.chunksOccluded = chunksOccluded;
    for (const auto& bucket : queue.VisibleBuckets())
    {
        for (const RenderableObjectBase* obj : bucket)
        {
            if (!obj) { continue; }
            const UINT gi = cameraView ? obj->GetCameraInstanceCount() : obj->GetInstanceCasterCount();
            const std::uint32_t instances = gi > 0u ? static_cast<std::uint32_t>(gi) : 1u;
            c.instancesDrawn += instances;
            const RenderableObject* ro = obj->AsRenderableObject();
            const Mesh* mesh = ro ? ro->GetMesh() : nullptr;
            if (!mesh) { continue; }
            const UINT lodCount = std::max(1u, mesh->GetLodCount());
            if (mesh->IsChunkedSubmeshes())
            {
                const std::vector<std::uint8_t>& lods = ro->ChunkCameraLods();
                // S6: the camera mask is the camera's -- a cascade's counter tests the frustum on
                // the spot, like its draw, and never touches the mask (the accessor asserts).
                static const std::vector<std::uint8_t> kNoMask;
                const std::vector<std::uint8_t>& camMask = cameraView ? ro->ChunkCameraVisible() : kNoMask;
                const std::vector<AABB>& boxes = mesh->GetSubmeshBounds();
                const Math::mat4& model = ro->GetModelMatrix();
                const size_t n = std::min(lods.size(), mesh->SubmeshesForLod(0).size());
                c.chunksIn += static_cast<std::uint32_t>(n);
                for (size_t s = 0; s < n; ++s)
                {
                    const bool drawn = cameraView
                        ? (s >= camMask.size() || camMask[s] != 0u)
                        : (s >= boxes.size() || RenderableObject::ChunkInFrustum(boxes[s].Transform(model), frustum));
                    if (!drawn) { continue; }
                    ++c.chunksDrawn;
                    const UINT lod = std::min<UINT>(lods[s], lodCount - 1u);
                    const auto& subs = mesh->SubmeshesForLod(lod);
                    if (s < subs.size()) { c.trianglesSubmitted += subs[s].indexCount / 3u; }
                }
            }
            else
            {
                const UINT lod = std::min<UINT>(obj->GetCameraLod(), lodCount - 1u);
                c.trianglesSubmitted += static_cast<std::uint64_t>(mesh->GetLodIndexCount(lod) / 3u) * instances;
            }
        }
    }
}
} // namespace

void Scene::BumpStaticSetVersion()
{
    ++staticSetVersion_; // the game's (the editor's command stack compares it)
    // Game thread plan G4b': the render half's queues hold raw object pointers and its version keys the caster set; both
    // are its own. A membership change happens with the render half parked (its objects die right after), so they go
    // at once; a visibility edit from the game half waits for the hand-over (Sync).
    if (threading::GameParkedOrSync()) { ApplyStaticSetToRender(); }
}

void Scene::ApplyStaticSetToRender()
{
    renderStaticSetVersion_ = staticSetVersion_;

    // Queue entries are non-owning raw pointers. An editor mutation can destroy an object between
    // frames, before UpdateCascades and other previous-frame diagnostics run. Drop every borrowed
    // pointer at the mutation boundary; PrepareViews repopulates all active queues this frame.
    renderQueueSourcesValid_ = false;
    shadowCasterSource_.Clear();
    cameraObjectSource_.Clear();
    cameraView_.queue.Clear();
    for (SceneView& view : shadowViews_.Cascades()) { view.queue.Clear(); }
    for (SceneView& view : shadowViews_.ClipmapViews()) { view.queue.Clear(); }
    for (SceneView& view : shadowViews_.SpotViews()) { view.queue.Clear(); }
    for (SceneView& view : shadowViews_.PointViews()) { view.queue.Clear(); }
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
    if (renderStaticSetVersion_ != staticSetVersion_) { ApplyStaticSetToRender(); }
    RebuildShadowCasters(renderer);
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

void Scene::RebuildShadowCasters(Renderer& renderer, bool sameGeometry)
{
    // Rebuild the caster data + consolidated mega VB/IB on a fresh GPU-idle upload batch (the meshes'
    // buffers have decayed to COMMON, so EnsureMegaBuffer's implicit-promotion copies are valid).
    // Mirrors FinalizeLevelLoad. The Rebuild pre-empts the per-frame UpdateForFrame rebuild (counts
    // already match), so there is no double work. Used by the editor caster-set refresh and the
    // shadow-LOD-bias change — both need the mega buffer (only ever built here) regenerated.
    renderer.WaitForPreviousFrame(); // no in-flight frame references the old mega buffers before we free them
    UploadBatch uploads;
    if (!uploads.Begin(&renderer)) { return; }
    shadowGpu_.Rebuild(&renderer, objects_, sameGeometry);
    shadowGpu_.EnsureMegaBuffer(&renderer, uploads.CommandList());
    uploads.SubmitAndWait(&renderer);
    // Content (materials/geometry) may have changed without a transform change. Keep the next VSM
    // frame from reusing cached pages rendered with the previous descriptors / previous LOD.
    shadowGpu_.ForceContentRefreshNextFrame();
}

void Scene::RebuildShadowMegaBuffer(Renderer& renderer)
{
    // UpdateForFrame already waited for the GPU and rebuilt the casters; the mesh buffers have decayed to COMMON, so the
    // batch's implicit-promotion copies are valid, as in RebuildShadowCasters.
    UploadBatch uploads;
    if (!uploads.Begin(&renderer)) { return; }
    shadowGpu_.EnsureMegaBuffer(&renderer, uploads.CommandList());
    uploads.SubmitAndWait(&renderer);
}

bool Scene::SkyOcclusionWanted(const Renderer* renderer) const
{
    // Wanted while the RT sky visibility is not traced -- the setting off, or no RT hardware (SceneRenderer::DecideFrame's
    // decisions_.skyVis, less its AS-failure term: a frame whose AS failed has no map yet and keeps the open sky).
    const SceneRenderSettings& rs = frameData_.settings;
    return rs.skyVisFallback && !(rs.skyVisEnabled && renderer && renderer->IsRaytracingSupported());
}

void Scene::ReconcileSkyOcclusion(Renderer* renderer)
{
    // The sky-from-above map (plan S's fallback): its textures follow skyVis.fallbackResolution and go when it is not
    // wanted -- freed or re-made here, never from the frame. The frames in flight read the old levels: they go through
    // render::deferred (SkyOcclusionMap::Resize; R16.10 -- this was a WaitForPreviousFrame). Made where it is first
    // wanted (PrepareViews: a creation needs no wait).
    if (!renderer || !skyOcclusion_.Built()) { return; }
    const bool wanted = SkyOcclusionWanted(renderer);
    const std::uint32_t want = wanted ? SkyOcclusionMap::ClampResolution(frameData_.settings.skyVisFallbackResolution) : 0u;
    if (want == skyOcclusion_.Resolution()) { return; }
    skyOcclusion_.Resize(renderer, want);
}

void Scene::ReconcileShadowLodCurve(Renderer* renderer)
{
    // The shadow LOD bias and tier stride pick the caster LOD rasterized into each shadow view.
    // The geometry lives in the consolidated mega buffer built at load, so a curve change needs a
    // GPU-idle rebuild. Cheap to poll; only rebuilds on an actual slider/sweep change. The clipmap's base moves the same
    // tiers, at each whole doubling (a drag of its slider rebuilds at those and nowhere between).
    // (Chunked-terrain LOD needs NO rebuild anywhere: its per-chunk camera tiers travel through a
    // per-frame CB override — see ShadowGpuData::RefreshChunkGroupLods.)
    if (!renderer) { return; }
    if (shadowGpu_.BuiltShadowLod() == render::g_shadowLodBias &&
        shadowGpu_.BuiltShadowLodTierStride() == render::ShadowLodTierStride() &&
        shadowGpu_.BuiltShadowLodBiasNearTier() == render::g_shadowLodBiasNearTier &&
        shadowGpu_.BuiltClipmapTierShift() == ShadowGpuData::ClipmapTierShift()) { return; }
    RebuildShadowCasters(*renderer);
}

OceanRenderable* Scene::FindOceanRenderable()
{
    // W1: the ocean's clock lives on the OceanRenderable, which enters objects_ through several
    // paths (level registry, editor spawn), and Clear() doesn't bump staticSetVersion_ — so a
    // cached pointer would be fragile. A once-per-frame scan is robust and negligible next to the
    // per-object Tick that just ran.
    for (std::unique_ptr<RenderableObjectBase>& obj : objects_)
    {
        if (obj)
        {
            if (OceanRenderable* ocean = obj->AsOceanRenderable())
            {
                return ocean;
            }
        }
    }
    return nullptr;
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

void Scene::PrepareViewQueue(SceneView& view, uint32_t cameraLayerMask)
{
    CPU_SCOPE(ProfilerScopes::kPrepareQueue);
    if (view.type == SceneView::Type::Shadow && !view.frustum.IsValid())
    {
        view.queue.Clear();
        return;
    }

    // Shadow and camera views using the camera's layer mask reuse their already bucketized,
    // BatchKey-sorted sources. Cull preserves source order, so neither view needs to sort again.
    const bool usesSharedShadowSource =
        view.type == SceneView::Type::Shadow && view.renderLayerMask == cameraLayerMask;
    const bool usesSharedCameraSource =
        view.type == SceneView::Type::Camera && view.renderLayerMask == cameraLayerMask;
    // Occlusion plan S6: for the rest of this prepare (a task thread of its own, parallel to the
    // camera's) the camera verdict accessors assert -- a shadow view that consulted them would
    // be the silent leak the UE rule (§2.5) forbids.
    struct ShadowPrepareGuard
    {
        explicit ShadowPrepareGuard(bool on) : on_(on) { if (on_) { render::g_preparingShadowView = true; } }
        ~ShadowPrepareGuard() { if (on_) { render::g_preparingShadowView = false; } }
        bool on_;
    } shadowPrepareGuard(view.type == SceneView::Type::Shadow);
    if (usesSharedShadowSource)
    {
        view.queue.Cull(view.frustum, shadowCasterSource_);
    }
    else if (usesSharedCameraSource)
    {
        view.queue.Cull(view.frustum, cameraObjectSource_);
    }
    else if (g_useFusedBucketizeCull)
    {
        view.queue.BucketizeCull(objects_, view.renderLayerMask,
            view.type == SceneView::Type::Shadow, view.frustum);
    }
    else
    {
        view.queue.Bucketize(objects_, view.renderLayerMask, view.type == SceneView::Type::Shadow);
        view.queue.Cull(view.frustum);
    }
    if (view.type == SceneView::Type::Camera)
    {
        view.queue.SortTransparent(view.view);
        // B6.1: split the sorted complex bucket into the depth-writing half (the ocean, drawn before
        // the screen-space fog) and the alpha-blended half (glass, drawn after it). Done here, right
        // after the sort, so both halves inherit the blend order the sort just established.
        view.transparentWater.clear();
        view.translucentComplex.clear();
        for (RenderableObjectBase* obj :
             view.queue.GetVisibleBucket(SceneRenderQueue::BucketType::TransparentComplex))
        {
            if (obj == nullptr) { continue; }
            (obj->IsDepthWritingTransparent() ? view.transparentWater : view.translucentComplex)
                .push_back(obj);
        }
        // Glass as a shading model: an OPAQUE object with glass material slots (thinTranslucent) draws
        // those slots in Main_Translucent too (RenderObjectBatch -> RenderTranslucent), blended back to
        // front with the glass objects by the key SortTransparent uses. Taken from the frustum's
        // survivors, before occlusion and the indirect G-buffer take objects out of the opaque buckets.
        const size_t glassBefore = view.translucentComplex.size();
        for (const SceneRenderQueue::BucketType type : { SceneRenderQueue::BucketType::OpaqueSimple,
                                                          SceneRenderQueue::BucketType::OpaqueComplex })
        {
            for (RenderableObjectBase* o : view.queue.GetVisibleBucket(type))
            {
                if (o && o->HasTranslucentSlots()) { view.translucentComplex.push_back(o); }
            }
        }
        if (view.translucentComplex.size() > glassBefore)
        {
            const auto depthOf = [&view](const RenderableObjectBase* o)
            {
                const AABB& b = o->GetWorldBounds();
                return b.IsValid() ? view.view.TransformPoint(b.GetCenter()).z : std::numeric_limits<float>::infinity();
            };
            std::stable_sort(view.translucentComplex.begin(), view.translucentComplex.end(),
                             [&depthOf](const RenderableObjectBase* a, const RenderableObjectBase* b)
                             { return depthOf(a) > depthOf(b); });
        }
        // Camera LOD must be selected before camera batches build their per-tier member lists.
        // S1: the view's own frustum rides along -- the chunk/instance masks use the planes the
        // object cull above used, so "object in, chunk out" is the same test at a finer grain.
        view.queue.SelectLods(renderCamera_, view.frustum);
        // S4: stamp what the camera's frustum kept -- the registry offers the GPU camera cull
        // only these (an object outside carries stale verdicts, and this frame's counters below
        // count exactly this set as "in the frustum").
        if (shadowGpu_.GBufferIndirectThisFrame())
        {
            const std::uint64_t frame = frameNumber_;
            for (const SceneRenderQueue::BucketType type : { SceneRenderQueue::BucketType::OpaqueSimple,
                                                              SceneRenderQueue::BucketType::OpaqueComplex })
            {
                for (RenderableObjectBase* o : view.queue.GetVisibleBucket(type)) { if (o) { o->MarkCameraVisible(frame); } }
            }
        }
    }
    // Occlusion plan S3a: after the frustum cull and the chunk masks, before batching (a batch is
    // built from what survives) and before the counters (which report what it removed).
    std::uint32_t objectsOccluded = 0, chunksOccluded = 0;
    if (view.type == SceneView::Type::Camera && occlusionHistory_.Enabled())
    {
        ApplyOcclusion(view, objectsOccluded, chunksOccluded);
        occlusionHistory_.EndConsider();
        const vis::OcclusionQueryPlan& plan = occlusionHistory_.Plan();
        render::OcclusionFrameStats& os = render::g_visibilityStats.occlusionCurrent;
        os.method = static_cast<std::uint32_t>(vis::g_occlusion.method);
        os.queriesIndividual = plan.individualQueries;
        os.queriesGrouped = plan.groupedQueries;
        os.queriesDropped = plan.droppedQueries;
        os.queriesTested = occlusionHistory_.TestedQueries();
        os.latencyFrames = occlusionHistory_.LatencyFrames();
        os.historyEntries = static_cast<std::uint32_t>(occlusionHistory_.EntryCount());
        os.ignoredResults = occlusionHistory_.IgnoredExistingQueries() ? 1u : 0u;
        std::uint32_t lights = 0;
        for (std::uint8_t o : spotLightOccluded_) { lights += o; }
        for (std::uint8_t o : pointLightOccluded_) { lights += o; }
        os.lightsOccluded = lights;
    }
    if (!usesSharedShadowSource && !usesSharedCameraSource)
    {
        view.queue.SortOpaque();
    }
    // Occlusion plan S0: visibility counters, taken BEFORE batching so every object counts once.
    // Only the camera and the four cascades have a slot; the fused path's queue holds survivors
    // only, so there `objectsIn` == `objectsFrustum` by construction.
    if (const int slot = VisibilitySlotFor(view); slot >= 0)
    {
        const size_t sourceCount = usesSharedShadowSource ? shadowCasterSource_.SourceObjectCount()
                                 : usesSharedCameraSource ? cameraObjectSource_.SourceObjectCount()
                                 : view.queue.SourceObjectCount();
        AccumulateVisibility(render::g_visibilityStats.current[static_cast<size_t>(slot)], view.queue, sourceCount,
                             view.frustum, view.type == SceneView::Type::Camera, objectsOccluded, chunksOccluded);
    }
    // Occlusion plan S4: the objects the GPU registry draws through the indirect G-buffer leave the
    // CPU buckets here -- after the counters (they ARE in the frustum), before the batching (a
    // batch must not re-collect them). Only on frames the indirect path runs; the registry's
    // camera cull then draws exactly these, with this frame's occlusion verdicts and chunk masks.
    if (view.type == SceneView::Type::Camera && shadowGpu_.GBufferIndirectThisFrame())
    {
        for (const SceneRenderQueue::BucketType type : { SceneRenderQueue::BucketType::OpaqueSimple,
                                                          SceneRenderQueue::BucketType::OpaqueComplex })
        {
            SceneRenderQueue::ObjectBucket& bucket = view.queue.GetVisibleBucket(type);
            bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                        [](RenderableObjectBase* o) { return o && o->GBufferIndirect(); }),
                         bucket.end());
        }
    }
    view.queue.BuildInstancedBatches(view.type == SceneView::Type::Camera);
}

void Scene::ApplyOcclusion(SceneView& view, std::uint32_t& objectsOccluded, std::uint32_t& chunksOccluded)
{
    CPU_SCOPE(ProfilerScopes::kPrepareQueue);
    // The camera and only the camera: shadow views take no occlusion verdicts (UE rule, plan §2.5).
    assert(view.type == SceneView::Type::Camera && "occlusion history consulted by a non-camera view");
    objectsOccluded = 0;
    chunksOccluded = 0;
    // Occlusion plan S6 (ownership): while the camera's two-pass HZB runs inside the indirect
    // G-buffer (S5), the objects that path draws belong to it -- tested on the GPU against this
    // frame's own depth, with no latency. Their CPU query would only feed the verdict bit the GPU
    // test supersedes (measured at the wall, K=4: 540 individual + 228 grouped queries a frame for
    // nothing). So they are not considered here: verdict cleared, chunk masks left as the frustum
    // made them, the GPU decides. Everything the CPU still draws -- and the lights -- stays with
    // the history. The knob is the S5 knob; `indirectQueries` is the pre-S6 A/B arm.
    const bool gpuOwnsIndirect = shadowGpu_.GBufferIndirectThisFrame() && render::g_gbufferHzbCullEnabled &&
                                 !vis::g_occlusion.indirectQueries;
    std::uint32_t gpuOwned = 0;
    // Opaque buckets only in S3a: the transparent buckets are mirrored in the queue's sorted
    // transparent entries, and pulling an object from one but not the other would draw it anyway.
    for (const SceneRenderQueue::BucketType type : { SceneRenderQueue::BucketType::OpaqueSimple,
                                                      SceneRenderQueue::BucketType::OpaqueComplex })
    {
        SceneRenderQueue::ObjectBucket& bucket = view.queue.GetVisibleBucket(type);
        const auto occluded = [&](RenderableObjectBase* obj) -> bool
        {
            if (!obj) { return false; }
            if (gpuOwnsIndirect && obj->GBufferIndirect())
            {
                obj->SetCameraOccluded(false);
                ++gpuOwned;
                return false;
            }
            RenderableObject* ro = obj->AsRenderableObject();
            const Mesh* mesh = ro ? ro->GetMesh() : nullptr;
            if (mesh && mesh->IsChunkedSubmeshes() && !ro->ChunkCameraVisible().empty())
            {
                // Sub-primitives (UE :2679, :2968-2980): every chunk the frustum kept has its own
                // history and its own, never grouped, query; the object goes only when all of them
                // are occluded. Chunks the frustum dropped are not considered -- their history is
                // left to age, as UE leaves an off-screen primitive's.
                std::vector<std::uint8_t>& mask = ro->ChunkCameraVisibleRef();
                const std::vector<AABB>& boxes = mesh->GetSubmeshBounds();
                const Math::mat4& model = ro->GetModelMatrix();
                bool anyVisible = false;
                for (size_t s = 0; s < mask.size(); ++s)
                {
                    if (mask[s] == 0u) { continue; }
                    if (s >= boxes.size() || !boxes[s].IsValid()) { anyVisible = true; continue; }
                    bool definite = false;
                    const bool occ = occlusionHistory_.Consider(
                        vis::OcclusionKey{ obj, static_cast<std::uint32_t>(1 + s) }, boxes[s].Transform(model),
                        /*allowGrouped=*/false, definite);
                    if (occ) { mask[s] = 0u; ++chunksOccluded; }
                    else { anyVisible = true; }
                }
                obj->SetCameraOccluded(!anyVisible); // S4: the GPU path reads the chunk mask + this
                if (!anyVisible) { ++objectsOccluded; return true; }
                return false;
            }
            const AABB& b = obj->GetWorldBounds();
            if (!b.IsValid()) { obj->SetCameraOccluded(false); return false; }
            bool definite = false;
            const bool occ = occlusionHistory_.Consider(vis::OcclusionKey{ obj, 0u }, b, /*allowGrouped=*/true, definite);
            obj->SetCameraOccluded(occ); // S4: this frame's verdict for the indirect G-buffer's cull
            if (occ) { ++objectsOccluded; }
            return occ;
        };
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(), occluded), bucket.end());
    }
    render::g_visibilityStats.occlusionCurrent.gpuOwnedObjects = gpuOwned;
}

void Scene::ConsiderLightOcclusion(const Frustum& cameraFrustum)
{
    const std::vector<SpotLight>& spots = lightManager_.SpotLights();
    const std::vector<PointLight>& points = lightManager_.PointLights();
    spotLightOccluded_.assign(spots.size(), 0u);
    pointLightOccluded_.assign(points.size(), 0u);
    if (!occlusionHistory_.Enabled()) { return; }
    // A volume outside the frustum is neither considered nor occluded: like an off-screen
    // primitive, its history just ages. One individual query per volume (UE allocates a
    // dedicated shadow query, never grouped); the verdict is last frame's, "no result" = lit.
    for (size_t i = 0; i < spots.size(); ++i)
    {
        const AABB& bounds = spots[i].GetConeBounds(); // encloses the finite outer cone
        if (!bounds.IsValid() || !cameraFrustum.Intersects(bounds)) { continue; }
        bool definite = false;
        spotLightOccluded_[i] = occlusionHistory_.Consider(vis::OcclusionKey{ &spots[i], 0u }, bounds,
                                                          /*allowGrouped=*/false, definite) ? 1u : 0u;
    }
    for (size_t i = 0; i < points.size(); ++i)
    {
        const PointLightDesc& desc = points[i].GetRenderDesc();
        const Math::float3 r(desc.radius, desc.radius, desc.radius);
        const AABB bounds(desc.position - r, desc.position + r);
        if (!cameraFrustum.Intersects(bounds)) { continue; }
        bool definite = false;
        pointLightOccluded_[i] = occlusionHistory_.Consider(vis::OcclusionKey{ &points[i], 0u }, bounds,
                                                           /*allowGrouped=*/false, definite) ? 1u : 0u;
    }
}

void Scene::SyncCameraView()
{
    cameraView_.view = renderCamera_.GetViewMatrix();
    cameraView_.invView = renderCamera_.GetInvViewMatrix();
    cameraView_.proj = renderCamera_.GetProjMatrix();
    cameraView_.invProj = renderCamera_.GetInvProjMatrix();
    cameraView_.position = renderCamera_.GetPosition();
    cameraView_.hfov = renderCamera_.GetBaseHFov();
    cameraView_.zNear = renderCamera_.GetZNear();
    cameraView_.zFar = renderCamera_.GetZFar();
    cameraView_.renderLayerMask = renderCamera_.GetRenderLayerMask();
}

int Scene::VisibilitySlotFor(const SceneView& view) const
{
    if (&view == &cameraView_) { return static_cast<int>(render::kVisibilityViewCamera); }
    const SceneView* first = shadowViews_.Cascades().data();
    if (&view >= first && &view < first + shadowViews_.Cascades().size())
    {
        return static_cast<int>(render::kVisibilityViewCascade0 + (&view - first));
    }
    return -1;
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
    syncedThisFrame_ = true;
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
    renderCameraCut_ |= renderCamera_.TakePose(camera_);
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
                    staticCasterMoved_.store(true, std::memory_order_relaxed);
                }
            }, 64);
    }
    if (renderStaticSetVersion_ != staticSetVersion_) { ApplyStaticSetToRender(); }
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

    frameData_.skyMoon = timeOfDay_.Present() ? timeOfDay_.GetSkyMoon() : SkyMoonFrame{};
    frameData_.nightSky = timeOfDay_.Present() ? timeOfDay_.GetNightSky() : NightSkyFrame{};
    frameData_.udsSky = timeOfDay_.Present() ? timeOfDay_.UdsSky() : UdsSkyFrameData{};
    // The stars twinkle on the scene's wind clock, so --wind-freeze stills them with everything else.
    frameData_.nightSky.timeSeconds = windState_.time;
    frameData_.settings = renderSettings_;
    // The atmosphere's authoritative copy is `heightFog_` (HeightFogRef: level load, Inspector,
    // --set). SetRenderSettings mirrors it into renderSettings_, but nothing guarantees a caller
    // between an HeightFogRef edit and this frame -- the scene-stress harness has none, and its
    // GBV gate ran with `volumetric` applied yet never rendered (volumetric fog plan A). The frame
    // takes the source directly.
    frameData_.settings.heightFog = heightFog_;
    frameData_.settings.skyAtmosphere = skyAtmosphere_;
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
            frameData_.settings.skyAtmosphere.parameters.RayleighScattering[c] *= rayleigh[c];
            frameData_.settings.skyAtmosphere.parameters.AbsorptionExtinction[c] *= ozone[c];
            frameData_.settings.skyAtmosphere.parameters.MieScattering[c] *= mie;
            frameData_.settings.skyAtmosphere.parameters.MieAbsorption[c] *= mie;
        }
        // ...and the day sky's brightness (TimeOfDaySettings::daySkyBrightness), the same way.
        frameData_.settings.skyAtmosphere.luminanceScale *= timeOfDay_.SkyLuminanceScale();
        // ...and the height fog's density by the sun's height (TimeOfDaySettings::fogDayDensity ..), the frame's copy.
        frameData_.settings.heightFog.density *= timeOfDay_.FogDensityScale();
    }
    // The sky's light in the height fog (UE's SkyAtmosphere HeightFogContribution) and Ultra Dynamic Sky's fog colour by
    // the sun's height (TimeOfDaySettings::fogUdsColor), on the frame's copy: HeightFogSettings' frame-only fields.
    {
        HeightFogSettings& fog = frameData_.settings.heightFog;
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
    frameData_.settings.volumetricCloud = volumetricCloud_;
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
            SkyTransmittanceTowardSun(frameData_.settings.skyAtmosphere.parameters, throughElevation, t);
            transmittance = Math::float3(t[0], t[1], t[2]);
        }
        transmittance = transmittance * above;
        dirLight_.SetAtmosphereTransmittance(transmittance);
        // The overcast deck takes the sun off the ground (the clouds still get it, GetAboveCloudsColor).
        // The clouds need the procedural sky: on a cubemap sky their pass does not run, so nor does this.
        dirLight_.SetCloudVisibility(skyAtmosphere_.mode != 0u ? CloudSunGroundVisibility(volumetricCloud_) : 1.0f);
    }
    frameData_.cameraExposure = cameraExposure_;
    // The owner's night exposure (TimeOfDaySettings::nightExposureEv), on the FRAME's camera after the curve (the
    // curve stays keyed on the scene's own EV), by the frame's open-sky share like the steady camera below: the open
    // night darker, a closed room as by day (exposure_solve_cs.hlsl).
    if (timeOfDay_.Present())
    {
        frameData_.cameraExposure.nightExposureEv = timeOfDay_.NightExposureEv();
        frameData_.cameraExposure.nightOpenSkyFrom = timeOfDay_.Settings().nightCameraOpenFrom;
        frameData_.cameraExposure.nightOpenSkyFull = timeOfDay_.Settings().nightCameraOpenFull;
        frameData_.cameraExposure.nightIndoorLift = timeOfDay_.Settings().nightCameraIndoorLift;
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
        const float curveShift = render::EvaluateExposureCurve(frameData_.cameraExposure, timeOfDay_.NightEv100()) -
                                 render::EvaluateExposureCurve(frameData_.cameraExposure, timeOfDay_.NightRefEv100());
        frameData_.cameraExposure.nightSteadyEv100 = timeOfDay_.Settings().nightCameraEv100 +
            std::log2(timeOfDay_.NightGain() / std::max(timeOfDay_.Settings().nightBrightness, 1.0e-6f)) +
            lightEv - curveShift;
        frameData_.cameraExposure.nightSteadyFollow = timeOfDay_.Settings().nightCameraFollow;
        frameData_.cameraExposure.nightSteadyWeight = timeOfDay_.NightShare();
    }
    frameData_.colorPipeline = colorPipeline_;
#if WITH_EDITOR
    renderSelection_ = selectedEditorObjectIds_; // the outline and the LOD view draw THIS frame's selection
    frameData_.selectedEditorObjectIds = renderSelection_.empty() ? nullptr : renderSelection_.data();
    // A clean MCP capture draws no selection outline (render::g_cleanCapture).
    frameData_.selectedEditorObjectCount = render::g_cleanCapture.load(std::memory_order_relaxed)
        ? 0u : static_cast<std::uint32_t>(renderSelection_.size());
    frameData_.selectionOutlineRadius = std::clamp<std::uint32_t>(selectionOutlineRadius_, 1u, 8u);
#else
    frameData_.selectedEditorObjectIds = nullptr;
    frameData_.selectedEditorObjectCount = 0;
    frameData_.selectionOutlineRadius = 1;
#endif

    // G2: the render half's copies of the game's sun, sky sun, wind and cascade knobs -- after the transmittance above
    // went into the game light (the Mesh Editor's preview reads its effective colour), so the copy carries it too. With
    // no time of day the sky is lit by the scene light itself, as before: the same copy.
    renderSun_ = dirLight_;
    if (timeOfDay_.Present()) { renderSkySun_ = skySun_; }
    frameData_.dirLight = &renderSun_;
    frameData_.skyLight = timeOfDay_.Present() ? &renderSkySun_ : &renderSun_;
    frameData_.sunGlintScale = timeOfDay_.Present() ? timeOfDay_.GlintScale() : 1.0f;
    renderWind_ = windState_;
    frameData_.wind = &renderWind_; // W3: gbuffer per-view CB reads this
    renderCascadeConfig_ = cascadeConfig_;
    frameData_.cascadeConfig = &renderCascadeConfig_; // S8: a Developer Controls edit reaches the next frame
}

namespace
{
// The name a culling task's Fatal gives (static storage, as FailurePolicy::Frame asks).
const wchar_t* ViewCullTaskName(SceneView::Type type)
{
    switch (type)
    {
    case SceneView::Type::Camera: return L"PrepareViewQueue (camera view)";
    case SceneView::Type::Shadow: return L"PrepareViewQueue (shadow view)";
    case SceneView::Type::ShoreDepth: return L"PrepareViewQueue (shore depth view)";
    case SceneView::Type::SkyOcclusion: return L"PrepareViewQueue (sky occlusion view)";
    }
    return L"PrepareViewQueue";
}
} // namespace

void Scene::PrepareViews(Renderer* renderer)
{
    CPU_SCOPE(ProfilerScopes::kPrepareViews);
    if (!renderer)
    {
        return;
    }
    // The LOD estimate projects each level's deviation onto the screen it is seen on (LodSelect.h).
    render::g_lodOutputWidthPx = static_cast<float>(std::max(renderer->GetWidth(), 1u));

    OceanSimulation* oceanSimulation = Systems::GetOceanSimulation();
    SceneView& mainView = cameraView_;
    SceneView* shoreViewPtr = nullptr;
    const uint32_t camMask = renderCamera_.GetRenderLayerMask();
    std::optional<Profiler::ScopedCpu> prepareViewsSetupScope(
        std::in_place, ProfilerScopes::kPrepareViewsSetup);
    renderCamera_.CalcMatrices(renderer);
    SyncCameraView();
    if (renderCameraCut_)
    {
        // The game camera was cut (set_camera, framing, a bookmark: its CalcMatrices, then ResetHistory). The render
        // camera repeats it in the same order, before anything reads its revision.
        renderCamera_.ResetHistory();
        renderCameraCut_ = false;
    }
    renderer->UpdateDlssCameraData(renderCamera_);

    // Publish this frame's pass inputs. SceneRenderer's pass bodies read frameData_,
    // not Scene members.
    frameData_.camera = &renderCamera_;
    frameData_.mainView = &cameraView_;
    frameData_.cascadeViews = &shadowViews_.Cascades();
    frameData_.clipmapViews = &shadowViews_.ClipmapViews();
    frameData_.clipmapSquares = &shadowViews_.ClipmapSquares();
    frameData_.smrtFrameIndex = vsm::g_smrtTemporalDither
        ? static_cast<std::uint32_t>((renderer->GetTotalFrameNumber() & 63ull) + 1ull)
        : 0u;
    frameData_.spotShadowViews = &shadowViews_.SpotViews();
    frameData_.pointShadowViews = &shadowViews_.PointViews();
    frameData_.lightManager = &lightManager_;
    frameData_.skybox = skyBox_.get();
    frameData_.objects = &objects_;
    frameData_.shadowGpu = &shadowGpu_;
    frameData_.localShadowCache = &localShadowCache_;
    frameData_.vsm = &vsm_;
    frameData_.ocean = FindOceanRenderable(); // caustics source for the deferred lighting pass
    frameData_.oceanSimulation = Systems::GetOceanSimulation(); // R6.3: the renderer reads it here, not from Systems

    mainView.renderLayerMask = renderCamera_.GetRenderLayerMask();
    mainView.frustum = Frustum::FromInvViewProj(mainView.invView, mainView.proj, renderCamera_.GetZNear(), renderCamera_.GetZFar());
    mainView.type = SceneView::Type::Camera;
    mainView.requiresDepthCheck = false;

    // The camera queue is independent of cascade/local-light view construction once its matrices
    // and shared source are ready. Publish it first so a worker can overlap its expensive
    // Cull/LOD/instancing chain with the remainder of PrepareViews setup on the main thread.
    if (!renderQueueSourcesValid_ || renderQueueSourceVersion_ != renderStaticSetVersion_ ||
        renderQueueSourceMask_ != camMask)
    {
        shadowCasterSource_.Bucketize(objects_, camMask, /*filterShadowCaster=*/true);
        shadowCasterSource_.SortOpaqueSource();
        cameraObjectSource_.Bucketize(objects_, camMask, /*filterShadowCaster=*/false);
        cameraObjectSource_.SortOpaqueSource();
        renderQueueSourceVersion_ = renderStaticSetVersion_;
        renderQueueSourceMask_ = camMask;
        renderQueueSourcesValid_ = true;
    }

    // Occlusion plan S3a: read the query results that can be read, open this frame's plan. On the
    // main thread BEFORE the camera task, which consults and extends the history; after
    // CalcMatrices, because the plan carries this frame's jittered view-projection.
    {
        // S3b: one history, two producers -- the query heap's sample counts (vis.method:1) or the
        // HZB tester's verdicts (vis.method:2). A producer whose device objects failed leaves the
        // method off for the session rather than culling on nothing.
        const int wanted = vis::g_occlusion.method;
        vis::OcclusionMethod method = vis::OcclusionMethod::Off;
        if (wanted == static_cast<int>(vis::OcclusionMethod::Queries) && occlusionQueries_.EnsureResources(renderer))
        {
            method = vis::OcclusionMethod::Queries;
        }
        else if (wanted == static_cast<int>(vis::OcclusionMethod::Hzb) && hzbTester_.EnsureResources(renderer))
        {
            method = vis::OcclusionMethod::Hzb;
        }
        const bool queriesOn = method == vis::OcclusionMethod::Queries;
        const bool hzbOn = method == vis::OcclusionMethod::Hzb;
        static_assert(vis::HzbOcclusionTester::kNoSlot == vis::OcclusionQueryHeap::kNoSlot, "one no-slot value for both producers");
        vis::OcclusionHistory::FrameResults results{};
        const std::uint64_t frameNo = renderer->GetTotalFrameNumber();
        if (queriesOn || hzbOn)
        {
            const std::uint32_t latency = static_cast<std::uint32_t>(
                std::clamp(vis::g_occlusion.queryLatency, 1, static_cast<int>(vis::kOcclusionBufferedFrames)));
            if (frameNo >= latency)
            {
                const std::uint64_t readFrame = frameNo - latency;
                const UINT slot = queriesOn ? occlusionQueries_.SlotOfFrame(readFrame) : hzbTester_.SlotOfFrame(readFrame);
                if (slot != vis::OcclusionQueryHeap::kNoSlot)
                {
                    // Latency below the frame count: that frame's fence has not been waited on by
                    // BeginFrame yet -- wait here, as UE's blocking Map does (r.NumBufferedOcclusionQueries=1).
                    if (latency < vis::kOcclusionBufferedFrames) { renderer->WaitForFrameSlot(slot); }
                    if (queriesOn && occlusionQueries_.ReadResults(readFrame, occlusionResults_))
                    {
                        results.samples = occlusionResults_.data();
                        results.count = static_cast<std::uint32_t>(occlusionResults_.size());
                        results.frameNumber = readFrame;
                    }
                    else if (hzbOn && hzbTester_.ReadResults(readFrame, hzbResults_))
                    {
                        results.hzbVisible = hzbResults_.data();
                        results.count = static_cast<std::uint32_t>(hzbResults_.size());
                        results.frameNumber = readFrame;
                    }
                }
            }
        }
        occlusionHistory_.BeginFrame(frameNo, GetTimeSeconds(), renderCamera_, renderer->GetRenderWidth(),
                                     renderer->GetRenderHeight(), renderStaticSetVersion_, results, method);
        frameData_.occlusionPlan = &occlusionHistory_.Plan();
        frameData_.occlusionQueries = &occlusionQueries_;
        frameData_.hzbTester = &hzbTester_;
        // S3a.6: the local lights' influence volumes, on this thread, before the camera task.
        ConsiderLightOcclusion(mainView.frustum);
        frameData_.spotLightOccluded = &spotLightOccluded_;
        frameData_.pointLightOccluded = &pointLightOccluded_;
    }

    TaskSystem& tasks = TaskSystem::Get();
    TaskSystem::TaskHandle mainViewTask = nullptr;
    {
        CPU_SCOPE(ProfilerScopes::kPrepareViewsDispatch);
        // A frame task (R15.3, the owner's answer В6): the frame draws what these queues hold, so a cull that threw is a
        // Fatal naming it -- not an Error line and a frame drawn from a half-built queue, which is what a background
        // task's exception meant here until the second review of wave 1 (item 1). --crash-selftest=view-throw.
        mainViewTask = tasks.CreateTask([this, &mainView, camMask]()
        {
            CPU_SCOPE(ProfilerScopes::kPrepareMainView);
            diag::ThrowSelfTestPoint(diag::ThrowSite::ViewCamera);
            PrepareViewQueue(mainView, camMask);
        }, 0, TaskSystem::FailurePolicy::Frame, L"PrepareViewQueue (camera view)");
        tasks.Submit(mainViewTask);
    }

    if (oceanSimulation)
    {
        // The shore field is a static map of the level, so it has to be told where the level IS.
        // Centred on the terrain's footprint, computed once per load — walking 600 objects every
        // frame for a value that only changes when the level does would be silly.
        if (!shoreAreaValid_)
        {
            float minX = FLT_MAX, minZ = FLT_MAX, maxX = -FLT_MAX, maxZ = -FLT_MAX;
            for (const auto& obj : objects_)
            {
                if (!obj || !IsLayerEnabled(obj->GetRenderLayerMask(), RenderLayer::Terrain))
                {
                    continue;
                }
                const AABB& bounds = obj->GetWorldBounds();
                minX = std::min(minX, bounds.GetMin().x);
                minZ = std::min(minZ, bounds.GetMin().z);
                maxX = std::max(maxX, bounds.GetMax().x);
                maxZ = std::max(maxZ, bounds.GetMax().z);
            }
            if (minX <= maxX)
            {
                oceanSimulation->SetShoreArea(float2((minX + maxX) * 0.5f, (minZ + maxZ) * 0.5f));
                shoreAreaValid_ = true;
            }
        }
        oceanSimulation->UpdateShoreView(renderCamera_);
        shoreViewPtr = &oceanSimulation->GetShoreDepthView();
    }
    // Plan S's fallback without RT: the world from above, wanted while the RT sky visibility is not traced
    // (SkyOcclusionWanted). Its resources are made here, before the graph; re-sized or freed by ReconcileSkyOcclusion.
    {
        const SceneRenderSettings& rs = frameData_.settings;
        const bool skyOccWanted = SkyOcclusionWanted(renderer);
        if (skyOccWanted) { skyOcclusion_.Prepare(renderer, rs.skyVisFallbackResolution); }
        skyOcclusion_.Update(renderCamera_.GetPosition(), renderStaticSetVersion_,
                             staticCasterMoved_.exchange(false, std::memory_order_relaxed), skyOccWanted,
                             rs.skyVisFallbackTexel);
        frameData_.skyOcclusion = (skyOccWanted && skyOcclusion_.Ready()) ? &skyOcclusion_ : nullptr;
    }
    shadowViews_.UpdateCascades(ShadowInputs(renderCamera_), renderCamera_, renderer); // R6.1
    shadowViews_.UpdateClipmap(ShadowInputs(renderCamera_), renderCamera_); // Step 24d: directional clipmap views (consumed only in VSM mode)

    // Choose which lit spots cast shadows this frame (highest projected size among
    // those whose influence intersects the view frustum) and their atlas slots,
    // then build one shadow view per slot from its owning light. Must precede the
    // view build and the Pass_SpotLights buffer fill.
    //
    // Use a NON-JITTERED frustum: mainView.frustum is built from the DLSS-jittered
    // projection, whose sub-pixel offset changes every frame. Feeding that into a
    // discrete per-frame selection makes a spot sitting near the frustum edge flip
    // in/out of the shadowed set as the jitter oscillates the planes — its shadow
    // flickers (Release-only, since jitter is active there). Shadow-caster
    // selection must be temporally stable, so cull against the un-jittered frustum.
    const Frustum shadowSelectFrustum = Frustum::FromInvViewProj(
        mainView.invView, renderCamera_.GetProjMatrixNoJitter(), renderCamera_.GetZNear(), renderCamera_.GetZFar());
    // Local-light shadow plan S3: UE's ScreenScale (LocalLightSceneProxy.cpp:104) at the RENDER resolution, as UE's
    // View.ViewRect is after the screen percentage.
    const mat4& shadowSelectProj = renderCamera_.GetProjMatrixNoJitter();
    const float shadowScreenScale = std::max(
        0.5f * static_cast<float>(renderer->GetRenderWidth()) * shadowSelectProj.m._11,
        0.5f * static_cast<float>(renderer->GetRenderHeight()) * shadowSelectProj.m._22);
    // Plan S4: the spot atlas slice is the largest a local shadow may be (ReconcileShadowMode keeps it the knob's).
    const auto& shadowTargets = renderer->GetDeferredForFrame();
    const std::uint32_t localShadowMaxRes = std::max(shadowTargets.spotShadowRes, 1u);
    // Plan S9e: every mode shadows as many spots / points as the view capacity holds (VSM's views are the whole cull
    // layout past the cascades, vsm::SpotView / PointFaceView); wanting more grows it at the next ReconcileShadowMode.
    const bool vsmLocal = render::VsmActive();
    lightManager_.SelectShadowedSpots(renderCamera_.GetPosition(), shadowSelectFrustum, shadowScreenScale, localShadowMaxRes,
        render::ShadowSpotCapacity());
    // B2a: choose which point lights cast (cube) shadows this frame, same non-jittered
    // frustum. Drives Pass_PointLights' shadowParams.x; the cube views + render pass are
    // B2b, sampling is B3, so this is inert (no visual change) until those land.
    lightManager_.SelectShadowedPoints(renderCamera_.GetPosition(), shadowSelectFrustum, shadowScreenScale, localShadowMaxRes,
        render::ShadowPointCapacity());
    // Plan S9: place them in the resident atlas pages (shrinking / dropping the lowest-ranked if they do not fit); the
    // pages it wants are grown at the next ReconcileShadowMode. VSM has no atlas.
    if (!vsmLocal)
    {
        LightManager::ShadowAtlasPages pages;
        pages.spotPages = std::max(shadowTargets.spotShadowPages, 1u);
        pages.pointPages = std::max(shadowTargets.pointShadowPages, 1u);
        pages.maxPages = std::clamp(render::g_localShadowMaxPages, 1u, render::kLocalShadowMaxPagesLimit);
        pages.spotPageSize = localShadowMaxRes;
        pages.pointFaceMax = std::max(shadowTargets.pointShadowRes, 1u);
        lightManager_.PackShadowAtlases(pages);
        const std::uint32_t shrunk = lightManager_.ShadowAtlasShrunk();
        const std::uint32_t dropped = lightManager_.ShadowAtlasDropped();
        if (shrunk != 0u || dropped != 0u)
        {
            LOG_INFO_ONCE_PER_MESSAGE(logging::LogCategory::RenderShadow,
                "local shadow atlas full ({} spot / {} point pages resident): {} light(s) at a lower resolution, {} without a "
                "shadow this frame; wants {} / {} pages", pages.spotPages, pages.pointPages, shrunk, dropped,
                lightManager_.WantedSpotShadowPages(), lightManager_.WantedPointShadowPages());
        }
    }
    // render::g_localShadowWindRange: a light whose reach stays farther than the range from the camera has a rigid shadow
    // -- in the atlas modes wholly cached, in VSM its pages drawn rigid and kept from wind dirt (VSM_VIEW_WIND_RIGID, via
    // the view list's mip.y). 1 m of hysteresis, so a light on the edge does not refill its cache entry (redraw its pages)
    // every frame the camera wobbles across it.
    const Math::float3 windCamera = renderCamera_.GetPosition();
    const auto windRigid = [&](std::vector<std::uint8_t>& memo, size_t lightIndex, const float3& position, float reach)
    {
        if (memo.size() <= lightIndex) { memo.resize(lightIndex + 1u, 0u); }
        bool rigid = false;
        if (render::g_localShadowWindRange >= 0.0f)
        {
            const float gap = std::max((position - windCamera).Length() - reach, 0.0f);
            rigid = gap > render::g_localShadowWindRange +
                          (memo[lightIndex] != 0u ? 0.0f : render::kLocalShadowWindRangeHysteresis);
        }
        memo[lightIndex] = rigid ? 1u : 0u;
        return rigid;
    };
    const size_t shadowedSpotCount = lightManager_.GetShadowedSpotCount();
    const auto& spotLights = lightManager_.SpotLights();
    for (size_t s = 0; s < shadowedSpotCount; ++s)
    {
        const size_t lightIndex = lightManager_.GetShadowedSpotLightIndex(s);
        const auto& light = spotLights[lightIndex];
        SceneView& view = shadowViews_.SpotViews()[s];
        view.type = SceneView::Type::Shadow;
        view.view = light.GetViewMatrix();
        view.proj = light.GetProjMatrix();
        view.invView = mat4::Inverse(view.view);
        view.invProj = mat4::Inverse(view.proj);
        const auto& desc = light.GetRenderDesc();
        const float nearPlane = light.GetShadowNear(); // plan S4: what its projection was built with
        const float farPlane = light.GetShadowFar();
        view.frustum = Frustum::FromInvViewProj(view.invView, view.proj, nearPlane, farPlane);
        // S3a.6: an occluded influence volume draws no shadow this frame (Legacy atlas only --
        // VSM never takes occlusion verdicts, UE rule).
        if (!render::VsmActive() && lightIndex < spotLightOccluded_.size() && spotLightOccluded_[lightIndex])
        {
            view.frustum = RejectAllFrustum();
        }
        view.renderLayerMask = renderCamera_.GetRenderLayerMask();
        view.position = desc.position;
        view.zNear = nearPlane;
        view.zFar = farPlane;
        view.hfov = 0.0f;
        // Plan S9e: the light's reach and its cone (the clamped outer angle its projection is built from).
        view.lightSphere = float4(desc.position, farPlane);
        view.lightCone = float4(light.GetDirection(), light.GetCosOuter());
        view.vsmMipBias = vsm::LocalMovingMipBias(lightManager_.GetSpotMobility(lightIndex)); // S10e
        view.windRigid = windRigid(spotWindRigid_, lightIndex, desc.position, farPlane);
        view.shadowBias = float2(desc.shadowBias, desc.shadowSlopeBias); // local-light shadows S1
        view.shadowRes = lightManager_.GetSpotShadowResolution(lightIndex); // S4: its square
        {
            const LightManager::ShadowAtlasPlacement place = lightManager_.GetSpotShadowPlacement(lightIndex); // S9
            view.shadowPage = static_cast<std::uint32_t>(std::max(place.page, 0));
            view.shadowX = place.x;
            view.shadowY = place.y;
        }
        view.requiresDepthCheck = false;
    }

    // B2b: point-light cube shadow views — 6 faces per shadowed point light. Face order
    // is the D3D cube-map convention (+X,-X,+Y,-Y,+Z,-Z) so a sample by direction
    // (P - lightPos) selects the matching slice (since plan S4 the shaders pick it themselves,
    // local_shadow_filter.hlsli PointShadowFace, by the same convention) (rendered into
    // shadowViews_.PointViews()[slot*6 + face], the view index Pass_PointShadows walks). All
    // faces share one 90° FOV perspective; only orientation differs.
    static const Math::float3 kCubeDir[6] = {
        { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f,  1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } };
    static const Math::float3 kCubeUp[6] = {
        { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f },
        { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    constexpr float kHalfPi = 1.57079632679f; // 90deg FOV per cube face
    const size_t shadowedPointCount = lightManager_.GetShadowedPointCount();
    const auto& pointLights = lightManager_.PointLights();
    for (size_t s = 0; s < shadowedPointCount; ++s)
    {
        const size_t lightIndex = lightManager_.GetShadowedPointLightIndex(s);
        const auto& desc = pointLights[lightIndex].GetRenderDesc();
        const float3 P = desc.position;
        // Plan S4: UE's 1 cm on the atlases' linear depth, else 2% of radius (min 0.2) so far/near stays ~20 for a usable
        // D16 perspective depth (render::PointShadowNear). MUST equal Pass_PointLights' shadowParams.z — the sampler
        // reconstructs the compare depth with this exact near/far (see PointShadowFactor).
        const float nearPlane = render::PointShadowNear(desc.radius);
        const float farPlane = std::max(desc.radius, nearPlane + 0.1f);
        // The atlas modes' faces carry a GUARD BAND of render::kPointShadowGuardTexels texels each side: a face 90 degrees
        // plus that much, so a filter tap up to that far past the seam reads its own face's depth, in its own depth axis,
        // instead of a clamped edge texel (which self-shadowed the floor along the seams once the texel bias stopped
        // hiding it). The shaders map a direction into the inner 90 degrees (local_shadow_filter.hlsli). VSM renders
        // these same views into its pages at exactly 90, so it keeps them.
        const float faceRes = static_cast<float>(std::max(lightManager_.GetPointShadowResolution(lightIndex), 1u));
        const float faceFov = vsmLocal ? kHalfPi
                                       : 2.0f * std::atan(1.0f + 2.0f * render::kPointShadowGuardTexels / faceRes);
        const mat4 proj = mat4::PerspectiveFovLH(faceFov, 1.0f, nearPlane, farPlane);
        const bool pointRigid = windRigid(pointWindRigid_, lightIndex, P, farPlane); // one verdict for the six faces
        for (int face = 0; face < 6; ++face)
        {
            SceneView& view = shadowViews_.PointViews()[s * 6 + static_cast<size_t>(face)];
            view.type = SceneView::Type::Shadow;
            view.view = mat4::LookAtLH(P, P + kCubeDir[face], kCubeUp[face]);
            view.proj = proj;
            view.invView = mat4::Inverse(view.view);
            view.invProj = mat4::Inverse(view.proj);
            view.frustum = Frustum::FromInvViewProj(view.invView, view.proj, nearPlane, farPlane);
            // S3a.6: see the spot loop.
            {
                const size_t pointLightIndex = lightManager_.GetShadowedPointLightIndex(s);
                if (!render::VsmActive() && pointLightIndex < pointLightOccluded_.size() && pointLightOccluded_[pointLightIndex])
                {
                    view.frustum = RejectAllFrustum();
                }
            }
            view.renderLayerMask = renderCamera_.GetRenderLayerMask();
            view.position = P;
            view.zNear = nearPlane;
            view.zFar = farPlane;
            view.hfov = 0.0f;
            view.lightSphere = float4(P, farPlane); // plan S9e: a face's corners reach to sqrt(3) x the radius
            view.lightCone = float4(0.0f, 0.0f, 0.0f, 2.0f);
            view.vsmMipBias = vsm::LocalMovingMipBias(lightManager_.GetPointMobility(lightIndex)); // S10e
            view.windRigid = pointRigid;
            view.shadowRes = lightManager_.GetPointShadowResolution(lightIndex); // S4: the face's square
            {
                // S9: the face's square inside the light's 3f x 2f block -- face k at (k % 3, k / 3) faces from its
                // corner, the layout local_shadow_filter.hlsli PointShadowCubeUV reads.
                const LightManager::ShadowAtlasPlacement place = lightManager_.GetPointShadowPlacement(lightIndex);
                view.shadowPage = static_cast<std::uint32_t>(std::max(place.page, 0));
                view.shadowX = place.x + static_cast<std::uint32_t>(face % 3) * view.shadowRes;
                view.shadowY = place.y + static_cast<std::uint32_t>(face / 3) * view.shadowRes;
            }
            view.requiresDepthCheck = false;
        }
    }

    // Local-light plan S7: every shadowed view asks the cache for its static casters -- a hit (copy them in, draw the
    // movables), a miss (the cache pass renders them into the pool first) or uncached (no room: draw all, as before).
    // Atlas modes with S6a's compaction only; an occluded light draws nothing this frame and is not asked (its empty
    // static rows would be cached as its picture). The cull decides later whether this frame really splits the rows
    // (not on its validation frame); the passes and the cache pass's commit read that decision.
    {
        // Without a pool yet every request is uncached and asks for one (grown at the next reconcile).
        const bool cacheOn = !vsmLocal && render::g_localShadowCache && render::g_localShadowCompact &&
                             shadowGpu_.MegaReady();
        localShadowCache_.BeginFrame(renderer ? renderer->GetTotalFrameNumber() : 0ull, shadowGpu_.LocalStaticVersion());
        // `view` names the key: its matrices and resolution (a point block's: its +X face's, the other five follow it).
        const auto request = [this](const SceneView& view, std::uint32_t id, bool linearDepth, bool pointBlock,
                                    render::LocalShadowCache::Rect& rect)
        {
            render::LocalShadowCache::Key key{};
            key.id = id;
            std::memcpy(key.viewProj.data(), &view.view.m, 16 * sizeof(float));
            std::memcpy(key.viewProj.data() + 16, &view.proj.m, 16 * sizeof(float));
            key.res = view.shadowRes;
            // Plan S4: the depth encoding; and the wind range -- a rigid light caches its swaying casters too, so a light
            // that crosses the range must not take the other mode's picture.
            key.encoding = (linearDepth ? 1u : 0u) | (view.windRigid ? 2u : 0u);
            return static_cast<std::uint8_t>(localShadowCache_.Request(key, rect, pointBlock));
        };
        const auto place = [](SceneView& view, std::uint8_t mode, const render::LocalShadowCache::Rect& rect,
                              std::uint32_t dx, std::uint32_t dy)
        {
            view.cacheMode = mode;
            view.cachePage = rect.page;
            view.cacheX = rect.x + dx;
            view.cacheY = rect.y + dy;
        };
        // Plan S7, the direct read (UE ShadowSetup.cpp:6002-6009, :6643-6653 -- SDCM_MovablePrimitivesOnly without subject
        // primitives projects straight from the cached map): a light whose views all hit and that has nothing movable in
        // reach is read from the pool by its receivers -- no copy, no draw. Only past the wind range (a light inside it has
        // its swaying casters to draw); a spot only while a pool page is a spot atlas page (the receivers' texel is the
        // page's), a point only as one 3f x 2f block (the receivers' face addressing).
        lightManager_.ClearShadowDirect();
        if (cacheOn) { shadowGpu_.CollectLocalMovables(); }
        const auto readsDirect = [&](const SceneView& view)
        {
            return render::g_localShadowDirectRead &&
                   view.cacheMode == static_cast<std::uint8_t>(render::LocalShadowCache::Mode::Hit) && view.windRigid &&
                   !shadowGpu_.LocalMovableTouches(view.lightSphere.xyz(), view.lightSphere.w);
        };
        for (size_t s = 0; s < shadowedSpotCount; ++s)
        {
            SceneView& view = shadowViews_.SpotViews()[s];
            const size_t lightIndex = lightManager_.GetShadowedSpotLightIndex(s);
            const bool occluded = lightIndex < spotLightOccluded_.size() && spotLightOccluded_[lightIndex];
            view.cacheMode = 0;
            view.cachePage = -1;
            view.directRead = false;
            if (!cacheOn || occluded) { continue; }
            render::LocalShadowCache::Rect rect{};
            place(view, request(view, render::LocalShadowCache::SpotId(static_cast<std::uint32_t>(lightIndex)),
                                render::SpotShadowLinearDepth(), false, rect), rect, 0u, 0u);
            if (localShadowCache_.PageSize() == localShadowMaxRes && readsDirect(view))
            {
                view.directRead = true;
                lightManager_.SetSpotShadowDirect(lightIndex, { view.cachePage, view.cacheX, view.cacheY });
                localShadowCache_.CountDirect();
            }
        }
        for (size_t s = 0; s < shadowedPointCount; ++s)
        {
            const size_t lightIndex = lightManager_.GetShadowedPointLightIndex(s);
            const bool occluded = lightIndex < pointLightOccluded_.size() && pointLightOccluded_[lightIndex];
            SceneView* faces = &shadowViews_.PointViews()[s * 6];
            for (std::uint32_t face = 0; face < 6u; ++face)
            {
                faces[face].cacheMode = 0;
                faces[face].cachePage = -1;
                faces[face].directRead = false;
            }
            if (!cacheOn || occluded) { continue; }
            const std::uint32_t faceRes = faces[0].shadowRes;
            if (localShadowCache_.PointBlockFits(faceRes))
            {
                // One entry for the six faces, laid out as in the atlas: face k at (k % 3, k / 3) faces from the corner.
                render::LocalShadowCache::Rect block{};
                const std::uint8_t mode = request(faces[0], render::LocalShadowCache::PointBlockId(
                    static_cast<std::uint32_t>(lightIndex)), render::PointShadowLinearDepth(), true, block);
                if (mode != static_cast<std::uint8_t>(render::LocalShadowCache::Mode::Uncached))
                {
                    for (std::uint32_t face = 0; face < 6u; ++face)
                    {
                        place(faces[face], mode, block, (face % 3u) * faceRes, (face / 3u) * faceRes);
                    }
                    if (readsDirect(faces[0]))
                    {
                        for (std::uint32_t face = 0; face < 6u; ++face) { faces[face].directRead = true; }
                        lightManager_.SetPointShadowDirect(lightIndex, { block.page, block.x, block.y });
                        localShadowCache_.CountDirect();
                    }
                    continue;
                }
                // No free 4f square for the block (a pool full of smaller pieces, half of it free): the six faces as
                // squares of their own, copied in -- cached all the same, only not read directly.
            }
            // A face too big for a block in a page (4f > a page), or no room for one: six squares of their own, copied in.
            for (std::uint32_t face = 0; face < 6u; ++face)
            {
                render::LocalShadowCache::Rect rect{};
                place(faces[face], request(faces[face], render::LocalShadowCache::PointId(static_cast<std::uint32_t>(lightIndex), face),
                                           render::PointShadowLinearDepth(), false, rect), rect, 0u, 0u);
            }
        }
    }

    // Local-light plan S10c: which light each VSM view holds this frame -- the pool moves a light's pages with it when its
    // slot changes (VirtualShadowMap::PlanViewRemap) instead of re-rendering them.
    {
        // Plan S9e: as many views as the pool was made for (a light past them waits for the re-create).
        VirtualShadowMap::ViewOwners owners = VirtualShadowMap::NoViewOwners(vsm_.ViewCount());
        const auto own = [&owners](std::uint32_t view, std::uint32_t owner) { if (view < owners.size()) { owners[view] = owner; } };
        for (size_t s = 0; s < shadowedSpotCount; ++s)
        {
            own(vsm::SpotView(static_cast<std::uint32_t>(s)),
                VirtualShadowMap::SpotViewOwner(static_cast<std::uint32_t>(lightManager_.GetShadowedSpotLightIndex(s))));
        }
        for (size_t s = 0; s < shadowedPointCount; ++s)
        {
            const auto light = static_cast<std::uint32_t>(lightManager_.GetShadowedPointLightIndex(s));
            for (std::uint32_t face = 0; face < 6u; ++face)
            {
                own(vsm::PointFaceView(static_cast<std::uint32_t>(s) * 6u + face), VirtualShadowMap::PointFaceViewOwner(light, face));
            }
        }
        vsm_.SetViewOwners(owners);
    }

    prepareViewsSetupScope.reset();

    // main + shore + shore SDF + cascades + the shadowed spots' views + 6 cube faces per shadowed point -- plan S9e: as
    // many as the view capacity holds, so a growing vector (tc::inl_vector does not grow: past its capacity Release
    // wrote over the stack once, S10b).
    std::vector<SceneView*>& viewsToCull = viewsToCull_;
    viewsToCull.clear();
    viewsToCull.reserve(3u + static_cast<size_t>(kCascades) + lightManager_.GetShadowedSpotCount() +
                        6u * lightManager_.GetShadowedPointCount());
    auto enqueueView = [this, camMask, &viewsToCull](SceneView& view)
    {
        if (view.type == SceneView::Type::Shadow && !view.frustum.IsValid())
        {
            PrepareViewQueue(view, camMask);
            return;
        }
        // Plan S7's direct read: the receivers read the cache pool and every shadow pass skips the view
        // (SceneRenderer_Shadows: `if (view.directRead) continue`), so its queue -- cull, LODs, instanced buckets --
        // was built for nothing, per face, per frame: ~200 views on wind_test, 82 ms of worker time a Debug frame. An
        // empty queue instead, as for an invalid frustum; PrepareOpaqueDrawStates then registers none of its objects,
        // which no pass would have drawn.
        if (view.type == SceneView::Type::Shadow && view.directRead)
        {
            view.queue.Clear();
            return;
        }
        viewsToCull.push_back(&view);
    };

    {
        CPU_SCOPE(ProfilerScopes::kPrepareViewsBuildList);
        if (shoreViewPtr)
        {
            enqueueView(*shoreViewPtr);
        }
        if (frameData_.skyOcclusion && skyOcclusion_.DrawThisFrame())
        {
            // Only on a frame that draws the map: the rest of the time its views have no work.
            enqueueView(skyOcclusion_.ObjectsView());
            enqueueView(skyOcclusion_.TerrainView());
        }
        if (oceanSimulation && oceanSimulation->ShouldBuildShoreSdf())
        {
            // Only on the frame the SDF is actually rebuilt — the rest of the time this view has no
            // work and culling it would be pure overhead.
            enqueueView(oceanSimulation->GetShoreSdfView());
        }
        for (auto& cascadeView : shadowViews_.Cascades())
        {
            enqueueView(cascadeView);
        }
        for (size_t s = 0; s < lightManager_.GetShadowedSpotCount(); ++s)
        {
            enqueueView(shadowViews_.SpotViews()[s]);
        }
        for (size_t v = 0; v < lightManager_.GetShadowedPointCount() * 6; ++v)
        {
            enqueueView(shadowViews_.PointViews()[v]);
        }
    }

    // Camera preparation was published before cascade/light-view setup. Publish the remaining
    // independent views now; while waiting for the camera task, main may help drain these tasks.
    // One task per view to cull (plan S9e: as many as there are views).
    std::vector<TaskSystem::TaskHandle>& viewTasks = viewTasks_;
    viewTasks.clear();
    viewTasks.reserve(viewsToCull.size());
    {
        CPU_SCOPE(ProfilerScopes::kPrepareViewsDispatch);
        for (SceneView* view : viewsToCull)
        {
            if (!view) { continue; }
            // Frame tasks too, as the camera's above (--crash-selftest=view-throw@other).
            if (TaskSystem::TaskHandle task = tasks.CreateTask([this, view, camMask]()
                {
                    diag::ThrowSelfTestPoint(diag::ThrowSite::ViewOther);
                    PrepareViewQueue(*view, camMask);
                }, 0, TaskSystem::FailurePolicy::Frame, ViewCullTaskName(view->type)))
            {
                tasks.Submit(task);
                viewTasks.push_back(task);
            }
        }
    }

    {
        CPU_SCOPE(ProfilerScopes::kPrepareViewsJoin);
        tasks.Wait(mainViewTask);
        tasks.Release(mainViewTask);
        for (TaskSystem::TaskHandle& task : viewTasks)
        {
            tasks.Wait(task);
            tasks.Release(task);
        }
    }
}

ShadowPlanInputs Scene::ShadowInputs(const Camera& camera)
{
    // R6.1: the render half's state the shadow view planner reads. Its S14 readout may borrow LAST frame's raw object
    // pointers from the cascade queues only while the object set and layer mask still match that queue: editor placement
    // removes the preview and bumps the set version before the planning; without this gate the previous queue retains a
    // dangling pointer until PrepareViewQueue clears it later in this same frame.
    ShadowPlanInputs in{ frameData_, renderCamera_, renderSun_, renderWind_, renderCascadeConfig_, shadowGpu_, sdsm_,
                         objects_, renderStaticSetVersion_ };
    in.previousCascadeQueuesCurrent = renderQueueSourcesValid_ && renderQueueSourceVersion_ == renderStaticSetVersion_ &&
                                      renderQueueSourceMask_ == camera.GetRenderLayerMask();
    return in;
}

bool Scene::ReconcileShadowCapacity(Renderer* renderer)
{
    // Local-light plan S9e: UE shadows every visible movable light that passes its fade (ShadowSetup.cpp:6411-6482) --
    // no count. Ours are a CAPACITY of view slots: when the last selection (LightManager::SelectShadowedSpots / Points)
    // wanted more than it holds, the capacity doubles (up to the ceiling, a memory guard) HERE, at the GPU-idle point,
    // and everything sized by views is made again for it: the scene's views, the cull's rows (RebuildShadowCasters),
    // VSM's page table and records, SDSM's frustum buffer. The frame that asked shadowed what fitted; the next one all.
    const unsigned spots = ShadowViewPlanner::GrowCapacity(render::g_shadowSpotCapacity, lightManager_.WantedShadowedSpots(),
                                                           render::kShadowSpotsCeiling); // R6.1: the planner's rule
    const unsigned points = ShadowViewPlanner::GrowCapacity(render::g_shadowPointCapacity, lightManager_.WantedShadowedPoints(),
                                                            render::kShadowPointsCeiling);
    if (lightManager_.WantedShadowedSpots() > spots || lightManager_.WantedShadowedPoints() > points)
    {
        LOG_WARNING_ONCE(logging::LogCategory::RenderShadow,
                         "local shadows: {} spot / {} point lights want a shadow, past the capacity ceiling {} / {} -- "
                         "the rest stay unshadowed", lightManager_.WantedShadowedSpots(), lightManager_.WantedShadowedPoints(),
                         render::kShadowSpotsCeiling, render::kShadowPointsCeiling);
    }
    if (spots == render::g_shadowSpotCapacity && points == render::g_shadowPointCapacity) { return false; }

    renderer->WaitForPreviousFrame(); // nothing in flight reads a buffer re-made below
    ApplyShadowCapacity(renderer, spots, points);
    RebuildShadowCasters(*renderer, /*sameGeometry=*/true);
    return true;
}

void Scene::ApplyShadowCapacity(Renderer* renderer, unsigned spots, unsigned points)
{
    // Plan S9e: the GPU is idle (the caller's wait, or a level load). Everything sized by the view count follows the
    // capacity here, except the cull's rows: the caller rebuilds the casters (RebuildShadowCasters / the level's Rebuild).
    const unsigned oldSpots = render::g_shadowSpotCapacity;
    const unsigned oldPoints = render::g_shadowPointCapacity;
    const unsigned oldViews = render::MaxShadowViews();
    render::g_shadowSpotCapacity = spots;
    render::g_shadowPointCapacity = points;
    shadowViews_.ResizeLocalViews(spots, points); // R6.1: the planner's views and cull rows for the new capacity
    if (vsm_.IsAllocated())
    {
        vsm_.ReleaseResources();
        vsm_.EnsureResources(renderer);
    }
    if (sdsm_.Ready()) // its moments go with it; ReconcileShadowMode's moments step makes them again
    {
        sdsm_.ReleaseResources(renderer);
        if (!sdsm_.EnsureResources(renderer))
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "SDSM resources unavailable after the shadow capacity changed; "
                      "falling back to Legacy CSM");
            render::g_shadowMode = render::ShadowMode::Legacy;
        }
    }
    LOG_INFO(logging::LogCategory::RenderShadow,
             "local shadow capacity -> {} spots / {} points (was {} / {}; wanted {} / {}): {} shadow views (was {})",
             spots, points, oldSpots, oldPoints, lightManager_.WantedShadowedSpots(), lightManager_.WantedShadowedPoints(),
             render::MaxShadowViews(), oldViews);
}

void Scene::ReconcileShadowMode(Renderer* renderer)
{
    // Step 24b: make the VSM resource state match the active shadow mode. The common path is a cheap
    // bool compare; only when they disagree (a Ctrl+V toggle since last frame) do we stall to GPU idle
    // and free/allocate — the same idle-then-realloc pattern the level-load path uses. So only ONE
    // mode's shadow resources are ever resident (memory-optimal). Legacy atlas freeing is Step 24c.
    if (!renderer) { return; }
    // The mode that is ACTUALLY active, every time it changes. Without this line a run's shadow
    // mode was only inferable from side effects (an "[VSM] allocated" here, an "SDSM resources
    // ready" there) and Legacy announced nothing at all -- which is how a whole measurement
    // session compared SDSM against VSM believing it was Legacy (2026-09-14, see ShadowSettings.h).
    {
        static render::ShadowMode s_logged = static_cast<render::ShadowMode>(0xFFFFFFFFu);
        if (s_logged != render::g_shadowMode)
        {
            s_logged = render::g_shadowMode;
            LOG_INFO(logging::LogCategory::RenderShadow, "directional shadow mode: {}{}",
                     render::ShadowModeLabel(render::g_shadowMode),
                     render::g_shadowModeFromCli ? " (--shadow-mode)" : "");
        }
        static bool s_warnedCli = false;
        if (render::g_shadowModeCliUnknown && !s_warnedCli)
        {
            s_warnedCli = true;
            LOG_WARNING(logging::LogCategory::RenderShadow,
                     "--shadow-mode= had an unrecognised value and was IGNORED; running {} from "
                     "settings. Valid: legacy (or csm), sdsm, vsm",
                     render::ShadowModeLabel(render::g_shadowMode));
        }
    }
    // Plan S9e: the shadow view capacity first -- what it re-creates, the residency below then settles as usual.
    ReconcileShadowCapacity(renderer);
    const bool wantVsm = render::VsmActive();
    const bool wantAtlasFull = !wantVsm; // legacy spot/point atlases full-res only in Legacy mode
    // S15: the SDSM buffers follow the same residency rule -- resident only in the mode that uses
    // them. They are tiny (a few KB) next to the VSM pool, but the rule is what keeps "which mode
    // owns which memory" answerable, and the readout ring is a mapped READBACK page.
    const bool wantSdsm = render::SdsmActive();
    const bool vsmOk = (wantVsm == vsm_.IsAllocated());
    const bool atlasOk = (wantAtlasFull == renderer->IsLocalShadowFull());
    const bool sdsmOk = (wantSdsm == sdsm_.Ready());
    // The CSM atlas edge is a knob now (render::g_csmAtlasRes). Changing it reallocates the atlas,
    // so it is reconciled HERE, with the other shadow residency changes, rather than applied where
    // the slider moved -- one GPU-idle stall covers all of them.
    const unsigned wantAtlasRes =
        std::clamp(render::g_csmAtlasRes, render::kCsmAtlasResMin, render::kCsmAtlasResMax);
    const bool atlasResOk = (wantAtlasRes == renderer->CsmAtlasResolution());
    // Local-light shadow plan S4: the spot / point atlas slices follow render::g_localShadowMaxRes the same way.
    const unsigned wantLocalRes = RenderTargetManager::LocalShadowSliceRes(render::g_localShadowMaxRes);
    const bool localResOk = (wantLocalRes == renderer->LocalShadowMaxResolution());
    // Plan S9: the local-shadow atlas pages -- grown to what the last pack wanted (LightManager::PackShadowAtlases), up
    // to the knob; lowered only when the knob drops below what is built. VSM has no atlas to grow.
    const unsigned pageCap = std::clamp(render::g_localShadowMaxPages, 1u, render::kLocalShadowMaxPagesLimit);
    const unsigned haveSpotPages = renderer->LocalSpotShadowPages();
    const unsigned havePointPages = renderer->LocalPointShadowPages();
    const unsigned wantSpotPages = std::min(std::max(haveSpotPages, lightManager_.WantedSpotShadowPages()), pageCap);
    const unsigned wantPointPages = std::min(std::max(havePointPages, lightManager_.WantedPointShadowPages()), pageCap);
    const bool pagesOk = wantVsm || (wantSpotPages == haveSpotPages && wantPointPages == havePointPages);
    // S16: the EVSM moments atlas is the SAME kind of decision -- resident only while SDSM is the
    // mode AND EVSM is on, and sized to the atlas edge. It belongs here and NOWHERE ELSE, because
    // turning EVSM off FREES a texture: doing that from the per-frame update (where the toggle
    // used to be read) destroys a resource the frames in flight are still reading, which is a
    // device removal, not a leak (found 2026-09-14 by toggling the EVSM checkbox in the editor).
    const bool wantMoments = wantSdsm && render::sdsm::g_evsm;
    const bool momentsOk = (wantMoments == sdsm_.MomentsReady()) &&
                           (!wantMoments || sdsm_.MomentsRes() == wantAtlasRes);
    // Plan S7: the local shadow cache's pool -- grown to what the cache asked for (LocalShadowCache::WantedPages), up to
    // its knob, in pages of the spot page's size; freed with the cache off and in VSM (no atlas lights to cache). It grows
    // by DOUBLING: every growth is a GPU-idle point, and wind_test's "Lamps" grew it a page at a time, 1 -> 8 in eight
    // stalls (2026-10-30).
    const bool wantCache = !wantVsm && render::g_localShadowCache && render::g_localShadowCompact;
    const unsigned cacheCap = std::clamp(render::g_localShadowCacheMaxPages, 1u, render::kLocalShadowCacheMaxPagesLimit);
    const unsigned haveCachePages = localShadowCache_.ResidentPages();
    const unsigned askedCachePages = localShadowCache_.WantedPages();
    const unsigned wantCachePages = wantCache ? std::min(askedCachePages > haveCachePages ? std::max(askedCachePages, 2u * haveCachePages)
                                                                                         : haveCachePages, cacheCap)
                                              : 0u;
    const bool cacheOk = wantCachePages == haveCachePages &&
                         (wantCachePages == 0u || localShadowCache_.PageSize() == wantLocalRes);
    if (vsmOk && atlasOk && sdsmOk && atlasResOk && momentsOk && localResOk && pagesOk && cacheOk) { return; } // all in sync — the common per-frame path
    // Reconciled independently so a resize (which rebuilds the atlases full-res) also self-corrects.
    renderer->WaitForPreviousFrame(); // GPU idle before freeing/allocating shadow resources
    if (!vsmOk) { if (wantVsm) { vsm_.EnsureResources(renderer); } else { vsm_.ReleaseResources(); } }
    if (!atlasOk) { renderer->SetLocalShadowResidency(wantAtlasFull); }
    // Plan S9: one atlas page of each pool in MB (R16): a spot page res^2, a point page 3 x 2 faces of res / 2.
    const auto pageMb = [](unsigned res, unsigned spotPages, unsigned pointPages)
    {
        const double spot = static_cast<double>(res) * res;
        const double point = 6.0 * (res / 2u) * (res / 2u);
        return (spot * spotPages + point * pointPages) * 2.0 / (1024.0 * 1024.0);
    };
    if (!localResOk)
    {
        renderer->SetLocalShadowMaxResolution(wantLocalRes);
        LOG_INFO(logging::LogCategory::RenderShadow, "local shadow max resolution -> {} (spot pages {}^2, point pages {}x{}: {:.1f} MB, R16)",
                 wantLocalRes, wantLocalRes, 3u * (wantLocalRes / 2u), wantLocalRes,
                 pageMb(wantLocalRes, renderer->LocalSpotShadowPages(), renderer->LocalPointShadowPages()));
    }
    if (!cacheOk) // S7 (the GPU is idle here, the rule for freeing anything a frame in flight may still read)
    {
        // More pages of the same size keep what is cached (the old pages copied over): re-creating the pool threw every
        // light's statics away -- the Lamps' 61 views refilled at once, 0.25-0.5 ms in one frame. Anything else re-creates.
        bool grown = false;
        if (wantCachePages > haveCachePages && haveCachePages != 0u && localShadowCache_.PageSize() == wantLocalRes)
        {
            UploadBatch copy;
            if (copy.Begin(renderer))
            {
                grown = localShadowCache_.GrowPool(renderer->GetDevice(), renderer->Declarations(), wantCachePages,
                                                   copy.CommandList(), copy.KeepAlive());
                copy.SubmitAndWait(renderer);
            }
        }
        if (!grown) { localShadowCache_.EnsurePool(renderer->GetDevice(), renderer->Declarations(), wantLocalRes, wantCachePages); }
    }
    if (!pagesOk)
    {
        renderer->SetLocalShadowPages(wantSpotPages, wantPointPages);
        LOG_INFO(logging::LogCategory::RenderShadow, "local shadow atlas pages -> {} spot + {} point (cap {}): {:.1f} MB, R16",
                 wantSpotPages, wantPointPages, pageCap, pageMb(wantLocalRes, wantSpotPages, wantPointPages));
    }
    if (!atlasResOk)
    {
        renderer->SetCsmAtlasResolution(wantAtlasRes);
        // The cascade tile pyramids are sized off the tile's CONTENT rect; EnsureResources
        // recreates them (and drops the history) when it changes, but it only ever sees the size
        // through SetSdsmHzbViews / SetCascadeHzbViews next frame. Nothing to do here -- noted so
        // the absence of a call is deliberate rather than forgotten.
        LOG_INFO(logging::LogCategory::RenderShadow, "CSM atlas resolution -> {} ({} MB, R16)",
                 wantAtlasRes,
                 (static_cast<double>(wantAtlasRes) * wantAtlasRes * 2.0) / (1024.0 * 1024.0));
    }
    if (!sdsmOk)
    {
        if (wantSdsm)
        {
            if (!sdsm_.EnsureResources(renderer))
            {
                // The mode cannot run. Say so once and fall back to Legacy rather than leaving a
                // mode selected whose passes are all absent -- that reads as "shadows vanished".
                LOG_ERROR(logging::LogCategory::RenderShadow,
                          "SDSM resources unavailable; falling back to Legacy CSM");
                render::g_shadowMode = render::ShadowMode::Legacy;
            }
        }
        else
        {
            sdsm_.ReleaseResources(renderer);
        }
    }
    // AFTER the SDSM buffers: leaving the mode already released the moments with them, and
    // entering it has to have the buffers before the atlas they are derived from is worth having.
    // Re-read `wantMoments` against the mode that actually settled -- the EnsureResources failure
    // above can demote us to Legacy, and allocating 67 MB of moments for a mode we just left is
    // exactly the kind of thing a residency reconcile exists to prevent.
    {
        const bool haveMoments = render::SdsmActive() && render::sdsm::g_evsm && sdsm_.Ready();
        if (haveMoments != sdsm_.MomentsReady() || (haveMoments && sdsm_.MomentsRes() != wantAtlasRes))
        {
            if (haveMoments) { sdsm_.EnsureMoments(renderer, wantAtlasRes); }
            else             { sdsm_.ReleaseMoments(renderer); }
        }
    }
}

void Scene::Render(Renderer* renderer) {
    // Occlusion plan S0: headless visibility readout, once, past the streaming window (frame 600,
    // same reason as csm_readout). Reads LAST frame's snapshot -- complete by construction.
    if (render::g_visDumpReadout && renderer && renderer->GetTotalFrameNumber() > 600)
    {
        render::g_visDumpReadout = false;
        diag::ArtifactFile f("visibility_readout.log", diag::ArtifactMode::AtomicReplace);
        if (f)
        {
            static const char* const kNames[render::kVisibilityViews] = { "camera", "c0", "c1", "c2", "c3" };
            f.Printf("view     objectsIn  frustum  occluded  chunksIn  chunksDrawn  chunksOcc  instances   triangles    frame=%llu drawCalls=%u primitives=%llu\n",
                     static_cast<unsigned long long>(renderer->GetTotalFrameNumber()),
                     render::g_renderStats.lastDrawCalls,
                     static_cast<unsigned long long>(render::g_renderStats.lastPrimitives));
            for (unsigned v = 0; v < render::kVisibilityViews; ++v)
            {
                const render::VisibilityViewCounters& c = render::g_visibilityStats.last[v];
                f.Printf("%-8s %9u  %7u  %8u  %8u  %11u  %9u  %9u  %10llu\n", kNames[v], c.objectsIn, c.objectsFrustum,
                         c.objectsOccluded, c.chunksIn, c.chunksDrawn, c.chunksOccluded, c.instancesDrawn,
                         static_cast<unsigned long long>(c.trianglesSubmitted));
            }
            // S3a: what the camera asked the GPU last frame and what came back.
            const render::OcclusionFrameStats& os = render::g_visibilityStats.occlusionLast;
            // S5: the GPU side -- indirect G-buffer candidates the main cull deferred against last
            // frame's pyramid and how many of those pass B drew (a fading caster twice), frame N-3.
            // S5b.2: the VSM light-space two-pass -- (caster, clipmap page) pairs the scatter
            // deferred against last frame's pool pyramid, pairs pass B drew, pairs past the list.
            const auto& vh = vsm_.HzbStats();
            f.Printf("occlusion method=%u individual=%u grouped=%u dropped=%u tested=%u latency=%u entries=%u ignored=%u lightsOccluded=%u gpuOwned=%u gpuHzbDeferred=%u gpuHzbDrawnB=%u vsmHzbDeferred=%u vsmHzbDrawnB=%u vsmHzbOverflow=%u\n",
                     os.method, os.queriesIndividual, os.queriesGrouped, os.queriesDropped, os.queriesTested,
                     os.latencyFrames, os.historyEntries, os.ignoredResults, os.lightsOccluded, os.gpuOwnedObjects,
                     shadowGpu_.CamHzbDeferred(), shadowGpu_.CamHzbDrawnB(), vh[0], vh[1], vh[2]);
        }
    }
    if (!renderer) {
        return;
    }
    TC_CHECK_RENDER("Scene::Render");
    if (!syncedThisFrame_)
    {
        // Every frame loop calls Sync before Render (App::Run, the scene-stress harness). One that does not would
        // render last frame's settings: say so, once, and hand over here.
        LOG_ERROR_ONCE(logging::LogCategory::Scene, "Scene::Render without Scene::Sync this frame -- synced late");
        threading::RoleScope syncRole(threading::ThreadRole::Sync);
        Sync(renderer);
    }
    syncedThisFrame_ = false;
    CPU_SCOPE(ProfilerScopes::kSceneRender);

    ReconcileShadowMode(renderer); // Step 24b: apply a pending Legacy<->VSM switch (GPU-idle free/alloc)
    ReconcileShadowLodCurve(renderer); // apply pending shadow-LOD curve changes (GPU-idle caster rebuild)
    ReconcileSkyOcclusion(renderer);   // the sky-from-above map's size, or freeing it (render::deferred)
    // The GI probe volumes a graphics level (or a knob) turned off give their memory back (GPU idle).
    sceneRenderer_.ReleaseUnwantedGi(renderer, frameData_.settings.giEnabled && renderer->IsRaytracingSupported(),
                                     frameData_.settings.giFarCascade);
    // Likewise the bloom's convolution grids (3 x 32 MB at 1440p, 3 x 64 MB at 4K) while the pyramid bloom runs or none,
    // and the clouds' volumes while the clouds are off (docs/scalability_plan.md §4.3).
    renderer->SetBloomFftResidency(frameData_.settings.bloom.enabled && frameData_.settings.bloom.method == 1u);
    sceneRenderer_.ReleaseUnwantedClouds(renderer, frameData_.settings.volumetricCloud.enabled);

    if (renderer->ConsumeMaterialHotReloadFlag())
    {
        sceneRenderer_.RefreshMaterialHandles(renderer, objects_, skyBox_.get());
        // THE MEGA BUFFER IS WHY THIS IS A FULL REBUILD AND NOT `InvalidateGroupMaterials`.
        //
        // That call only set `rebuildPending_`, so the next `UpdateForFrame` ran
        // `ShadowGpuData::Rebuild` -- which CLEARS `megaReady_` and does not put it back, because
        // the consolidated VB/IB is only ever built by `EnsureMegaBuffer`, and that needs a
        // GPU-idle upload batch. `VirtualShadowMap::RecordPageRender` then falls back to the
        // per-page loop (pool pages x mesh-groups x bind VB/IB + ExecuteIndirect) and STAYS there
        // for the rest of the session. Measured on `wind_test` by touching an ocean shader
        // mid-run: Pass_VsmPageRender CPU 0.47 ms -> 5.24 ms, with the engine's own
        // "[VSM] single-draw page render OFF: mega buffer unavailable" warning one millisecond
        // after the rebuild line.
        //
        // This is the same failure the editor's spawn preview hit (ViewportGizmo) and the
        // shadow-LOD-bias slider hit (ReconcileShadowLodCurve); both already route through
        // RebuildShadowCasters, which is called from THIS phase of the frame two lines above.
        // A shader reload is the third trigger, and it was the one nobody wired.
        //
        // The ForceContentRefreshNextFrame inside is not incidental either: the cached VSM pages
        // were rasterised with the shaders that just changed.
        RebuildShadowCasters(*renderer);
    }

    {
        CPU_SCOPE(ProfilerScopes::kSceneRenderLightCache);
        lightManager_.UpdateSpotLightCache();
        // Plan S10e: which lights' shadows moved (UE's light mobility) -- before PrepareViews builds their views.
        lightManager_.UpdateLightMobility(renderer->GetTotalFrameNumber(), vsm::g_lightActiveFrameCount);
    }

    // Rung 0 / Steps 1-2: refresh the persistent per-caster shadow buffers (instance + bounds),
    // re-uploading only the movers' entries into this frame's ring region. Pure CPU write into
    // mapped upload memory; no consumer yet.
    shadowGpu_.UpdateForFrame(renderer, objects_);
    // A caster-set change mid-session (the editor's setEnabled / spawn) was just rebuilt, and that drops the mega VB/IB:
    // without it the local views lose their compaction and the Tessy atlas path, VSM its single-draw page render.
    if (shadowGpu_.TakeMegaRebuildRequest()) { RebuildShadowMegaBuffer(*renderer); }
    {
        CPU_SCOPE(ProfilerScopes::kSceneRenderPolls);
        shadowGpu_.PollValidation(renderer); // Step 4: one-shot GPU-vs-CPU cull-count check when ready
        shadowGpu_.PollHzbStats(renderer);   // S5b: the cascade HZB cull's counters of frame N - 3
        shadowGpu_.PollTessyAtlasStats(renderer); // Tessy N4: the atlas cluster cull's counters of frame N - 3
        vsm_.PollPageRequestDebug(renderer);  // Step 19: one-shot page-request count log when ready
        vsm_.UpdateDynamicResolution(renderer); // S10e: the page pool's load of frame N - kFrameCount -> the local mip bias
        vsm_.PollHzbStats(renderer);          // S5b.2: the light-space two-pass counters of frame N - 3
    }

    // Occlusion plan S4: does the indirect G-buffer run this frame? Decided here, BEFORE the
    // camera prepare (which drops eligible objects from its CPU buckets on such frames) and the
    // registry's per-frame tables (which fill the camera flags only then). The registry was
    // refreshed above, so its eligibility is this frame's.
    const bool gbufferIndirect = render::g_indirectGBufferEnabled && shadowGpu_.GBufferIndirectReady();
    shadowGpu_.SetGBufferIndirect(gbufferIndirect);
    frameData_.gbufferIndirect = gbufferIndirect;
    frameNumber_ = renderer->GetTotalFrameNumber(); // the stamp the camera prepare puts on visible objects

    PrepareViews(renderer);

    // Per-caster shadow LOD: publish THIS frame's receiver LODs (per chunk / per instance, inside
    // PrepareViews above) as the shadow caster overrides — after PrepareViews on purpose, so the
    // caster can never lag the receiver by a frame at a LOD transition. S4: the same walk fills
    // the camera flags + crossfade tables from the verdicts PrepareViews just produced.
    {
        CPU_SCOPE(ProfilerScopes::kRefreshCasterLods);
        shadowGpu_.RefreshCasterLods(renderer, objects_, renderCamera_.GetPosition(), occlusionHistory_.Enabled(),
                                     renderer->GetTotalFrameNumber());
    }

    // LOD selection debug view (dev window "LOD" tab, or --set=lod.debug). Emitted HERE because
    // this is the one point where both halves of what it shows are valid: this frame's tiers are
    // final (SelectLods ran inside PrepareViews just above) and the HUD text buffer is still open
    // for writing (AppController::WaitForHudBuild returned before Scene::Render was called, and
    // TextManager::Build runs later, inside SceneRenderer::Render). Off by default; the body
    // early-outs on the mode before touching anything.
#if WITH_EDITOR
    // The editor selection, resolved from ids to pointers here because objectIds_ is what holds the
    // lockstep mapping. Kept out of SceneFrameData: this is a same-frame main-thread read, and the
    // frame data is for what the render threads consume.
    // Only the LOD view's "selected only" filter reads the pointers, and the view is off by default: resolved every frame
    // regardless, the walk below (a binary search per object) was 0.5 ms of the render thread's Debug frame on
    // wind_test with nothing selected (2026-11-03).
    if (render::g_lodDebugMode != render::LodDebugMode::Off)
    {
        CPU_SCOPE(ProfilerScopes::kSceneEditorSelection);
        // One pass over the objects testing each against the sorted selection, instead of a scan
        // of every object per selected id -- the old shape was O(selection x objects) and a few
        // hundred selected meshes made it the most expensive thing in the editor frame.
        std::vector<const RenderableObjectBase*> sel;
        sel.reserve(renderSelection_.size());
        for (size_t o = 0; o < objectIds_.size() && o < objects_.size() && !renderSelection_.empty(); ++o)
        {
            if (std::binary_search(renderSelection_.begin(),
                    renderSelection_.end(), objectIds_[o]))
            {
                sel.push_back(objects_[o].get());
            }
        }
        render::DrawLodDebug(renderer, renderCamera_, objects_, sel.data(), sel.size());
    }
#else
    render::DrawLodDebug(renderer, renderCamera_, objects_);
#endif

    // Rung 0 / Step 2: upload the active shadow views' frustum planes (the per-view cull input)
    // into this frame's ring region. Fixed slot layout [cascades | spots | point-faces] so a
    // view's slot index is stable for the future cull; inactive slots pass null → zeroed.
    {
        CPU_SCOPE(ProfilerScopes::kSceneShadowViewUpload);
        constexpr size_t kCascadeSlots = static_cast<size_t>(kCascades);
        const size_t kSpotSlots = render::ShadowSpotCapacity();          // plan S9e: the capacity, not a cap
        const size_t kPointFaceSlots = 6u * render::ShadowPointCapacity();
        constexpr size_t kClipmapSlots = vsm::kNumClipmapLevels; // Step 24e: directional clipmap cull views
        const size_t maxViews = render::MaxShadowViews(); // the indirect buffers (ShadowGpuData) size per view by it
        static_assert(kCascadeSlots == render::kShadowViewCascades && kClipmapSlots == render::kShadowViewClipmapLevels,
                      "the cull layout's cascades / clipmap are VSM's");
        // Local-light shadow plan S9: [cascades | 8 spots | 24 point faces | clipmap | S - 8 more spots | 6 (P - 4) more
        // faces]; every slot index comes from render::ShadowView*Slot, never from a running count. Plan S10b: VSM's views
        // are these slots minus the cascades (vsm::SpotView / PointFaceView / ClipmapView: rung0View = view + 4).
        // Occlusion plan S4: one more slot past the shadow views -- the CAMERA frustum, for the
        // registry's camera row (the indirect G-buffer). Never a shadow view: the shadow cull
        // loops MaxShadowViews() and the camera branch addresses its slot by index.
        std::vector<const Frustum*>& frustums = shadowViews_.ViewFrustums();
        frustums.assign(maxViews + 1u, nullptr);
        // S15: in SDSM the four directional slots are written by Main_SdsmAnalyze from this frame's
        // depth, and Main_SdsmCull re-clears and re-fills their args rows. Uploading the Legacy
        // cascade frusta here would make the MAIN cull append every caster into rows that are
        // about to be thrown away -- so they go up as reject-all instead, exactly as the clipmap
        // slots do in Legacy mode. (Null = the reject-all sentinel; see UpdateViewFrustums.)
        const bool sdsmDirectional = render::SdsmActive();
        for (size_t i = 0; i < kCascadeSlots; ++i)
        {
            frustums[i] = sdsmDirectional ? nullptr : &shadowViews_.Cascades()[i].frustum;
        }
        const size_t spotCount = lightManager_.GetShadowedSpotCount();
        for (size_t i = 0; i < kSpotSlots; ++i)
        {
            frustums[render::ShadowViewSpotSlot(static_cast<unsigned>(i))] =
                (i < spotCount) ? &shadowViews_.SpotViews()[i].frustum : nullptr;
        }
        const size_t pointFaceCount = lightManager_.GetShadowedPointCount() * 6;
        for (size_t i = 0; i < kPointFaceSlots; ++i)
        {
            frustums[render::ShadowViewPointFaceSlot(static_cast<unsigned>(i))] =
                (i < pointFaceCount) ? &shadowViews_.PointViews()[i].frustum : nullptr;
        }
        // Step 24e: directional clipmap levels — culled ONLY in VSM mode (null = reject-all in Legacy,
        // so the cull emits zero for them and the Legacy atlas path is untouched).
        for (size_t i = 0; i < kClipmapSlots; ++i)
        {
            frustums[render::ShadowViewClipmapSlot(static_cast<unsigned>(i))] =
                render::VsmActive() ? &shadowViews_.ClipmapViews()[i].frustum : nullptr;
        }
        // S4: the camera, this frame's (PrepareViews above built it).
        const SceneView& camView = cameraView_;
        frustums[maxViews] = &camView.frustum;
        // Local-light plan S9e: each local view's light bounds, and -- atlas modes, the knob -- UE's hull of its light and
        // the camera frustum (one per light: a point light's six faces share it). VSM caches pages: no hull (UE).
        {
            const bool boundsOn = render::g_localShadowBoundsCull;
            const bool hullOn = render::g_localShadowHullCull && !render::VsmActive();
            std::fill(shadowViews_.LocalViewCullPtr().begin(), shadowViews_.LocalViewCullPtr().end(), nullptr);
            const auto fill = [&](size_t slot, const SceneView& view, const render::LocalViewCull& hull)
            {
                render::LocalViewCull& lc = shadowViews_.LocalViewCull()[slot];
                lc = hull;
                if (boundsOn)
                {
                    lc.sphere = view.lightSphere.xf();
                    lc.cone = view.lightCone.xf();
                }
                // Tessy N7e: the light and the view's texels per unit at distance 1 (the atlases' DAG cut).
                lc.dag = DirectX::XMFLOAT4(view.position.x, view.position.y, view.position.z,
                                           0.5f * static_cast<float>(view.shadowRes) * std::abs(view.proj.m.m[0][0]));
                // Tessy: what the atlas draw projects with and its viewport (SceneRenderer_Shadows: BuildShadowViewCB,
                // BindLocalShadowTarget at ShadowViewRes -- the slice's own edge when the view names none: no test then).
                lc.viewProj = (view.view * view.proj).m;
                lc.rectTexels = static_cast<float>(view.shadowRes);
                shadowViews_.LocalViewCullPtr()[slot] = &lc;
            };
            const auto buildHull = [&](const float3& light)
            {
                render::LocalViewCull hull{};
                if (hullOn)
                {
                    Math::float4 planes[Frustum::kMaxLightHullPlanes] = {};
                    const int n = std::min<int>(camView.frustum.LightViewConvexHull(light, planes),
                                                static_cast<int>(render::kLocalViewHullPlanes));
                    for (int j = 0; j < n; ++j) { hull.hull[j] = planes[j].xf(); }
                    hull.hullCount = static_cast<std::uint32_t>(n);
                }
                return hull;
            };
            for (size_t i = 0; i < kSpotSlots && i < spotCount; ++i)
            {
                fill(render::ShadowViewSpotSlot(static_cast<unsigned>(i)), shadowViews_.SpotViews()[i],
                     buildHull(shadowViews_.SpotViews()[i].position));
            }
            for (size_t i = 0; i < kPointFaceSlots && i < pointFaceCount; i += 6)
            {
                const render::LocalViewCull hull = buildHull(shadowViews_.PointViews()[i].position);
                for (size_t face = 0; face < 6 && i + face < pointFaceCount; ++face)
                {
                    fill(render::ShadowViewPointFaceSlot(static_cast<unsigned>(i + face)), shadowViews_.PointViews()[i + face], hull);
                }
            }
            // UE r.Shadow.RadiusThreshold x LODDistanceFactor (the player camera's FOV / 90, UE LocalPlayer.cpp) -- the
            // horizontal FOV, which is UE's; 1 / proj[0][0] = tan(hfov / 2), unchanged by the jitter.
            const float m00 = camView.proj.m.m[0][0];
            const float hfovDeg = m00 > 1e-6f ? DirectX::XMConvertToDegrees(2.0f * std::atan(1.0f / m00)) : 90.0f;
            const float share = std::max(render::g_localShadowRadiusThreshold, 0.0f) * (hfovDeg / 90.0f);
            shadowGpu_.SetLocalRadiusThreshold(camView.position, share * share);
        }
        // render::g_localShadowWindRange: the slots of the lights past it (Scene::PrepareViews' verdicts). Tessy N4: and
        // the slots that draw their static rows this frame -- every view but a cache hit (PrepareViews' cacheMode).
        {
            ShadowGpuData::ViewMask rigid{};
            ShadowGpuData::ViewMask staticDrawn{};
            const auto mark = [](ShadowGpuData::ViewMask& mask, unsigned slot)
            {
                if (slot / 32u < mask.size()) { mask[slot / 32u] |= 1u << (slot % 32u); }
            };
            const auto drawsStatics = [](const SceneView& view)
            {
                return view.cacheMode != static_cast<std::uint8_t>(render::LocalShadowCache::Mode::Hit);
            };
            for (size_t i = 0; i < kSpotSlots && i < spotCount; ++i)
            {
                const unsigned slot = render::ShadowViewSpotSlot(static_cast<unsigned>(i));
                if (shadowViews_.SpotViews()[i].windRigid) { mark(rigid, slot); }
                if (drawsStatics(shadowViews_.SpotViews()[i])) { mark(staticDrawn, slot); }
            }
            for (size_t i = 0; i < kPointFaceSlots && i < pointFaceCount; ++i)
            {
                const unsigned slot = render::ShadowViewPointFaceSlot(static_cast<unsigned>(i));
                if (shadowViews_.PointViews()[i].windRigid) { mark(rigid, slot); }
                if (drawsStatics(shadowViews_.PointViews()[i])) { mark(staticDrawn, slot); }
            }
            shadowGpu_.SetLocalWindRigid(rigid);
            shadowGpu_.SetLocalStaticDrawn(staticDrawn);
        }
        shadowGpu_.UpdateViewFrustums(renderer, frustums.data(), frustums.size(), shadowViews_.LocalViewCullPtr().data());
        // Plan S9: the cull walks VSM's prefix and, past it, only up to the last occupied extra slot.
        {
            unsigned activeViews = render::kVsmShadowViews;
            if (spotCount > LightManager::kVsmShadowedSpotLights)
            {
                activeViews = std::max(activeViews, render::ShadowViewSpotSlot(static_cast<unsigned>(spotCount) - 1u) + 1u);
            }
            if (pointFaceCount > 6u * LightManager::kVsmShadowedPointLights)
            {
                activeViews = std::max(activeViews,
                                       render::ShadowViewPointFaceSlot(static_cast<unsigned>(pointFaceCount) - 1u) + 1u);
            }
            shadowGpu_.SetActiveViewCount(activeViews);
        }

        // Occlusion plan S5b: this frame's cascade light matrices for the light-space HZB cull
        // (last frame's become `prev`), and whether the cull should test at all: the knob, the
        // Legacy atlas (VSM never takes it: its pages have their own S5b.2), and a sun to cast.
        {
            std::array<Math::mat4, kCascades> lightViewProj{};
            for (int c = 0; c < kCascades; ++c)
            {
                lightViewProj[c] = frameData_.cascades.lightView[c] * frameData_.cascades.lightProj[c];
            }
            const auto& D = renderer->GetDeferredForFrame();
            const UINT tile = D.shadowRes / 2u;
            const UINT border = render::kCascadeAtlasBorder;
            const UINT content = (tile > 2u * border) ? (tile - 2u * border) : tile;
            const bool wantOn = renderCascadeConfig_.hzbCull && !render::VsmActive() && render::g_indirectShadowsEnabled &&
                                shadowViews_.Cascades()[0].frustum.IsValid();
            shadowGpu_.SetCascadeHzbViews(renderer, lightViewProj.data(), renderer->GetTotalFrameNumber(), wantOn, content);
        }

        // S15: everything the GPU analysis needs that only the CPU knows. AFTER UpdateViewFrustums,
        // because the analyze pass copies the non-directional slots out of THIS frame's ring region
        // and the handle moves with the region.
        {
            CPU_SCOPE(ProfilerScopes::kSceneSdsmFrameParams); // its own block: CPU_SCOPE names one variable per block
            shadowViews_.UpdateSdsmFrameParams(ShadowInputs(renderCamera_), renderer); // R6.1
        }
    }

    sceneRenderer_.Render(renderer, frameData_);
}

void Scene::Clear()
{
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::Clear");
    sceneRenderer_.Reset();
    skyAtmosphere_ = {};
    frameData_ = SceneFrameData{}; // drop pointers into objects we are about to destroy
    lightManager_.Reset();
    shadowGpu_.Reset(); // drop CPU caster state; GPU buffers retained
    shadowCasterSource_.Clear();
    cameraObjectSource_.Clear();
    renderQueueSourcesValid_ = false;
    vesselSpray_.Clear(); // its emitters are objects_ below
    if (gameForget_) { gameForget_(); } // R6.2a: the race's boats were objects_ too (game::PlayWorld::Forget)
    objects_.clear();
    physics_.ForgetRemoved(); // their entries with them (RemoveEditorObject says why): none outlives its object
    shoreAreaValid_ = false; // the next level's terrain sits somewhere else
    skyOcclusion_.Invalidate(); // ...and its world from above is the old level's
#if WITH_EDITOR
    objectIds_.clear();
    selectedEditorObjectIds_.clear();
    selectionOutlineRadius_ = 1;
#endif
    cameraView_.queue.Clear();
    for (auto& view : shadowViews_.Cascades())
    {
        view.queue.Clear();
    }
    occlusionHistory_.Reset(); // S3a: the keys are object pointers of the level being dropped
    skyBox_.reset();
}
