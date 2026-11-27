#include "rendering/scene/RenderScene.h"
#include "core/logging/Log.h"

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
#include "rendering/core/GpuIdlePoint.h" // R8.1: the frame's one GPU-idle point
#include "rendering/core/RenderConstants.h" // g_cleanCapture
#include "rendering/core/UploadBatch.h"
#include "rendering/meshes/LodSelect.h" // render::g_shadowLodBias (shadow caster LOD)
#include "rendering/debug/LodDebugView.h" // render::DrawLodDebug (LOD selection debug view)
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/InstancedDrawBatch.h" // S14 readout: count a batch's members, not the batch
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/core/VisibilityStats.h" // occlusion plan S0: per-view visibility counters
#include "rendering/core/RenderStats.h"     // occlusion plan S0: draw/primitive totals in the readout
#include "core/diagnostics/ArtifactWriter.h" // S7: headless cascade readout dump
#include "core/diagnostics/BootProfile.h"    // game thread plan G10d: the level load's phases
#include "ocean/OceanSimulation.h"
#include "ocean/OceanRenderable.h"
#include "core/task/TaskSystem.h"
#include "core/diagnostics/ThrowSelfTest.h" // --crash-selftest=view-throw: a culling task throws once
#include "core/threading/ThreadRole.h" // game thread plan: who may touch what, and when
#include "core/threading/Knob.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/math/AABB.h"
#include "core/math/Frustum.h"
#include "core/containers/inl_vector.h"

RenderScene::RenderScene(std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
#if WITH_EDITOR
                         const std::vector<std::uint64_t>& objectIds,
#endif
                         LightManager& lights, const std::unique_ptr<Skybox>& sky)
    : objects_(objects)
#if WITH_EDITOR
    , objectIds_(objectIds)
#endif
    , lightManager_(lights)
    , skyBox_(sky)
{
}

OceanRenderable* RenderScene::FindOcean(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects)
{
    for (const std::unique_ptr<RenderableObjectBase>& obj : objects)
    {
        if (obj)
        {
            if (OceanRenderable* ocean = obj->As<OceanRenderable>())
            {
                return ocean;
            }
        }
    }
    return nullptr;
}

void RenderScene::BeginLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList,
                                 std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    BOOT_SCOPE("SceneResources::Finalize");
    sceneRenderer_.FinalizeLevelLoad(renderer, objects_, uploadCmdList, uploadKeepAlive, skyBox_.get());
}

void RenderScene::FinishLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList)
{
    // Local-light plan S9e: the initial shadow capacity for a fresh level (Scene::FinalizeLevelLoad says why).
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
    // Rung 2 (Step 24b): allocate the persistent VSM page pool + page table only when VSM is the
    // active mode — Legacy mode keeps ZERO VSM resources resident. A runtime Ctrl+V switch reconciles
    // this at GPU idle in Scene::Render. (The mega-buffer above stays built regardless — it's tiny
    // and lets a runtime switch to VSM use the fast per-page draw path immediately.)
    if (render::VsmActive()) { vsm_.EnsureResources(renderer); }
    // Fresh level = every resident VSM page is stale (same view slots, different level content).
    // Drop all mappings so the first frames re-request/re-render cleanly.
    vsm_.InvalidateAllPages();
}

void RenderScene::Clear()
{
    sceneRenderer_.Reset();
    frameData_ = SceneFrameData{}; // drop pointers into objects we are about to destroy
    shadowGpu_.Reset(); // drop CPU caster state; GPU buffers retained
    shadowCasterSource_.Clear();
    cameraObjectSource_.Clear();
    renderQueueSourcesValid_ = false;
    shoreAreaValid_ = false; // the next level's terrain sits somewhere else
    skyOcclusion_.Invalidate(); // ...and its world from above is the old level's
    cameraView_.queue.Clear();
    for (auto& view : shadowViews_.Cascades())
    {
        view.queue.Clear();
    }
    occlusionHistory_.Reset(); // S3a: the keys are object pointers of the level being dropped
    oceanSimulation_ = nullptr; // Sync hands the next one over
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
            const RenderableObject* ro = obj->As<RenderableObject>();
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

void RenderScene::ApplyStaticSet(std::uint32_t staticSetVersion)
{
    renderStaticSetVersion_ = staticSetVersion;

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

void RenderScene::RebuildShadowCasters(Renderer& renderer, GpuIdlePoint& idle, bool sameGeometry)
{
    // Rebuild the caster data + consolidated mega VB/IB on a fresh GPU-idle upload batch (the meshes'
    // buffers have decayed to COMMON, so EnsureMegaBuffer's implicit-promotion copies are valid).
    // Mirrors FinalizeLevelLoad. The Rebuild pre-empts the per-frame UpdateForFrame rebuild (counts
    // already match), so there is no double work. Used by the editor caster-set refresh and the
    // shadow-LOD-bias change — both need the mega buffer (only ever built here) regenerated.
    idle.Ensure(); // R8.1: no in-flight frame references the old mega buffers before we free them (the frame's one wait)
    UploadBatch uploads;
    if (!uploads.Begin(&renderer)) { return; }
    shadowGpu_.Rebuild(&renderer, objects_, sameGeometry);
    shadowGpu_.EnsureMegaBuffer(&renderer, uploads.CommandList());
    uploads.SubmitAndWait(&renderer);
    idle.NoteIdle(); // its wait covered everything handed out: the next need of the frame finds the GPU idle
    // Content (materials/geometry) may have changed without a transform change. Keep the next VSM
    // frame from reusing cached pages rendered with the previous descriptors / previous LOD.
    shadowGpu_.ForceContentRefreshNextFrame();
}

void RenderScene::RebuildShadowMegaBuffer(Renderer& renderer)
{
    // UpdateForFrame already waited for the GPU and rebuilt the casters; the mesh buffers have decayed to COMMON, so the
    // batch's implicit-promotion copies are valid, as in RebuildShadowCasters.
    UploadBatch uploads;
    if (!uploads.Begin(&renderer)) { return; }
    shadowGpu_.EnsureMegaBuffer(&renderer, uploads.CommandList());
    uploads.SubmitAndWait(&renderer);
}

bool RenderScene::SkyOcclusionWanted(const Renderer* renderer) const
{
    // Wanted while the RT sky visibility is not traced -- the setting off, or no RT hardware (SceneRenderer::DecideFrame's
    // decisions_.skyVis, less its AS-failure term: a frame whose AS failed has no map yet and keeps the open sky).
    const SceneRenderSettings& rs = frameData_.settings;
    return rs.skyVisFallback && !(rs.skyVisEnabled && renderer && renderer->IsRaytracingSupported());
}

void RenderScene::ReconcileSkyOcclusion(Renderer* renderer)
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

void RenderScene::ReconcileShadowLodCurve(Renderer* renderer, GpuIdlePoint& idle)
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
    RebuildShadowCasters(*renderer, idle);
}

void RenderScene::PrepareViewQueue(SceneView& view, uint32_t cameraLayerMask)
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

void RenderScene::ApplyOcclusion(SceneView& view, std::uint32_t& objectsOccluded, std::uint32_t& chunksOccluded)
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
            RenderableObject* ro = obj->As<RenderableObject>();
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

void RenderScene::ConsiderLightOcclusion(const Frustum& cameraFrustum)
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

void RenderScene::SyncCameraView()
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

int RenderScene::VisibilitySlotFor(const SceneView& view) const
{
    if (&view == &cameraView_) { return static_cast<int>(render::kVisibilityViewCamera); }
    const SceneView* first = shadowViews_.Cascades().data();
    if (&view >= first && &view < first + shadowViews_.Cascades().size())
    {
        return static_cast<int>(render::kVisibilityViewCascade0 + (&view - first));
    }
    return -1;
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

void RenderScene::PrepareViews(Renderer* renderer)
{
    CPU_SCOPE(ProfilerScopes::kPrepareViews);
    if (!renderer)
    {
        return;
    }
    // The LOD estimate projects each level's deviation onto the screen it is seen on (LodSelect.h).
    render::g_lodOutputWidthPx = static_cast<float>(std::max(renderer->GetWidth(), 1u));

    OceanSimulation* oceanSimulation = oceanSimulation_;
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
    frameData_.ocean = FindOcean(objects_); // caustics source for the deferred lighting pass
    frameData_.oceanSimulation = oceanSimulation_; // R6.3: the renderer reads it here, not from Systems (R6.4a: Sync hands it over)

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

ShadowPlanInputs RenderScene::ShadowInputs(const Camera& camera)
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

bool RenderScene::ReconcileShadowCapacity(Renderer* renderer, GpuIdlePoint& idle)
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

    idle.Ensure(); // R8.1: nothing in flight reads a buffer re-made below (the frame's one wait)
    ApplyShadowCapacity(renderer, spots, points);
    RebuildShadowCasters(*renderer, idle, /*sameGeometry=*/true);
    return true;
}

void RenderScene::ApplyShadowCapacity(Renderer* renderer, unsigned spots, unsigned points)
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

void RenderScene::ReconcileShadowMode(Renderer* renderer, GpuIdlePoint& idle)
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
    ReconcileShadowCapacity(renderer, idle);
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
    idle.Ensure(); // R8.1: GPU idle before freeing/allocating shadow resources (the frame's one wait)
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

void RenderScene::Render(Renderer* renderer) {
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
    TC_CHECK_RENDER("RenderScene::Render"); // a late hand-over ran in Scene::Render just before
    syncedThisFrame_ = false;
    CPU_SCOPE(ProfilerScopes::kSceneRender);

    // R8.1: the frame's one GPU-idle point (GpuIdlePoint.h) -- every reconcile below that frees or re-makes what the frames
    // in flight read, and the caster set's growth in UpdateForFrame, wait through it: the first need waits, the rest find
    // the GPU idle while nothing was handed out since.
    GpuIdlePoint idle(*renderer);
    ReconcileShadowMode(renderer, idle); // Step 24b: apply a pending Legacy<->VSM switch (GPU-idle free/alloc)
    ReconcileShadowLodCurve(renderer, idle); // apply pending shadow-LOD curve changes (GPU-idle caster rebuild)
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
        RebuildShadowCasters(*renderer, idle);
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
    shadowGpu_.UpdateForFrame(renderer, objects_, idle);
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
