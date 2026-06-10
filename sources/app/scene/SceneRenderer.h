#pragma once

#include <array>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
#include "app/scene/SceneFrameData.h"
#include "app/scene/SceneRenderQueue.h"
#include "app/scene/SceneResourceBootstrapper.h"

class Renderer;
class Camera;
class Skybox;
class RenderableObjectBase;
struct SceneView;

// Owns the frame render pipeline: render-graph construction and all concrete
// pass bodies, plus the pass materials (SceneResourceBootstrapper). World state
// (objects, lights, camera, views) stays in Scene; per-frame inputs arrive
// through SceneFrameData.
class SceneRenderer
{
public:
    static constexpr size_t kMainRenderGraphPassCount = static_cast<size_t>(RenderPass::Main_Count);
    static constexpr size_t kEpilogueRenderGraphPassCount = static_cast<size_t>(RenderPass::Epilogue_Count)
        - static_cast<size_t>(RenderPass::Epilogue_Overlay);
    static constexpr size_t kGBufferRenderGraphPassCount = static_cast<size_t>(RenderPass::GBuffer_Count)
        - static_cast<size_t>(RenderPass::GBuffer_Driver);
    static constexpr size_t kTransparentRenderGraphPassCount = static_cast<size_t>(RenderPass::Transparent_Count)
        - static_cast<size_t>(RenderPass::Transparent_Driver);

    void InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void FinalizeLevelLoad(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        Skybox* skybox);
    void RefreshMaterialHandles(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        Skybox* skybox);
    void Reset();

    // Renders one frame (main graph, overlay epilogue, EndFrame).
    // `frame` must stay valid for the duration of the call.
    void Render(Renderer* renderer, const SceneFrameData& frame);

private:
    static constexpr int kCascades = SceneFrameData::kCascades;

    void RenderObjectBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const Camera& camera, bool useCommandBundle, bool bindGbufOrScene, bool bindVelocity, size_t chunkSize);
    void RenderShadowBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const mat4& lightView, const mat4& lightProj, UINT cascadeIndex, size_t chunkSize);

    void Pass_PrologueClear(Renderer* r, RenderGraphPassContext ctx);
    void Pass_ObjectCompute(Renderer* r, RenderGraphPassContext ctx);

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
    void Pass_ShoreDepth(Renderer* r, RenderGraphPassContext ctx,
        const SceneView* view);
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

    SceneResourceBootstrapper resources_{};

    // Valid only during Render(); pass bodies (running on task threads) read it.
    const SceneFrameData* frame_ = nullptr;
};
