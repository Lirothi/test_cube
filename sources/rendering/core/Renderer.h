#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>

#include "core/math/Math.h"
#include "rendering/descriptors/DescriptorAllocator.h"
#include "rendering/core/FrameResource.h"
#include "rendering/core/GraphicsDevice.h"
#include "rendering/core/SwapchainManager.h"
#include "rendering/core/FrameScheduler.h"
#include "rendering/core/ResourceStateTracker.h"
#include "rendering/core/RenderTargetManager.h"
#include "rendering/core/SubmitTimeline.h"
#include "rendering/core/RenderConstants.h"
#include "third_party/robin_hood.h"
#include "rendering/descriptors/SamplerManager.h"
#include "materials/Material.h"
#include "rendering/descriptors/InputLayoutManager.h"
#include "rendering/meshes/MeshManager.h"
#include "text/TextManager.h"
#include "text/FontManager.h"
#include "materials/MaterialDataManager.h"
#include "rendering/core/RenderContextPool.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/debug/DebugDraw.h"
#include "ui/ImGuiLayer.h"
#include "streamline/include/sl.h"
#include "streamline/include/sl_core_types.h"
#include "streamline/include/sl_dlss.h"

using Microsoft::WRL::ComPtr;

class Camera;

class DlssHandler;

class Renderer {
public:
    struct ThreadCL {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cl = nullptr;
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    };
    enum class ClearMode { None, Color, ColorDepth };
    using DeferredTargets = RenderTargetManager::DeferredTargets;

    Renderer();
    ~Renderer();
    void Shutdown();
    void ReportLiveObjects();

    // Initialize device/queue/swap/RTV/DSV plus frame resources and the fence
    void InitD3D12(HWND window, UINT width, UINT height);
    void InitFence(); // kept for compatibility; does nothing if already initialized

    // Frame cycle
    void BeginFrame();                 // waits for its frame, resets allocator and command list
    void EndFrame();                   // barrier RT->Present, Execute, Present, signal fence
    void InitImGui();
    void BeginImGuiFrame();
    void RenderImGui(ID3D12GraphicsCommandList* commandList);
    void ShutdownImGui();
    bool HandleImGuiWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool ImGuiWantsMouse() const;
    bool ImGuiWantsKeyboard() const;

    void Tick(float dt);
    bool ConsumeMaterialHotReloadFlag();

    void CreateDeferredTargets(UINT width, UINT height);
    void DestroyDeferredTargets();

    void BindGBuffer(ID3D12GraphicsCommandList* cl, ClearMode mode);
    void BindLightTarget(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindSceneColor(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindLightTargetWithVelocity(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindSceneColorWithVelocity(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindShadowTarget(ID3D12GraphicsCommandList* cl, int cascadeIndex, bool clearDepth);
    void BindSpotShadowTarget(ID3D12GraphicsCommandList* cl, UINT lightIndex, bool clearDepth);
    // Prebuilt SRV tables (in the frame's shader-visible heap)
    D3D12_GPU_DESCRIPTOR_HANDLE StageGBufferSrvTable(); // t0..t3 : GB0,GB1,GB2,Depth
    D3D12_GPU_DESCRIPTOR_HANDLE StageComposeSrvTable(); // t0..t1 : Light,GB2
    D3D12_GPU_DESCRIPTOR_HANDLE StageTonemapSrvTable(); // t0     : Scene or DLSS output

    // Format accessors — definitions live in RenderConstants.h (render:: namespace).
    DXGI_FORMAT GetLightTargetFormat() const { return render::kLightTargetFormat; }
    DXGI_FORMAT GetSceneColorFormat() const { return render::kSceneColorFormat; }
    DXGI_FORMAT GetDlssBiasFormat() const { return render::kDlssBiasFormat; }
    DXGI_FORMAT GetBackbufferFormat() const { return render::kBackbufferFormat; }
    DXGI_FORMAT GetBackbufferResourceFormat() const { return render::kBackbufferResourceFormat; }
    DXGI_FORMAT GetGBuffer0Format() const { return render::kGBuffer0Format; }
    DXGI_FORMAT GetGBuffer1Format() const { return render::kGBuffer1Format; }
    DXGI_FORMAT GetGBuffer2Format() const { return render::kGBuffer2Format; }
    DXGI_FORMAT GetDeferredDepthFormat() const { return render::kDeferredDepthFormat; }
    DXGI_FORMAT GetDsvFormat() const { return render::kDeferredDepthFormat; }
    DXGI_FORMAT GetDepthSrvFormat() const { return render::kDeferredDepthSrvFormat; }
    DXGI_FORMAT GetGBufferVelocityFormat() const { return render::kGBufferVelocityFormat; }
    DXGI_FORMAT GetSsrFormat() const { return render::kSsrFormat; }
    DXGI_FORMAT GetSsrBlurFormat() const { return render::kSsrBlurFormat; }

    const DeferredTargets& GetDeferredForFrame() const { return rtManager_.Deferred(currentFrameIndex_); }

    // Utility functions
    void WaitForPreviousFrame();       // full synchronization (used during resize/destruction)
    void OnResize(UINT width, UINT height);

    ThreadCL BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE type, ID3D12PipelineState* initialPSO = nullptr);
    // localOrder positions this list deterministically within its batch's
    // direct namespace, assigned BEFORE dispatch (chunk/cascade/light index;
    // a lone direct in its batch uses 0). See SubmitTimeline.
    void EndThreadCommandList(ThreadCL& t, size_t batchIndex, uint32_t localOrder = 0);
    ThreadCL BeginThreadCommandBundle(ID3D12PipelineState* initialPSO = nullptr);
    // localOrder positions this bundle within its batch's bundle namespace.
    void EndThreadCommandBundle(ThreadCL& b, size_t batchIndex, uint32_t localOrder = 0);

    void BeginSubmitTimeline();
    size_t BeginSubmitBatch();
    void ExecuteTimelineAndPresent();
    void RecordBindAndClear(ID3D12GraphicsCommandList* cl);
    void RecordBindDefaultsNoClear(ID3D12GraphicsCommandList* cl);
    void RegisterPassDriver(ID3D12GraphicsCommandList* cl, size_t batchIndex);

    // Getters
    ID3D12Device* GetDevice() const { return graphicsDevice_.Device(); }
    ID3D12CommandQueue* GetCommandQueue() const { return graphicsDevice_.Queue(); }
    HWND GetHWND() const { return hWnd_; }
    UINT GetWidth() const { return width_; }
    UINT GetHeight() const { return height_; }
    UINT GetRenderWidth() const { return renderWidth_; }
    UINT GetRenderHeight() const { return renderHeight_; }
    float GetRenderResolutionScale() const { return renderResolutionScale_; }
    Math::float2 GetCameraJitter() const;

    ID3D12Resource* GetCurrentBackbuffer() const { return currentFrameIndex_ < render::kFrameCount ? swapchain_.Backbuffer(currentFrameIndex_) : nullptr; }

    // Access the global descriptor allocator and current frame
    DescriptorAllocator& GetDescAlloc() { return frameScheduler_.GetFrameResource(currentFrameIndex_)->GetDescAlloc(); }
    DescriptorAllocator& GetSamplerAlloc() { return frameScheduler_.GetFrameResource(currentFrameIndex_)->GetSamplerAlloc(); }
    FrameResource* GetFrameResource() { return frameScheduler_.GetFrameResource(currentFrameIndex_); }

    UINT GetCurrentFrameIndex() const { return currentFrameIndex_; }
    uint64_t GetTotalFrameNumber() const { return totalFrameNumber_; }

    void InitTextSystem(ID3D12GraphicsCommandList* uploadCl, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive, const std::wstring& folder);

    SamplerManager* GetSamplerManager() { return &samplerManager_; }
    MaterialManager* GetMaterialManager() { return &materialManager_; }
    InputLayoutManager* GetInputLayoutManager() { return &inputLayoutManager_; }
    MeshManager* GetMeshManager() { return &meshManager_; }
    TextManager* GetTextManager() { return &textManager_; }
    FontManager* GetFontManager() { return &fontManager_; }
    MaterialDataManager* GetMaterialDataManager() { return &materialDataManager_; }
    RenderContextPool* GetRenderContextPool() { return &ctxPool_; }
    DebugDrawSystem* GetDebugDrawSystem() { return &debugDrawSystem_; }
    const DebugDrawSystem* GetDebugDrawSystem() const { return &debugDrawSystem_; }

	float GetFPS() const { return fps_; }
    void SetWireframeMode(bool w) { wireframeMode_ = w; }
    bool GetWireframeMode() const { return wireframeMode_; }

    void SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state);
    void ClearResourceState(ID3D12Resource* res);
    void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after);
    void UAVBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res);

    void SetSsrTextureScale(Math::float2 scale);
    void SetSsrTextureScale(float scale) { SetSsrTextureScale(Math::float2(scale, scale)); }
    Math::float2 GetSsrTextureScale() const { return ssrTextureScale_; }
    UINT GetSsrTextureWidth() const;
    UINT GetSsrTextureHeight() const;
    void SetRenderResolutionScale(float scale);
    void UpdateDlssCameraData(const Camera& camera);
    bool EvaluateDLSS(ID3D12GraphicsCommandList* cl);
    bool IsDlssActive() const;
    bool IsDlssAvailable() const;
    void SetDlssActive(bool active);
    void SetDlssMode(sl::DLSSMode mode);
    sl::DLSSMode GetDlssMode() const { return dlssMode_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetTonemapSourceSrvCPU() const;
    void BindDescriptorHeaps(ID3D12GraphicsCommandList* cl) const;

    template<class Alloc, class It>
    inline GpuDescHandle StageDescriptorTableRange(
        Alloc& alloc,
        D3D12_DESCRIPTOR_HEAP_TYPE heapType,
        It first, It last)
    {
        const UINT count = static_cast<UINT>(std::distance(first, last));
        if (count == 0) {
            return {};
        }

        GpuDescHandle block = alloc.Alloc(count);
        const UINT incr = alloc.GetIncr();

        D3D12_CPU_DESCRIPTOR_HANDLE dst = block.cpu;
        for (It it = first; it != last; ++it) {
            const D3D12_CPU_DESCRIPTOR_HANDLE src = *it;
            graphicsDevice_.Device()->CopyDescriptorsSimple(1, dst, src, heapType);
            dst.ptr += incr;
        }
        return block;
    }

    inline GpuDescHandle StageSrvUavTable(std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> src)
    {
        return StageDescriptorTableRange(GetDescAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, src.begin(), src.end());
    }

    template<size_t N>
    inline GpuDescHandle StageSrvUavTable(const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, N>& src)
    {
        return StageSrvUavTable(src, N);
    }

    template<size_t N>
    inline GpuDescHandle StageSrvUavTable(const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, N>& src, size_t count)
    {
        const size_t clamped = std::min(count, src.size());
        const auto first = src.begin();
        const auto last = first + static_cast<std::ptrdiff_t>(clamped);
        return StageDescriptorTableRange(GetDescAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, first, last);
    }

    inline GpuDescHandle StageSamplerTable(std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> src)
    {
        return StageDescriptorTableRange(GetSamplerAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, src.begin(), src.end());
    }

    template<size_t N>
    inline GpuDescHandle StageSamplerTable(const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, N>& src)
    {
        return StageSamplerTable(src, N);
    }

    template<size_t N>
    inline GpuDescHandle StageSamplerTable(const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, N>& src, size_t count)
    {
        const size_t clamped = std::min(count, src.size());
        const auto first = src.begin();
        const auto last = first + static_cast<std::ptrdiff_t>(clamped);
        return StageDescriptorTableRange(GetSamplerAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, first, last);
    }

private:
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void CreateSwapChainAndRTVs(UINT width, UINT height);
    void CreateDepthResources(UINT width, UINT height);
    void WaitForFrame(UINT frameIndex);   // wait for a specific frame (by that frame's fence value)
    void SignalFrame(UINT frameIndex);    // signal the fence for a frame
    void RefreshCurrentFrameCaches();
    std::pair<UINT, UINT> ComputeSsrTextureSize(UINT baseWidth, UINT baseHeight) const;
    void RecreateDeferredTargets();
    void UpdateRenderResolutionFromScale();


private:
    SubmitTimeline submitTimeline_;
    std::vector<ID3D12CommandList*> submitListsScratch_;
    std::vector<ID3D12CommandList*> fixedSubmitScratch_;
    std::vector<D3D12_RESOURCE_BARRIER> barrierScratch_;

    // Per-frame deferred render targets + their CPU descriptor heaps
    RenderTargetManager rtManager_;

    // OS / dimensions
    HWND  hWnd_ = nullptr;
    UINT  width_ = 1600;
    UINT  height_ = 900;
    float renderResolutionScale_ = 1.0f;
    UINT  renderWidth_ = width_;
    UINT  renderHeight_ = height_;

    Math::float2 ssrTextureScale_ = Math::float2(0.5f, 0.5f);
    UINT ssrTextureWidth_ = 1;
    UINT ssrTextureHeight_ = 1;

    bool wireframeMode_ = false;

    float fps_ = 0.0f;
    float fpsAlpha_ = 0.99f; // exponential smoothing: 0..1 (higher is smoother)

    uint64_t totalFrameNumber_ = 0;
    bool     shaderHotReloadEnabled_ = true;
    float    shaderWatchIntervalSec_ = 1.0f; // once per second
    float    shaderWatchAccumSec_ = 0.0f;
    bool     materialsHotReloaded_ = false;

    // D3D12 core (device/queue), presentation surface, frame pacing
    GraphicsDevice                    graphicsDevice_;
    SwapchainManager                  swapchain_;
    FrameScheduler                    frameScheduler_;

    UINT                              currentFrameIndex_ = 0;                   // 0..render::kFrameCount-1
    FrameResource*                    currentFrameResource_ = nullptr;
    static constexpr UINT             kFrameShaderVisibleHeapCount_ = 2;
    std::array<ID3D12DescriptorHeap*, kFrameShaderVisibleHeapCount_> currentFrameDescriptorHeaps_{};
    UINT                              currentFrameDescriptorHeapCount_ = 0;

    // Resource-state tracking across parallel command-list recording
    ResourceStateTracker stateTracker_;

    // Streamline / DLSS integration
    sl::DLSSMode dlssMode_ = sl::DLSSMode::eBalanced;
    std::unique_ptr<DlssHandler> dlssHandler_;

    void UpdateDlssSettings();
    void AllocateDlssResourcesIfNeeded();


    SamplerManager samplerManager_;
    MaterialManager materialManager_;
    InputLayoutManager inputLayoutManager_;
    MeshManager meshManager_;
    FontManager fontManager_;
    TextManager textManager_;
    MaterialDataManager materialDataManager_;
    RenderContextPool ctxPool_;
    DebugDrawSystem debugDrawSystem_;
    ImGuiLayer imguiLayer_;

    friend class DlssHandler;
};
