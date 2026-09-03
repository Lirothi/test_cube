#pragma once

#include <array>
#include <memory>
#include <vector>
#include <wrl/client.h>
#include <cstdint>

#include "rendering/renderables/RenderableObject.h"
#include "rendering/shadows/ShadowGpuData.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "app/camera/Camera.h"
#include "rendering/core/PhotographicSettings.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/LightManager.h"

#include "app/scene/SceneRenderConfig.h"
#include "app/scene/SceneRenderQueue.h"
#include "app/scene/SceneView.h"
#include "app/scene/SceneFrameData.h"
#include "app/scene/SceneRenderer.h"
#include "vfx/WindState.h"

class Renderer;
class UploadBatch;
class OceanRenderable;

class Scene {
public:

    Camera& CameraRef() { return camera_; }
    const Camera& CameraRef() const { return camera_; }

    const DirectionalLight& GetDirectionalLight() const { return dirLight_; }
    // P4 measurement hook: the sweep harness needs to drive the sun's own controls to measure how
    // much of the level's `exposure` the auto-exposure silently cancels. Runtime only -- like the
    // camera settings above, nothing here writes the level.
    DirectionalLight& DirectionalLightRef() { return dirLight_; }
    // P1: the level's photographic camera settings. Dormant by default (enabled = false), so this
    // is pure state until P2 schedules the metering passes that read it.
    const render::CameraExposureSettings& GetCameraExposure() const { return cameraExposure_; }
    void SetCameraExposure(const render::CameraExposureSettings& settings) { cameraExposure_ = settings; }
    // Live tuning from the developer window. This is RUNTIME state only -- it does not write the
    // level, so a tuned value has to be copied into the level's cameraExposure section (or the
    // editor inspector) to survive a reload. Same deal as the ocean controls.
    render::CameraExposureSettings& CameraExposureRef() { return cameraExposure_; }

    // P3: the display transform. Same ownership and live-tuning story as the exposure block above.
    const render::ColorPipelineSettings& GetColorPipeline() const { return colorPipeline_; }
    void SetColorPipeline(const render::ColorPipelineSettings& s) { colorPipeline_ = s; }
    render::ColorPipelineSettings& ColorPipelineRef() { return colorPipeline_; }
    LightManager& GetLightManager() { return lightManager_; }
    const LightManager& GetLightManager() const { return lightManager_; }
    Skybox* GetSkybox() const { return skyBox_.get(); }

    // Rung 2: the virtual-shadow-map page pool + page table (dev-window "VSM" tab reads its stats).
    const VirtualShadowMap& Vsm() const { return vsm_; }

    // RT reflections for glass (S15): forwarded from the scene renderer so the
    // transparent pass can bind the TLAS + gate ray tracing.
    bool IsRtReflectActive() const { return sceneRenderer_.IsRtReflectActive(); }
    // I2: after a material's GPU textures are rebuilt, drop the RT acceleration/bindless caches so
    // they re-register with the new SRVs (else the geom-info table dangles -> DEVICE_HUNG). Call
    // with the GPU idle. Cheap no-op when RT is off.
    void InvalidateRaytracing() { sceneRenderer_.InvalidateRaytracing(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtTlasSrv(UINT frameIndex) const { return sceneRenderer_.GetTlasSrvCpu(frameIndex); }

    const mat4& GetCascadeView(size_t index) const;
    const mat4& GetCascadeProj(size_t index) const;
    float2 GetCascadeScale(size_t index) const;
    float2 GetCascadeBias(size_t index) const;
    float GetCascadeTexelWS(size_t index) const;
    float GetCascadeDepthBias(size_t index) const;
    const float* GetCascadeSplitsVS() const { return frameData_.cascades.splitsVS; }
    // S0.1: the whole cascade block, including the *Dbg diagnostics the developer window reads.
    const SceneFrameData::CascadeData& GetCascadeData() const { return frameData_.cascades; }
    CascadeShadowConfig& CascadeConfig() { return cascadeConfig_; }
    const CascadeShadowConfig& CascadeConfig() const { return cascadeConfig_; }

    void InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void AddObject(std::unique_ptr<RenderableObjectBase> obj);
    // Occlusion plan S0: union of every renderable's world box except the ocean. For the
    // `scene.replicate` stress grid's step; not a per-frame call.
    AABB ComputeStaticBounds() const;
    bool AddInitializedObject(Renderer& renderer, UploadBatch& uploads, std::unique_ptr<RenderableObjectBase> obj);
    bool RemoveOceanObjects();
    void SetOceanVisible(bool visible);
    // W1: locate the ocean's shared clock (null if no ocean). PUBLIC since P16.6 -- the capture
    // harness turns the shore contact foam off through it, which beats editing tuned content.
    OceanRenderable* FindOceanRenderable();

#if WITH_EDITOR
    // Stable identity for editor-spawned objects. SceneObjectId 0 = a runtime
    // object with no editor identity (what AddObject assigns). Editor code uses
    // the same uint64 value for EditorObjectId and SceneObjectId. objectIds_ is
    // kept in lockstep with objects_ on every add/remove/clear.
    using SceneObjectId = std::uint64_t;
    SceneObjectId AddEditorObject(std::unique_ptr<RenderableObjectBase> obj);
    void AddObjectWithEditorId(std::unique_ptr<RenderableObjectBase> obj, SceneObjectId id);
    bool AddInitializedEditorObject(Renderer& renderer, UploadBatch& uploads, SceneObjectId id, std::unique_ptr<RenderableObjectBase> obj);
    bool RemoveEditorObject(SceneObjectId id);
    // Rebuild the shadow-caster GPU data + the consolidated mega VB/IB after an editor caster-set
    // change (spawn/delete). Runs on a fresh GPU-idle upload batch, so the VSM per-page draw keeps its
    // fast single-ExecuteIndirect-per-page path instead of the ~10ms per-group fallback until reload.
    void RefreshShadowGpuForEditor(Renderer& renderer);
    // Marks cached render-queue sources stale after an editor visibility/layer change. The command
    // stack coalesces visibility with other caster-set changes into one shadow GPU refresh.
    void NotifyEditorShadowCasterVisibilityChanged() { BumpStaticSetVersion(); }
    RenderableObjectBase* FindEditorObject(SceneObjectId id);
    const RenderableObjectBase* FindEditorObject(SceneObjectId id) const;
    void SetSelectedEditorObjectIds(const std::vector<SceneObjectId>& ids);
    void SetEditorSelectionOutlineRadius(std::uint32_t radius) { selectionOutlineRadius_ = radius; }

    // Nearest editor-owned, visible object hit by the ray. World AABBs provide the
    // broad phase; standalone CPU meshes use exact base-LOD triangles as the narrow
    // phase. Runtime generators and hidden objects are intentionally not editor
    // picking or placement surfaces.
    SceneObjectId RaycastEditorObject(const Math::float3& origin,
        const Math::float3& dir,
        float* outDistance = nullptr,
        SceneObjectId ignoredObjectId = 0,
        const std::vector<SceneObjectId>* ignoredObjectIds = nullptr) const;
#endif

    void Tick(float deltaTime);
    void Render(Renderer* renderer);

    void Clear();

    void SetDirectionalLight(DirectionalLight light);
    void SetSkybox(std::unique_ptr<Skybox> skybox);

    // Render/debug toggles are owned by the app layer (AppController) and pushed
    // here each frame; PrepareViews snapshots them into SceneFrameData.
    // P6B: the AO settings belong to the LEVEL, like cameraExposure and colorPipeline, not to the
    // app's runtime toggles — they describe how this scene should look, and a level has to be able
    // to save them. AppController still owns the rest of SceneRenderSettings and re-pushes it every
    // Tick, so `gtao` is deliberately overwritten from here on the way in: the app's copy is a
    // transport, this is the source of truth.
    const GtaoSettings& GetGtao() const { return gtao_; }
    void SetGtao(const GtaoSettings& s) { gtao_ = s; }
    GtaoSettings& GtaoRef() { return gtao_; }

    // P7: aerial perspective is level-scoped for exactly the reasons above -- it describes how this
    // scene should look, and a level has to be able to save it.
    const AtmosphereSettings& GetAtmosphere() const { return atmosphere_; }
    void SetAtmosphere(const AtmosphereSettings& s) { atmosphere_ = s; }
    AtmosphereSettings& AtmosphereRef() { return atmosphere_; }

    // P8 bloom. Same ownership rule as the two above: the SCENE copy is the source of truth and
    // SceneRenderSettings is only the per-frame transport, so the dev window, `--set` and the
    // editor all edit this one.
    const BloomSettings& GetBloom() const { return bloom_; }
    void SetBloom(const BloomSettings& s) { bloom_ = s; }
    BloomSettings& BloomRef() { return bloom_; }

    void SetRenderSettings(const SceneRenderSettings& settings)
    {
        renderSettings_ = settings;
        renderSettings_.gtao = gtao_;
        renderSettings_.atmosphere = atmosphere_;
        renderSettings_.bloom = bloom_;
    }
    const SceneRenderSettings& GetRenderSettings() const { return renderSettings_; }

    const SceneFrameData& FrameData() const { return frameData_; }

    // W1: the global wind source of truth (drives the ocean's force/direction and, from W3 on,
    // foliage sway). Advanced each Tick from the ocean's shared clock; authored by the "wind"
    // level entity (W2).
    vfx::WindState& GetWindState() { return windState_; }
    const vfx::WindState& GetWindState() const { return windState_; }

    // Rung 1 (Step 11) foundation: monotonic version of the shadow-caster set — bumped when its
    // membership or editor visibility changes. A shadow cache compares it to know its data is
    // stale. Dynamic movers do NOT bump it (their motion is tracked per-caster via
    // RenderableObject::MovedThisFrame).
    std::uint32_t GetStaticSetVersion() const { return staticSetVersion_; }

private:
    // Every queue stores non-owning object pointers. Changing scene membership must invalidate
    // them immediately; waiting for PrepareViews is too late because UpdateCascades reads the
    // previous frame's cascade queues first.
    void BumpStaticSetVersion();

    static constexpr int kCascades = SceneFrameData::kCascades;

    void ReconcileShadowMode(Renderer* renderer); // Step 24b: GPU-idle Legacy<->VSM resource switch
    // GPU-idle rebuild of the shadow caster data + consolidated mega VB/IB (the body shared by the
    // editor caster-set refresh and shadow-LOD curve changes). Waits for the GPU, so call sparingly.
    void RebuildShadowCasters(Renderer& renderer);
    // Poll the live shadow-LOD bias/stride against the caster tables; rebuild on a change.
    void ReconcileShadowLodCurve(Renderer* renderer);
    void UpdateCascades(const Camera& camera, Renderer* renderer);
    // Occlusion plan S0: which render::VisibilityStats slot a view writes (camera 0, cascades
    // 1..4), or -1 for the views that have none (local lights, clipmap levels).
    int VisibilitySlotFor(const SceneView& view) const;
    void UpdateClipmap(const Camera& camera); // Step 24d: camera-centered directional clipmap views (VSM)

    void PrepareViewQueue(SceneView& view, uint32_t cameraLayerMask);
    void PrepareViews(Renderer* renderer);
    void SyncObjectsForRender(SceneObjectSyncReason reason);

    SceneRenderer sceneRenderer_{};
    CascadeShadowConfig cascadeConfig_{};

    // Per-frame pass inputs, filled by PrepareViews (cascade caches, view/light
    // pointers, render settings). SceneRenderer's pass bodies read from this.
    SceneFrameData frameData_{};

    std::vector<std::unique_ptr<RenderableObjectBase>> objects_;
    // Has the ocean been told where this level's terrain sits? One sweep of objects_ per load.
    bool shoreAreaValid_ = false;
#if WITH_EDITOR
    // Lockstep with objects_: objectIds_[i] is the editor id of objects_[i], or
    // 0 for a runtime object with no editor identity.
    std::vector<SceneObjectId> objectIds_;
    SceneObjectId nextEditorId_ = 1;
    std::array<SceneObjectId, SceneFrameData::kMaxEditorSelection> selectedEditorObjectIds_{};
    std::uint32_t selectedEditorObjectCount_ = 0;
    std::uint32_t selectionOutlineRadius_ = 1;
#endif
    std::array<SceneView, kCascades> cascadeViews_{};
    std::array<SceneView, vsm::kNumClipmapLevels> clipmapViews_{}; // Step 24d: directional clipmap (VSM)
    // Built beside clipmapViews_ in UpdateClipmap; feeds the clipmap LOD fallback chain. Kept
    // separate from SceneView because it is expressed in the SHARED light frame, which no single
    // level's view matrix carries (each level's own view is centred on itself).
    vsm::ClipmapSquares clipmapSquares_{};
    std::array<SceneView, LightManager::kMaxShadowedSpotLights> spotShadowViews_{};
    std::array<SceneView, LightManager::kMaxShadowedPointLights * 6> pointShadowViews_{}; // 6 cube faces per shadowed point light
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
    // Rung 2 (Step 18): persistent virtual-shadow-map page pool + page table. Allocated once at
    // level load; not yet consumed by any pass.
    VirtualShadowMap vsm_{};
    LightManager lightManager_{};
    Camera camera_;

    SceneRenderSettings renderSettings_{};
    std::uint32_t staticSetVersion_ = 0; // Step 11: bumped on shadow-caster membership/visibility changes

    vfx::WindState windState_{}; // W1: global wind, advanced each Tick from the ocean's shared clock

    DirectionalLight dirLight_;
    render::CameraExposureSettings cameraExposure_{}; // P1, dormant; see PhotographicSettings.h
    render::ColorPipelineSettings colorPipeline_{};   // P3; see PhotographicSettings.h
    GtaoSettings gtao_{};                             // P6B; level-scoped, see GetGtao
    AtmosphereSettings atmosphere_{};                 // P7; level-scoped, see GetAtmosphere
    BloomSettings bloom_{};                           // P8; level-scoped, see GetBloom

    std::unique_ptr<Skybox> skyBox_;
};
