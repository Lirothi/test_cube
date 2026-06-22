#pragma once

#include <array>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/rt/BindlessTable.h"
#include "rendering/rt/ReflectionHistory.h"
#include "core/task/TaskSystem.h"
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
        const Camera& camera, bool useCommandBundle, bool bindGbufOrScene, bool bindVelocity, size_t chunkSize,
        D3D12_GPU_VIRTUAL_ADDRESS viewCB);

    void Pass_BuildAS(Renderer* r, RenderGraphPassContext ctx);
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
    void Pass_ScreenSpaceReflections(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_RTReflections(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_RTDenoise(Renderer* r, RenderGraphPassContext ctx); // S11 temporal accumulate
    void Pass_ClearReflections(Renderer* r, RenderGraphPassContext ctx); // S8 "Off": zero the reflection target
    void Pass_ReflectionBlur(Renderer* r, RenderGraphPassContext ctx);
    void Pass_Compose(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_RTDebug(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_Transparent(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const SceneView& mainView);
    void Pass_DebugDraw(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera);
    void Pass_Tonemap(Renderer* r, RenderGraphPassContext ctx);
    void Pass_Debug(Renderer* r, RenderGraphPassContext ctx);
    void Pass_Overlay(Renderer* r, RenderGraphPassContext ctx, TaskSystem::TaskHandle& overlayPrepTask);

    SceneResourceBootstrapper resources_{};

    // RT acceleration structures (S5). Built by Pass_BuildAS when RT is supported
    // and enabled; no consumer yet. asManager_ owns the per-mesh BLAS cache and
    // the per-frame TLAS. asScratchRetireFrame_ defers releasing one-time BLAS
    // build scratch until its command list's frame has surely completed.
    rt::AccelerationStructureManager asManager_;
    rt::BindlessTable bindless_; // S9: per-mesh VB/IB + geometry-info for RT hit shading
    rt::ReflectionHistory reflectionHistory_; // S11: ping-pong temporal-accumulation textures
    bool asManagerInited_ = false;
    uint64_t asScratchRetireFrame_ = 0;
    std::vector<rt::InstanceEntry> rtInstances_; // reused scratch (only Pass_BuildAS touches it)

    // Valid only during Render(); pass bodies (running on task threads) read it.
    const SceneFrameData* frame_ = nullptr;
};
