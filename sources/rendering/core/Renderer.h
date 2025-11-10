#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>

#include "core/containers/inl_vector.h"
#include "core/math/Math.h"
#include "rendering/descriptors/DescriptorAllocator.h"
#include "rendering/core/FrameResource.h"
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
    struct DeferredTargets {
        static constexpr size_t kResourceCount = 16; // gb0,gb1,gb2,gbVelocity,depth,depthCopy,light,scene,sceneOpaque,tonemap,fxaa,ssr,ssrBlur,shadow,spotShadow,dlssOutput
        // Resources
        ComPtr<ID3D12Resource> gb0;   // Renderer::kGBuffer0Format (albedo+metal)
        ComPtr<ID3D12Resource> gb1;   // Renderer::kGBuffer1Format (normalOcta+rough)
        ComPtr<ID3D12Resource> gb2;   // Renderer::kGBuffer2Format (emissive)
        ComPtr<ID3D12Resource> gbVelocity; // Renderer::kGBufferVelocityFormat (motion vectors)
        ComPtr<ID3D12Resource> depth; // Renderer::kDeferredDepthFormat
        ComPtr<ID3D12Resource> depthCopy; // Copy of depth before transparent pass
        ComPtr<ID3D12Resource> light; // Renderer::kLightTargetFormat
        ComPtr<ID3D12Resource> scene; // Renderer::kSceneColorFormat
        ComPtr<ID3D12Resource> sceneOpaque; // Copy of opaque scene color for refraction
        ComPtr<ID3D12Resource> tonemap; // Tonemap output (R8G8B8A8)
        ComPtr<ID3D12Resource> fxaa;    // FXAA output (R8G8B8A8)
        ComPtr<ID3D12Resource> ssr;     // Renderer::kSsrFormat (premultiplied)
        ComPtr<ID3D12Resource> ssrBlur; // Renderer::kSsrBlurFormat
        ComPtr<ID3D12Resource> shadow; // R32_TYPELESS (DSV=D32F, SRV=R32F)
        ComPtr<ID3D12Resource> spotShadow; // R32_TYPELESS array for spot lights
        ComPtr<ID3D12Resource> dlssOutput; // Renderer::kSceneColorFormat upscaled

        // CPU descriptors
        D3D12_CPU_DESCRIPTOR_HANDLE gbRTV[4]{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        D3D12_CPU_DESCRIPTOR_HANDLE gbSRV[4]{}; // GB0,GB1,GB2,GBVelocity
        D3D12_CPU_DESCRIPTOR_HANDLE depthSRV{};  // Depth(R32F)
        D3D12_CPU_DESCRIPTOR_HANDLE depthCopySRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE lightRTV{}, lightSRV{}, lightUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRTV{}, sceneSRV{}, sceneUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneOpaqueSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE tonemapSRV{}, tonemapUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE fxaaSRV{}, fxaaUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE ssrSRV{}, ssrUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE ssrBlurSRV{}, ssrBlurUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDSV{}, shadowSRV{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, LightManager::kMaxSpotLights> spotShadowDSV{};
        D3D12_CPU_DESCRIPTOR_HANDLE spotShadowSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE dlssOutputSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE dlssOutputUAV{};

        UINT shadowRes = 4096; // atlas 4096x4096, tile size 2048
        UINT spotShadowRes = 512;
    };

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

    // Formats
    static constexpr DXGI_FORMAT kBackbufferResourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    static constexpr DXGI_FORMAT kDepthBufferResourceFormat = DXGI_FORMAT_D32_FLOAT;
    static constexpr DXGI_FORMAT kDepthBufferViewFormat = DXGI_FORMAT_D32_FLOAT;
    static constexpr DXGI_FORMAT kDeferredDepthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    static constexpr DXGI_FORMAT kDeferredDepthSrvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    static constexpr DXGI_FORMAT kGBuffer0Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT kGBuffer1Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    static constexpr DXGI_FORMAT kGBuffer2Format = DXGI_FORMAT_R11G11B10_FLOAT;
    static constexpr DXGI_FORMAT kGBufferVelocityFormat = DXGI_FORMAT_R16G16_FLOAT;
    static constexpr DXGI_FORMAT kLightTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT kSsrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT kSsrBlurFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr UINT kConstantBufferAlignment = 256u;

    // Formats
    DXGI_FORMAT GetLightTargetFormat() const { return kLightTargetFormat; }
    DXGI_FORMAT GetSceneColorFormat() const { return kSceneColorFormat; }
    DXGI_FORMAT GetBackbufferFormat() const { return kBackbufferFormat; }
    DXGI_FORMAT GetBackbufferResourceFormat() const { return kBackbufferResourceFormat; }
    DXGI_FORMAT GetDsvFormat() const { return kDeferredDepthFormat; }
    DXGI_FORMAT GetDepthSrvFormat() const { return kDeferredDepthSrvFormat; }
    DXGI_FORMAT GetSsrFormat() const { return kSsrFormat; }
    DXGI_FORMAT GetSsrBlurFormat() const { return kSsrBlurFormat; }

    const DeferredTargets& GetDeferredForFrame() const { return deferred_[currentFrameIndex_]; }

    // Utility functions
    void WaitForPreviousFrame();       // full synchronization (used during resize/destruction)
    void OnResize(UINT width, UINT height);

    ThreadCL BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE type, ID3D12PipelineState* initialPSO = nullptr);
    void EndThreadCommandList(ThreadCL& t, size_t batchIndex);
    ThreadCL BeginThreadCommandBundle(ID3D12PipelineState* initialPSO = nullptr);
    void EndThreadCommandBundle(ThreadCL& b, size_t batchIndex);

    void BeginSubmitTimeline();
    size_t BeginSubmitBatch();
    void ExecuteTimelineAndPresent();
    void RecordBindAndClear(ID3D12GraphicsCommandList* cl);
    void RecordBindDefaultsNoClear(ID3D12GraphicsCommandList* cl);
    void RegisterPassDriver(ID3D12GraphicsCommandList* cl, size_t batchIndex);

    // Getters
    ID3D12Device* GetDevice() const { return device_.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return commandQueue_.Get(); }
    HWND GetHWND() const { return hWnd_; }
    UINT GetWidth() const { return width_; }
    UINT GetHeight() const { return height_; }
    UINT GetRenderWidth() const { return renderWidth_; }
    UINT GetRenderHeight() const { return renderHeight_; }
    float GetRenderResolutionScale() const { return renderResolutionScale_; }

    ID3D12Resource* GetCurrentBackbuffer() const { return currentFrameIndex_ < kFrameCount ? renderTargets_[currentFrameIndex_].Get() : nullptr; }

    // Access the global descriptor allocator and current frame
    DescriptorAllocator& GetDescAlloc() { return frameResources_[currentFrameIndex_]->GetDescAlloc(); }
    DescriptorAllocator& GetSamplerAlloc() { return frameResources_[currentFrameIndex_]->GetSamplerAlloc(); }
    FrameResource* GetFrameResource() { return frameResources_[currentFrameIndex_].get(); }

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
    D3D12_CPU_DESCRIPTOR_HANDLE GetTonemapSourceSrvCPU() const;

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
            device_->CopyDescriptorsSimple(1, dst, src, heapType);
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

    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvAt(UINT idx) const;

private:
    static constexpr UINT kFrameCount = 2;

    enum class DeferredRtvSlot : UINT { GB0, GB1, GB2, GBVelocity, Light, Scene, Count };
    enum class DeferredSrvSlot : UINT { GB0, GB1, GB2, GBVelocity, Depth, DepthCopy, Light, LightUAV, Scene, SceneUAV, SceneOpaque, SSR, SSRBlur, Shadow, SpotShadow, SSRUAV, SSRBlurUAV, Tonemap, TonemapUAV, Fxaa, FxaaUAV, DLSSOutput, DLSSOutputUAV, Count };
    enum class DeferredDsvSlot : UINT { Depth, Shadow, Count };

    static constexpr UINT kDeferredRtvPerFrame = (UINT)DeferredRtvSlot::Count;
    static constexpr UINT kDeferredSrvPerFrame = (UINT)DeferredSrvSlot::Count;
    static constexpr UINT kDeferredDsvPerFrame = (UINT)DeferredDsvSlot::Count + LightManager::kMaxSpotLights;

    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvCPU(UINT frame, DeferredRtvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvCPU(UINT frame, DeferredSrvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvCPU(UINT frame, DeferredDsvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSpotShadowDsvCPU(UINT frame, UINT lightIndex) const;

    D3D12_RESOURCE_STATES GetGlobalKnownState(ID3D12Resource* res);
    void RegisterCurrentThreadCL(ID3D12GraphicsCommandList* cl);
    void UnregisterCurrentThreadCL();

    static constexpr size_t kMaxNumCLsInPass = 8;
    struct PassBatch_ {
        ID3D12GraphicsCommandList* driver = nullptr;              // DIRECT
        tc::inl_vector<ID3D12GraphicsCommandList*, kMaxNumCLsInPass> bundles;          // TYPE_BUNDLE
        tc::inl_vector<ID3D12CommandList*, kMaxNumCLsInPass> directs;          // ready DIRECT command lists
    };
    std::vector<PassBatch_> submitTimeline_;
    std::mutex submitMtx_;
    std::vector<ID3D12CommandList*> submitListsScratch_;
    std::vector<ID3D12CommandList*> fixedSubmitScratch_;
    std::vector<D3D12_RESOURCE_BARRIER> barrierScratch_;

    // CPU heaps for offscreen resources
    ComPtr<ID3D12DescriptorHeap> deferredRtvHeap_;   // RTV shared across frames
    ComPtr<ID3D12DescriptorHeap> deferredDsvHeap_;   // DSV shared across frames
    ComPtr<ID3D12DescriptorHeap> deferredSrvCpuHeap_;// SRV CPU-only
    UINT deferredRtvIncr_ = 0, deferredDsvIncr_ = 0, deferredSrvIncr_ = 0;

    // Per-frame sets
    DeferredTargets deferred_[kFrameCount];

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

    // D3D12 core
    ComPtr<ID3D12Device>              device_;
    ComPtr<ID3D12CommandQueue>        commandQueue_;
    ComPtr<IDXGISwapChain3>           swapChain_;

    // RTV/DSV
    ComPtr<ID3D12DescriptorHeap>      rtvHeap_;
    ComPtr<ID3D12Resource>            renderTargets_[kFrameCount];
    UINT                              rtvDescriptorSize_ = 0;

    ComPtr<ID3D12DescriptorHeap>      dsvHeap_;
    ComPtr<ID3D12Resource>            depthBuffer_;
    UINT                              dsvDescriptorSize_ = 0;

    // Synchronization
    ComPtr<ID3D12Fence>               fence_;
    HANDLE                            fenceEvent_ = nullptr;
    UINT64                            nextFenceValue_ = 1;                  // global increment
    UINT64                            frameFenceValues_[kFrameCount] = {};  // last signal value for each frame

    // Frame resources (allocator + upload, etc.)
    std::unique_ptr<FrameResource>    frameResources_[kFrameCount];
    UINT                              currentFrameIndex_ = 0;                   // 0..kFrameCount-1
    FrameResource*                    currentFrameResource_ = nullptr;
    static constexpr UINT             kFrameShaderVisibleHeapCount_ = 2;
    std::array<ID3D12DescriptorHeap*, kFrameShaderVisibleHeapCount_> currentFrameDescriptorHeaps_{};
    UINT                              currentFrameDescriptorHeapCount_ = 0;

    std::mutex knownStatesMtx_;
    robin_hood::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> knownStates_;
    struct CLState {
        // First required resource state in this command list (not barriered inside the CL)
        robin_hood::unordered_flat_map<ID3D12Resource*, D3D12_RESOURCE_STATES> firstUse;
        // Current (latest) resource state within THIS command list (for transitions and final state)
        robin_hood::unordered_flat_map<ID3D12Resource*, D3D12_RESOURCE_STATES> current;
    };
    struct CLStateEntry {
        ID3D12CommandList* cmd = nullptr;
        CLState st;
        uint64_t epoch = 0;
    };

    static constexpr uint32_t kCLStateLanes = 64;
    //struct CLStateLane { std::vector<CLStateEntry> entries; };
    struct CLStateLane {
        robin_hood::unordered_flat_map<ID3D12CommandList*, CLStateEntry> entries;
        uint64_t epoch = 0;
    };

    std::atomic<uint32_t> clLaneCount_{ 0 };
    CLStateLane           clLanes_[kCLStateLanes];

    // TLS: which lane the thread uses and which CL is currently active
    static thread_local uint32_t      tlLaneIndex_;
    static thread_local CLStateEntry* tlCurrentEntry_;

    // Streamline / DLSS integration
    sl::DLSSMode dlssMode_ = sl::DLSSMode::eBalanced;
    std::unique_ptr<DlssHandler> dlssHandler_;

    void UpdateDlssSettings();
    void AllocateDlssResourcesIfNeeded();

    const CLState* FindCLStateForCmd(ID3D12CommandList* cmd) const;

    SamplerManager samplerManager_;
    MaterialManager materialManager_;
    InputLayoutManager inputLayoutManager_;
    MeshManager meshManager_;
    FontManager fontManager_;
    TextManager textManager_;
    MaterialDataManager materialDataManager_;
    RenderContextPool ctxPool_;
    DebugDrawSystem debugDrawSystem_;

    friend class DlssHandler;
};
