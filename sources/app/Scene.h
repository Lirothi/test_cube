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

    void InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void AddObject(std::unique_ptr<RenderableObjectBase> obj);
    void Tick(float deltaTime);
    void Render(Renderer* renderer);

    void Clear();

    void SetDirectionalLight(DirectionalLight light);
    void SetSkybox(std::unique_ptr<Skybox> skybox);

private:
    void RefreshCachedHandles(Renderer* renderer);

    enum class ObjectRenderType { OpaqueSimple, OpaqueComplex, TransparentSimple, TransparentComplex };
    static constexpr size_t kRenderTypeCount = 4;
    using ObjectBucket = std::vector<RenderableObjectBase*>;
    using ObjectBuckets = std::array<ObjectBucket, kRenderTypeCount>;
    static constexpr size_t ToIndex(ObjectRenderType type) { return static_cast<size_t>(type); }

    struct TransparentSortEntry
    {
        RenderableObjectBase* object = nullptr;
        float depth = 0.0f;
    };

    void RenderObjectBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const mat4& view, const mat4& proj, bool useCommandBundle, bool bindGbufOrScene, size_t chunkSize);
    void RenderShadowBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const mat4& lightView, const mat4& lightProj, UINT cascadeIndex, size_t chunkSize);

    void Pass_PrologueClear(Renderer* r, RenderGraph::PassContext ctx);
    void Pass_CSM(Renderer* r, RenderGraph::PassContext ctx,
        const mat4& view, const mat4& proj,
        const mat4& invView, const mat4& invProj,
        float zNear, float zFar,
        const float3& camDir,
        const ObjectBuckets& buckets);
    void Pass_GBuffer(Renderer* r, RenderGraph::PassContext ctx,
        const mat4& view, const mat4& proj,
        const ObjectBuckets& buckets);
    void Pass_Lighting(Renderer* r, RenderGraph::PassContext ctx,
        const mat4& view, const mat4& proj,
        const mat4& invView, const mat4& invProj,
        const float3& camDir);
    void Pass_SpotShadows(Renderer* r, RenderGraph::PassContext ctx,
        const ObjectBuckets& buckets);
    void Pass_SpotLights(Renderer* renderer, RenderGraph::PassContext ctx,
        const mat4& invView, const mat4& invProj);
    void Pass_PointLights(Renderer* renderer, RenderGraph::PassContext ctx,
        const mat4& invView, const Math::mat4& invProj);
    void Pass_Skybox(Renderer* r, RenderGraph::PassContext ctx,
        const mat4& view, const mat4& proj);
    void Pass_SSR(Renderer* r, RenderGraph::PassContext ctx,
        const mat4& view, const mat4& proj,
        const mat4& invView, const mat4& invProj,
        float zNear, float zFar);
    void Pass_SSR_Blur(Renderer* r, RenderGraph::PassContext ctx);
    void Pass_Compose(Renderer* r, RenderGraph::PassContext ctx,
        const mat4& view, const mat4& proj,
        const mat4& invView, const mat4& invProj,
        float zNear, float zFar);
    void Pass_Transparent(Renderer* r, RenderGraph::PassContext ctx,
        const mat4& view, const mat4& proj,
        const ObjectBuckets& buckets);
    void Pass_DebugDraw(Renderer* r, RenderGraph::PassContext ctx,
        const mat4& view, const mat4& proj);
    void Pass_Tonemap(Renderer* r, RenderGraph::PassContext ctx);
    void Pass_Debug(Renderer* r, RenderGraph::PassContext ctx);
    void Pass_Overlay(Renderer* r, RenderGraph::PassContext ctx);

    void PrepareTransparentBuckets(const mat4& view);

    std::shared_ptr<Material> matLighting_;
    std::shared_ptr<Material> matPointLightCS_;
    std::shared_ptr<Material> matSpotLightCS_;
    std::shared_ptr<Material> matComposeCS_;
    std::shared_ptr<Material> matTonemapCS_;
    std::shared_ptr<Material> matFxaaCS_;
    std::shared_ptr<Material> matSSR_;
    std::shared_ptr<Material> matBlur_;
    std::shared_ptr<Material> matDebug_;
    static constexpr int kCascades = 4;

    struct CBHandleCache {
        struct LightingHandles {
            Material::CBFieldHandle sunDir;
            Material::CBFieldHandle ambient;
            Material::CBFieldHandle lightRgb;
            Material::CBFieldHandle exposure;
            Material::CBFieldHandle camPos;
            Material::CBFieldHandle camDir;
            Material::CBFieldHandle view;
            Material::CBFieldHandle invView;
            Material::CBFieldHandle invProj;
            Material::CBFieldHandle lightViewProj;
            Material::CBFieldHandle cascadeScaleBias;
            Material::CBFieldHandle cascadeSplits;
            Material::CBFieldHandle shadowAtlasSize;
            Material::CBFieldHandle shadowBiasNDC;
            Material::CBFieldHandle normalBiasWS;
            Material::CBFieldHandle screenSize;
            Material::CBFieldHandle invScreenSize;
            void Populate(Material* material);
        } lighting;

        struct PointLightHandles {
            Material::CBFieldHandle invView;
            Material::CBFieldHandle invProj;
            Material::CBFieldHandle camPos;
            Material::CBFieldHandle lightCount;
            Material::CBFieldHandle screenSize;
            Material::CBFieldHandle invScreenSize;
            void Populate(Material* material);
        } pointLights;

        struct SpotLightHandles {
            Material::CBFieldHandle invView;
            Material::CBFieldHandle invProj;
            Material::CBFieldHandle camPos;
            Material::CBFieldHandle lightCount;
            Material::CBFieldHandle screenSize;
            Material::CBFieldHandle invScreenSize;
            Material::CBFieldHandle shadowSize;
            Material::CBFieldHandle invShadowSize;
            void Populate(Material* material);
        } spotLights;

        struct SsrHandles {
            Material::CBFieldHandle view;
            Material::CBFieldHandle proj;
            Material::CBFieldHandle invView;
            Material::CBFieldHandle invProj;
            Material::CBFieldHandle depthA;
            Material::CBFieldHandle depthB;
            Material::CBFieldHandle zNear;
            Material::CBFieldHandle zFar;
            Material::CBFieldHandle screenSize;
            void Populate(Material* material);
        } ssr;

        struct BlurHandles {
            Material::CBFieldHandle dir;
            Material::CBFieldHandle radius;
            void Populate(Material* material);
        } blur;

        struct ComposeHandles {
            Material::CBFieldHandle invView;
            Material::CBFieldHandle invProj;
            Material::CBFieldHandle skyboxIntensity;
            Material::CBFieldHandle camPos;
            Material::CBFieldHandle screenSize;
            Material::CBFieldHandle invScreenSize;
            void Populate(Material* material);
        } compose;

        struct FxaaHandles {
            Material::CBFieldHandle invResolution;
            Material::CBFieldHandle subpix;
            Material::CBFieldHandle edgeThreshold;
            Material::CBFieldHandle edgeThresholdMin;
            void Populate(Material* material);
        } fxaa;
    } cbHandles_{};

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

    ObjectBuckets renderBuckets_;
    std::array<std::vector<TransparentSortEntry>, 2> transparentSortScratch_{};
    Camera camera_;

    bool debugTexMode_ = false;
    bool showProfiler_ = false;

    DirectionalLight dirLight_;

    std::unique_ptr<Skybox> skyBox_;

};
