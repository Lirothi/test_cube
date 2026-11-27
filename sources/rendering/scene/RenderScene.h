#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/shadows/ShadowGpuData.h"
#include "rendering/shadows/LocalShadowCache.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/shadows/SdsmShadows.h"
#include "rendering/shadows/ShadowViewPlanner.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/scene/SceneRenderConfig.h"
#include "rendering/scene/SceneRenderQueue.h"
#include "rendering/scene/SceneView.h"
#include "rendering/scene/SkyOcclusionMap.h"
#include "rendering/scene/SceneFrameData.h"
#include "rendering/scene/SceneRenderer.h"
#include "rendering/view/Camera.h"
#include "rendering/wind/WindState.h"
#include "core/task/TaskSystem.h" // viewTasks_
#include "core/threading/ThreadRole.h"
#include "rendering/visibility/OcclusionHistory.h" // occlusion plan S3a
#include "rendering/visibility/OcclusionQueries.h"
#include "rendering/visibility/HzbOcclusionTester.h" // occlusion plan S3b

class Renderer;
class Skybox;
class OceanRenderable;
class OceanSimulation;
class Scene;

// Architecture plan R6.4a: the render half of the scene -- what Sync hands it (the camera, the suns, the wind, the cascade
// knobs, the editor selection, the frame's settings in frameData_) and what it keeps between frames to draw: the views
// and their queues, the shadow data (casters, VSM, SDSM, the local lights' cache), the sky-from-above map, the occlusion
// history, the scene renderer. Its functions run on the render thread (Render, PrepareViews, the GPU-idle reconciles) or
// with it parked (level load, Clear, an editor caster-set refresh).
//
// The world it draws is the game half's: the objects (their render copies, which Sync wrote), their editor ids, the
// lights (their render descs) and the sky -- references to Scene's containers, given at construction. Membership changes
// only with the render half parked (threading::ParkRenderForGame), so nothing here sees a vector being resized.
class RenderScene
{
public:
    RenderScene(std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
#if WITH_EDITOR
                const std::vector<std::uint64_t>& objectIds,
#endif
                LightManager& lights, const std::unique_ptr<Skybox>& sky);
    RenderScene(const RenderScene&) = delete;
    RenderScene& operator=(const RenderScene&) = delete;

    // The render half's camera: the game camera's pose taken at Sync, its own jitter, matrices and history.
    const Camera& RenderCamera() const { TC_CHECK_RENDER("RenderScene::RenderCamera"); return renderCamera_; }
    // The render half's sun: the scene light (with the atmosphere's transmittance) as Sync copied it.
    const DirectionalLight& RenderSun() const { TC_CHECK_RENDER("RenderScene::RenderSun"); return renderSun_; }
    const SceneFrameData& FrameData() const { TC_CHECK_RENDER("RenderScene::FrameData"); return frameData_; }

    const render::LocalShadowCache& GetLocalShadowCache() const { return localShadowCache_; } // S7 readout
    // Rung 2: the virtual-shadow-map page pool + page table (dev-window "VSM" tab reads its stats).
    const VirtualShadowMap& Vsm() const { return vsm_; }
    // S0.1: the whole cascade block, including the *Dbg diagnostics the developer window reads.
    const SceneFrameData::CascadeData& GetCascadeData() const { return frameData_.cascades; }
    // Occlusion plan S5b: the cascade HZB cull's counters for the Render tab (read-only).
    const ShadowGpuData& ShadowGpu() const { return shadowGpu_; }
    // S15: the SDSM partition readout (dev window + --sdsm-readout).
    const SdsmShadows& Sdsm() const { return sdsm_; }

    // RT reflections for glass (S15): forwarded from the scene renderer so the
    // transparent pass can bind the TLAS + gate ray tracing.
    bool IsRtReflectActive() const { return sceneRenderer_.IsRtReflectActive(); }
    // I2: after a material's GPU textures are rebuilt, drop the RT acceleration/bindless caches so
    // they re-register with the new SRVs (else the geom-info table dangles -> DEVICE_HUNG). Call
    // with the GPU idle. Cheap no-op when RT is off.
    void InvalidateRaytracing() { sceneRenderer_.InvalidateRaytracing(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtTlasSrv(UINT frameIndex) const { return sceneRenderer_.GetTlasSrvCpu(frameIndex); }

    void InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList,
                                   std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
    {
        sceneRenderer_.InitializeCommonResources(renderer, uploadCmdList, uploadKeepAlive);
    }
    // Level load, GPU idle (Scene::FinalizeLevelLoad): the scene renderer's level resources, before the objects' render
    // copies are taken...
    void BeginLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList,
                        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    // ...and after them (and the lights'): the shadow capacity back to the initial one, the casters, the mega buffer, VSM.
    void FinishLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList);
    // What Scene::Clear drops of the render half: every pointer into the objects about to be destroyed, the caster state,
    // the level's sky-from-above map and occlusion history.
    void Clear();

    // Game thread plan G4b': the render half's side of a caster-set change -- its queues dropped, its version moved.
    void ApplyStaticSet(std::uint32_t staticSetVersion);
    std::uint32_t StaticSetVersion() const { return renderStaticSetVersion_; }
    // GPU-idle rebuild of the shadow caster data + consolidated mega VB/IB (the body shared by the
    // editor caster-set refresh and shadow-LOD curve changes). Waits for the GPU, so call sparingly.
    // `sameGeometry`: only the shadow view capacity changed (ShadowGpuData::Rebuild keeps the local shadow cache).
    void RebuildShadowCasters(Renderer& renderer, bool sameGeometry = false);

    // The frame's render half (Scene::Render calls it after the hand-over).
    void Render(Renderer* renderer);

    // The world's ocean, if it has one (the objects are walked: several paths add it, see Scene::FindOceanRenderable).
    static OceanRenderable* FindOcean(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects);

private:
    // The hand-over (Scene::Sync, and the frozen-state audit it serves) is the one writer of the copies below that come
    // from the game half; nothing else of Scene reaches past the public functions above.
    friend class Scene;

    static constexpr int kCascades = SceneFrameData::kCascades;

    // The world's containers (Scene's), read by the render half; see the class comment.
    std::vector<std::unique_ptr<RenderableObjectBase>>& objects_;
#if WITH_EDITOR
    const std::vector<std::uint64_t>& objectIds_; // lockstep with objects_: the editor id of each, 0 for a runtime object
#endif
    LightManager& lightManager_;
    const std::unique_ptr<Skybox>& skyBox_;
    OceanSimulation* oceanSimulation_ = nullptr; // R6.4a: the ocean's simulation (Systems), handed over at Sync

    void ReconcileShadowMode(Renderer* renderer); // Step 24b: GPU-idle Legacy<->VSM<->SDSM resource switch
    // Plan S's fallback without RT: wanted this frame (the setting on, the RT trace not), and its textures' size / freeing
    // at the GPU-idle point before PrepareViews (skyVis.fallbackResolution).
    bool SkyOcclusionWanted(const Renderer* renderer) const;
    void ReconcileSkyOcclusion(Renderer* renderer);
    // R6.1: what the shadow view planner reads from the render half this frame (`camera`: the one it plans for).
    ShadowPlanInputs ShadowInputs(const Camera& camera);
    // The mega VB/IB alone, after ShadowGpuData::UpdateForFrame rebuilt a changed caster set (it waited for the GPU).
    void RebuildShadowMegaBuffer(Renderer& renderer);
    // Poll the live shadow-LOD bias/stride against the caster tables; rebuild on a change.
    void ReconcileShadowLodCurve(Renderer* renderer);
    // Occlusion plan S0: which render::VisibilityStats slot a view writes (camera 0, cascades
    // 1..4), or -1 for the views that have none (local lights, clipmap levels).
    int VisibilitySlotFor(const SceneView& view) const;
    // The render camera's matrices, lens and layers into the main view (architecture plan R1.4: the view was the
    // camera's own member, which its CalcMatrices wrote). Right after renderCamera_.CalcMatrices.
    void SyncCameraView();
    // Occlusion plan S3a: on the CAMERA view's prepare task, after the frustum cull and the chunk
    // masks -- consults the history for every surviving opaque object (per chunk for terrain),
    // drops the occluded ones from the visible buckets, ANDs occluded chunks out of the mask and
    // queues this frame's queries. Never for a shadow view (asserted).
    void ApplyOcclusion(SceneView& view, std::uint32_t& objectsOccluded, std::uint32_t& chunksOccluded);
    // S3a.6: one query per local light's influence volume (UE SOQ_LightInfluenceSphere). Main
    // thread, before the camera task -- the history is single-threaded and the camera task only
    // starts afterwards. Fills spotLightOccluded_ / pointLightOccluded_ (1 = its shadow pass draws
    // nothing and its lighting entry is zeroed this frame). Its casters are NOT culled.
    void ConsiderLightOcclusion(const Frustum& cameraFrustum);

    void PrepareViewQueue(SceneView& view, uint32_t cameraLayerMask);
    void PrepareViews(Renderer* renderer);

    SceneRenderer sceneRenderer_{};

    // Per-frame pass inputs, filled by Sync (the values from game state) and PrepareViews (cascade caches,
    // view/light pointers). SceneRenderer's pass bodies read from this.
    SceneFrameData frameData_{};
    bool syncedThisFrame_ = false; // Sync ran since the last Render

    // Has the ocean been told where this level's terrain sits? One sweep of objects_ per load.
    bool shoreAreaValid_ = false;
    // R6.1: the shadow views -- cascades, VSM's clipmap, the local lights', their cull -- and the planning that keeps state
    // between frames (last frame's cascade boxes, the clipmap's depth centres, the SDSM partition cache).
    ShadowViewPlanner shadowViews_{};
    // PrepareViews' working lists, kept between frames for their capacity (two allocations a frame as locals).
    std::vector<SceneView*> viewsToCull_;
    std::vector<TaskSystem::TaskHandle> viewTasks_;
    // Step 6e: the directional cascades + spot shadow views all bucketize the same
    // shadow-caster set (identical objects/mask/filter), so it's bucketized ONCE here and
    // shared — each shadow view only runs its own per-frustum Cull against it.
    SceneRenderQueue shadowCasterSource_{};
    // Classification and opaque BatchKey order do not depend on camera pose. Cache them until
    // membership/visibility/layer or the camera layer mask changes; frustum cull, transparent
    // depth sort and LOD selection still run every frame.
    SceneRenderQueue cameraObjectSource_{};
    std::uint32_t renderQueueSourceVersion_ = 0;
    std::uint32_t renderQueueSourceMask_ = 0;
    bool renderQueueSourcesValid_ = false;
    // Rung 0 / Steps 1-2: GPU-side shadow data (per-caster instance + bounds, per-view
    // frustum planes). Built at level load, maintained per frame; not yet consumed by any pass.
    ShadowGpuData shadowGpu_{};
    // Local-light plan S7: the atlas spot / point lights' cache of their static casters (pool + entries).
    render::LocalShadowCache localShadowCache_{};
    // Plan S's fallback without RT (skyVis.fallback): the world from above, drawn when the camera moved a snap step.
    SkyOcclusionMap skyOcclusion_{};
    // ...and a static caster moved since it last asked (the object sync's ParallelFor sets it, PrepareViews takes it).
    std::atomic<bool> staticCasterMoved_{ false };
    // S9e: grows the view capacity to what the last selection wanted (doubling, up to the ceiling) -- ReconcileShadowMode,
    // at its GPU-idle point. Returns whether it grew (everything sized by views has then been re-created).
    bool ReconcileShadowCapacity(Renderer* renderer);
    // S9e: sets the capacity and re-makes what is sized by it (scene views, VSM, SDSM) -- GPU idle; the caller rebuilds
    // the casters. ReconcileShadowCapacity grows through it, FinishLevelLoad resets to the initial capacity.
    void ApplyShadowCapacity(Renderer* renderer, unsigned spots, unsigned points);
    // Occlusion plan S3a: the camera's occlusion history (per primitive / per chunk), the query
    // heap that records this frame's plan and the CPU copy of the results frame N - latency read
    // at the top of PrepareViews (kept alive for the camera task's Consider calls).
    std::uint64_t frameNumber_ = 0; // S4: this frame's number, the camera-visible stamp (see PrepareViewQueue)
    vis::OcclusionHistory occlusionHistory_{};
    vis::OcclusionQueryHeap occlusionQueries_{};
    std::vector<std::uint64_t> occlusionResults_;
    // S3b: the alternative producer for the same history (vis.method:2) and its CPU copy.
    vis::HzbOcclusionTester hzbTester_{};
    std::vector<std::uint32_t> hzbResults_;
    std::vector<std::uint8_t> spotLightOccluded_;  // S3a.6, per LightManager spot index
    std::vector<std::uint8_t> pointLightOccluded_; // per point index
    // render::g_localShadowWindRange: each light's last verdict (1 = rigid), for the hysteresis. Per LightManager index.
    std::vector<std::uint8_t> spotWindRigid_;
    std::vector<std::uint8_t> pointWindRigid_;
    // Rung 2 (Step 18): persistent virtual-shadow-map page pool + page table. Allocated once at
    // level load; not yet consumed by any pass.
    VirtualShadowMap vsm_{};
    // S15: the SDSM analysis (partition buffer + GPU-written cull planes). Allocated only while
    // the mode is Sdsm and released on the way out, the same residency rule VSM follows.
    SdsmShadows sdsm_{};
    // Game thread plan G2: the render half's copies, written only by Sync (and by the render half itself), never read by
    // the game half -- the camera (pose taken, matrices and history its own), the suns, the wind, the cascade knobs and
    // the editor selection the frame draws.
    Camera renderCamera_;
    SceneView cameraView_; // the main view: renderCamera_'s matrices (SyncCameraView), the frustum, the queue
    bool renderCameraCut_ = false; // the game camera was cut since the last frame: repeat it after CalcMatrices
    DirectionalLight renderSun_;
    DirectionalLight renderSkySun_;
    vfx::WindState renderWind_{};
    CascadeShadowConfig renderCascadeConfig_{};
    std::vector<std::uint64_t> renderSelection_;
    std::uint32_t renderStaticSetVersion_ = 0; // G4b': the render half's, moved by ApplyStaticSet
};
