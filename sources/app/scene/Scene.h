#pragma once

#include <array>
#include <memory>
#include <vector>
#include <wrl/client.h>
#include <cstdint>

#include "rendering/renderables/RenderableObject.h"
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

private:
    static constexpr int kCascades = SceneFrameData::kCascades;

    void UpdateCascades(const Camera& camera, Renderer* renderer);

    void PrepareViews(Renderer* renderer);

    SceneRenderer sceneRenderer_{};
    CascadeShadowConfig cascadeConfig_{};

    // Per-frame pass inputs, filled by PrepareViews (cascade caches, view/light
    // pointers, render settings). SceneRenderer's pass bodies read from this.
    SceneFrameData frameData_{};

    std::vector<std::unique_ptr<RenderableObjectBase>> objects_;
    std::array<SceneView, kCascades> cascadeViews_{};
    std::array<SceneView, LightManager::kMaxSpotLights> spotShadowViews_{};
    // Step 6e: the directional cascades + spot shadow views all bucketize the same
    // shadow-caster set (identical objects/mask/filter), so it's bucketized ONCE here and
    // shared — each shadow view only runs its own per-frustum Cull against it.
    SceneRenderQueue shadowCasterSource_{};
    LightManager lightManager_{};
    Camera camera_;

    SceneRenderSettings renderSettings_{};

    DirectionalLight dirLight_;

    std::unique_ptr<Skybox> skyBox_;
};
