#pragma once

#include <array>
#include <memory>
#include <vector>
#include <wrl/client.h>
#include <cstdint>

#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/ShadowGpuData.h"
#include "app/camera/Camera.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/LightManager.h"

#include "app/scene/SceneRenderConfig.h"
#include "app/scene/SceneRenderQueue.h"
#include "app/scene/SceneView.h"
#include "app/scene/SceneFrameData.h"
#include "app/scene/SceneRenderer.h"

class Renderer;
class UploadBatch;

class Scene {
public:

    Camera& CameraRef() { return camera_; }
    const Camera& CameraRef() const { return camera_; }

    const DirectionalLight& GetDirectionalLight() const { return dirLight_; }
    LightManager& GetLightManager() { return lightManager_; }
    const LightManager& GetLightManager() const { return lightManager_; }
    Skybox* GetSkybox() const { return skyBox_.get(); }

    // RT reflections for glass (S15): forwarded from the scene renderer so the
    // transparent pass can bind the TLAS + gate ray tracing.
    bool IsRtReflectActive() const { return sceneRenderer_.IsRtReflectActive(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtTlasSrv(UINT frameIndex) const { return sceneRenderer_.GetTlasSrvCpu(frameIndex); }

    const mat4& GetCascadeView(size_t index) const;
    const mat4& GetCascadeProj(size_t index) const;
    float2 GetCascadeScale(size_t index) const;
    float2 GetCascadeBias(size_t index) const;
    float GetCascadeNormalBias(size_t index) const;
    float GetCascadeDepthBias(size_t index) const;
    const float* GetCascadeSplitsVS() const { return frameData_.cascades.splitsVS; }
    CascadeShadowConfig& CascadeConfig() { return cascadeConfig_; }
    const CascadeShadowConfig& CascadeConfig() const { return cascadeConfig_; }

    void InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void AddObject(std::unique_ptr<RenderableObjectBase> obj);
    bool AddInitializedObject(Renderer& renderer, UploadBatch& uploads, std::unique_ptr<RenderableObjectBase> obj);
    bool RemoveOceanObjects();
    void SetOceanVisible(bool visible);

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
    RenderableObjectBase* FindEditorObject(SceneObjectId id);
    const RenderableObjectBase* FindEditorObject(SceneObjectId id) const;
    void SetSelectedEditorObjectId(SceneObjectId id) { selectedEditorObjectId_ = id; }
    void SetEditorSelectionOutlineRadius(std::uint32_t radius) { selectionOutlineRadius_ = radius; }

    // Nearest editor-owned, visible object hit by the ray (CPU ray vs world AABB),
    // or 0 if none. For viewport click-to-select.
    SceneObjectId RaycastEditorObject(const Math::float3& origin, const Math::float3& dir) const;
#endif

    void Tick(float deltaTime);
    void Render(Renderer* renderer);

    void Clear();

    void SetDirectionalLight(DirectionalLight light);
    void SetSkybox(std::unique_ptr<Skybox> skybox);

    // Render/debug toggles are owned by the app layer (AppController) and pushed
    // here each frame; PrepareViews snapshots them into SceneFrameData.
    void SetRenderSettings(const SceneRenderSettings& settings) { renderSettings_ = settings; }
    const SceneRenderSettings& GetRenderSettings() const { return renderSettings_; }

    const SceneFrameData& FrameData() const { return frameData_; }

    // Rung 1 (Step 11) foundation: monotonic version of the STATIC caster set — bumped when set
    // membership changes (level load, object add/remove/clear). A shadow cache compares it to
    // know its static atlas is stale. Dynamic movers do NOT bump it (their motion is tracked
    // per-caster via RenderableObject::MovedThisFrame). No consumer yet.
    std::uint32_t GetStaticSetVersion() const { return staticSetVersion_; }

private:
    void BumpStaticSetVersion() { ++staticSetVersion_; }

    static constexpr int kCascades = SceneFrameData::kCascades;

    void UpdateCascades(const Camera& camera, Renderer* renderer);

    void PrepareViews(Renderer* renderer);
    void SyncObjectsForRender(SceneObjectSyncReason reason);

    SceneRenderer sceneRenderer_{};
    CascadeShadowConfig cascadeConfig_{};

    // Per-frame pass inputs, filled by PrepareViews (cascade caches, view/light
    // pointers, render settings). SceneRenderer's pass bodies read from this.
    SceneFrameData frameData_{};

    std::vector<std::unique_ptr<RenderableObjectBase>> objects_;
#if WITH_EDITOR
    // Lockstep with objects_: objectIds_[i] is the editor id of objects_[i], or
    // 0 for a runtime object with no editor identity.
    std::vector<SceneObjectId> objectIds_;
    SceneObjectId nextEditorId_ = 1;
    SceneObjectId selectedEditorObjectId_ = 0;
    std::uint32_t selectionOutlineRadius_ = 1;
#endif
    std::array<SceneView, kCascades> cascadeViews_{};
    std::array<SceneView, LightManager::kMaxShadowedSpotLights> spotShadowViews_{};
    std::array<SceneView, LightManager::kMaxShadowedPointLights * 6> pointShadowViews_{}; // 6 cube faces per shadowed point light
    // Step 6e: the directional cascades + spot shadow views all bucketize the same
    // shadow-caster set (identical objects/mask/filter), so it's bucketized ONCE here and
    // shared — each shadow view only runs its own per-frustum Cull against it.
    SceneRenderQueue shadowCasterSource_{};
    // Rung 0 / Steps 1-2: GPU-side shadow data (per-caster instance + bounds, per-view
    // frustum planes). Built at level load, maintained per frame; not yet consumed by any pass.
    ShadowGpuData shadowGpu_{};
    LightManager lightManager_{};
    Camera camera_;

    SceneRenderSettings renderSettings_{};
    std::uint32_t staticSetVersion_ = 0; // Step 11: bumped on static-caster-set membership changes

    DirectionalLight dirLight_;

    std::unique_ptr<Skybox> skyBox_;
};
