#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>
#include <wrl/client.h>
#include <cstdint>

#include "rendering/renderables/RenderableObject.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
#include "app/camera/Camera.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/LightManager.h"

#include "app/scene/SceneRenderConfig.h"
#include "app/scene/SceneRenderQueue.h"
#include "app/scene/SceneView.h"
#include "app/scene/SceneResourceBootstrapper.h"

class Renderer;

class Scene {
public:

    static constexpr size_t kMainRenderGraphPassCount = static_cast<size_t>(RenderPass::Main_Count);
    static constexpr size_t kEpilogueRenderGraphPassCount = static_cast<size_t>(RenderPass::Epilogue_Count)
        - static_cast<size_t>(RenderPass::Epilogue_Overlay);
    static constexpr size_t kGBufferRenderGraphPassCount = static_cast<size_t>(RenderPass::GBuffer_Count)
        - static_cast<size_t>(RenderPass::GBuffer_Driver);
    static constexpr size_t kTransparentRenderGraphPassCount = static_cast<size_t>(RenderPass::Transparent_Count)
        - static_cast<size_t>(RenderPass::Transparent_Driver);

    Camera& CameraRef() { return camera_; }
    const Camera& CameraRef() const { return camera_; }

    const DirectionalLight& GetDirectionalLight() const { return dirLight_; }
    LightManager& GetLightManager() { return lightManager_; }
    const LightManager& GetLightManager() const { return lightManager_; }
    Skybox* GetSkybox() const { return skyBox_.get(); }

    const mat4& GetCascadeView(size_t index) const;
    const mat4& GetCascadeProj(size_t index) const;
    float2 GetCascadeScale(size_t index) const;
    float2 GetCascadeBias(size_t index) const;
    float GetCascadeNormalBias(size_t index) const;
    float GetCascadeDepthBias(size_t index) const;
    const float* GetCascadeSplitsVS() const { return cachedSplitsVS_; }

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

    void SetSsrTechnique(SsrTechnique technique);
    void CycleSsrTechnique();
    SsrTechnique GetSsrTechnique() const { return ssrTechnique_; }

private:
    static constexpr int kCascades = 4;

    void RenderObjectBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const Camera& camera, bool useCommandBundle, bool bindGbufOrScene, bool bindVelocity, size_t chunkSize);
    void RenderShadowBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const mat4& lightView, const mat4& lightProj, UINT cascadeIndex, size_t chunkSize);

    void Pass_PrologueClear(Renderer* r, RenderGraphPassContext ctx);
    void Pass_ObjectCompute(Renderer* r, RenderGraphPassContext ctx);
    using BucketArray = std::array<SceneRenderQueue::ObjectBucket, 4>;

    void UpdateCascades(const Camera& camera, Renderer* renderer);

    void Pass_CSM(Renderer* r, RenderGraphPassContext ctx,
        const std::array<SceneView, kCascades>& cascadeViews);
    void Pass_GBuffer(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const SceneView& mainView);
    void Pass_Lighting(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_SpotShadows(Renderer* r, RenderGraphPassContext ctx,
        const std::array<SceneView, LightManager::kMaxSpotLights>& views);
    void Pass_SpotLights(Renderer* renderer, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_PointLights(Renderer* renderer, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_Skybox(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_SSR(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_SSR_Blur(Renderer* r, RenderGraphPassContext ctx);
    void Pass_Compose(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_Transparent(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const SceneView& mainView);
    void Pass_DebugDraw(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_Tonemap(Renderer* r, RenderGraphPassContext ctx);
    void Pass_Debug(Renderer* r, RenderGraphPassContext ctx);
    void Pass_Overlay(Renderer* r, RenderGraphPassContext ctx);

    void PrepareViews(Renderer* renderer);

    SceneResourceBootstrapper resources_{};
    CascadeShadowConfig cascadeConfig_{};

    // Cache for the lighting pass
    mat4  cachedLightView_[kCascades];
    mat4  cachedLightProj_[kCascades];
    float2 cachedScale_[kCascades];  // atlas scale
    float2 cachedBias_[kCascades];   // atlas bias
    float  cachedSplitsVS_[kCascades + 1] = {}; // near..far in view space
    float  cachedNormalBiasWS_[kCascades] = {};
    float  cachedDepthBiasNDC_[kCascades] = {};

    std::vector<std::unique_ptr<RenderableObjectBase>> objects_;
    std::array<SceneView, kCascades> cascadeViews_{};
    std::array<SceneView, LightManager::kMaxSpotLights> spotShadowViews_{};
    LightManager lightManager_{};
    Camera camera_;

    bool debugTexMode_ = false;
    bool showProfiler_ = false;

    DirectionalLight dirLight_;

    std::unique_ptr<Skybox> skyBox_;

    SsrTechnique ssrTechnique_ = SsrTechnique::LogMarch;

};
