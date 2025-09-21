#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "RenderableObject.h"
#include "Camera.h"
#include "InputManager.h"
#include "Skybox.h"
#include "PointLight.h"

class Renderer;

class Scene {
public:
    void SetInput(InputManager* input) { input_ = input; }
    void SetActions(ActionMap* a) { actions_ = a; }
    Camera& CameraRef() { return camera_; }
    const Camera& CameraRef() const { return camera_; }

    void InitAll(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void AddObject(std::unique_ptr<RenderableObjectBase> obj);
    void Tick(float deltaTime);
    void Render(Renderer* renderer);

    void Clear();

private:
    enum class ObjectRenderType { OpaqueSimple, OpaqueComplex, TransparentSimple, TransparentComplex };
    static constexpr size_t kRenderTypeCount = 4;
    using ObjectBucket = std::vector<RenderableObjectBase*>;
    using ObjectBuckets = std::array<ObjectBucket, kRenderTypeCount>;
    static constexpr size_t ToIndex(ObjectRenderType type) { return static_cast<size_t>(type); }

    void RenderObjectBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const mat4& view, const mat4& proj, bool useCommandBundle, bool bindGbufOrScene);
    void RenderShadowBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const mat4& lightView, const mat4& lightProj, UINT cascadeIndex, size_t chunkSize = 32);

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
    void Pass_PointLights(Renderer* renderer, RenderGraph::PassContext ctx,
        const mat4& view, const Math::mat4& proj,
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
    void Pass_Tonemap(Renderer* r, RenderGraph::PassContext ctx);
    void Pass_Debug(Renderer* r, RenderGraph::PassContext ctx);
    void Pass_Overlay(Renderer* r, RenderGraph::PassContext ctx);

    std::shared_ptr<Material> matLighting_;
    std::shared_ptr<Material> matCompose_;
    std::shared_ptr<Material> matTonemap_;
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
            void Populate(Material* material);
        } lighting;

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
            Material::CBFieldHandle view;
            Material::CBFieldHandle proj;
            Material::CBFieldHandle invView;
            Material::CBFieldHandle invProj;
            Material::CBFieldHandle skyboxIntensity;
            void Populate(Material* material);
        } compose;
    } cbHandles_{};

    // кэш для лайт-пасса
    mat4  cachedLightView_[kCascades];
    mat4  cachedLightProj_[kCascades];
    float2 cachedScale_[kCascades];  // atlas scale
    float2 cachedBias_[kCascades];   // atlas bias
    float  cachedSplitsVS_[kCascades + 1] = {}; // near..far в view-space
    float  cachedNormalBiasWS_[kCascades] = {};
    float  cachedDepthBiasNDC_[kCascades] = {};

    std::vector<std::unique_ptr<RenderableObjectBase>> objects_;
    std::vector<PointLight> pointLights_;

    ObjectBuckets renderBuckets_;

    InputManager* input_ = nullptr;
    ActionMap* actions_ = nullptr;
    Camera camera_;

    bool debugTexMode_ = false;
    bool showProfiler_ = false;

    struct DirectionalLight
    {
        float3 dir;
        float3 color;
        float exposure = 1.0f;
        float ambient = 0.05f;
    };
    DirectionalLight dirLight_;

    std::unique_ptr<Skybox> skyBox_;
};
