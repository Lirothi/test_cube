#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "rendering/renderables/RenderableObject.h"
#include "app/Camera.h"
#include "app/DirectionalLight.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/LightManager.h"

#include "app/scene/SceneRenderConfig.h"
#include "app/scene/SceneRenderQueue.h"
#include "app/scene/SceneResourceBootstrapper.h"

class Renderer;

class Scene {
public:

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

private:

    void RenderObjectBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const Camera& camera, bool useCommandBundle, bool bindGbufOrScene, size_t chunkSize);
    void RenderShadowBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const mat4& lightView, const mat4& lightProj, UINT cascadeIndex, size_t chunkSize);

    void Pass_PrologueClear(Renderer* r, RenderGraph::PassContext ctx);
    using BucketArray = std::array<SceneRenderQueue::ObjectBucket, 4>;

    void Pass_CSM(Renderer* r, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_GBuffer(Renderer* r, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_Lighting(Renderer* r, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_SpotShadows(Renderer* r, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_SpotLights(Renderer* renderer, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_PointLights(Renderer* renderer, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_Skybox(Renderer* r, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_SSR(Renderer* r, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_SSR_Blur(Renderer* r, RenderGraph::PassContext ctx);
    void Pass_Compose(Renderer* r, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_Transparent(Renderer* r, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_DebugDraw(Renderer* r, RenderGraph::PassContext ctx,
        const Camera& camera);
    void Pass_Tonemap(Renderer* r, RenderGraph::PassContext ctx);
    void Pass_Debug(Renderer* r, RenderGraph::PassContext ctx);
    void Pass_Overlay(Renderer* r, RenderGraph::PassContext ctx);

    static constexpr int kCascades = 4;

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
    LightManager lightManager_{};
    Camera camera_;

    bool debugTexMode_ = false;
    bool showProfiler_ = false;

    DirectionalLight dirLight_;

    std::unique_ptr<Skybox> skyBox_;

};
