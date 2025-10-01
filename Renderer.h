#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>

#include "DescriptorAllocator.h"
#include "FrameResource.h"
#include "third_party/robin_hood.h"
#include "Samplermanager.h"
#include "Material.h"
#include "InputLayoutManager.h"
#include "MeshManager.h"
#include "TextManager.h"
#include "FontManager.h"
#include "MaterialDataManager.h"
#include "RenderContextPool.h"

using Microsoft::WRL::ComPtr;

class Renderer {
public:
    struct ThreadCL {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cl = nullptr;
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    };
    enum class ClearMode { None, Color, ColorDepth };
    struct DeferredTargets {
        // Resources
        ComPtr<ID3D12Resource> gb0;   // R8G8B8A8 (albedo+metal)
        ComPtr<ID3D12Resource> gb1;   // R10G10G10A2 (normalOcta+rough)
        ComPtr<ID3D12Resource> gb2;   // R11G11B10 (emissive)
        ComPtr<ID3D12Resource> depth; // D32
        ComPtr<ID3D12Resource> light; // R16G16B16A16F
        ComPtr<ID3D12Resource> scene; // R16G16B16A16F
        ComPtr<ID3D12Resource> ssr;     // R16G16B16A16F premultiplied
        ComPtr<ID3D12Resource> ssrBlur; // R16G16B16A16F
        ComPtr<ID3D12Resource> shadow; // R32_TYPELESS (DSV=D32F, SRV=R32F)

        // CPU descriptors
        D3D12_CPU_DESCRIPTOR_HANDLE gbRTV[3]{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        D3D12_CPU_DESCRIPTOR_HANDLE gbSRV[4]{}; // GB0,GB1,GB2,Depth(R32F)
        D3D12_CPU_DESCRIPTOR_HANDLE lightRTV{}, lightSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRTV{}, sceneSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE ssrRTV{}, ssrSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE ssrBlurRTV{}, ssrBlurSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDSV{}, shadowSRV{};

        UINT shadowRes = 4096; // atlas 4096x4096, tile size 2048
    };

    Renderer();
    ~Renderer();
    void Shutdown();
    void ReportLiveObjects();

    // Initialize device/queue/swap/RTV/DSV plus frame resources and the fence
    void InitD3D12(HWND window);
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
    void BindSSRTarget(ID3D12GraphicsCommandList* cl, ClearMode mode);
    void BindSSRBlurTarget(ID3D12GraphicsCommandList* cl, ClearMode mode);
    void BindShadowTarget(ID3D12GraphicsCommandList* cl, int cascadeIndex, bool clearDepth);

    // Prebuilt SRV tables (in the frame's shader-visible heap)
    D3D12_GPU_DESCRIPTOR_HANDLE StageGBufferSrvTable(); // t0..t3 : GB0,GB1,GB2,Depth
    D3D12_GPU_DESCRIPTOR_HANDLE StageComposeSrvTable(); // t0..t1 : Light,GB2
    D3D12_GPU_DESCRIPTOR_HANDLE StageTonemapSrvTable(); // t0     : Scene

    // Formats
    DXGI_FORMAT GetLightTargetFormat() const { return DXGI_FORMAT_R16G16B16A16_FLOAT; }
    DXGI_FORMAT GetSceneColorFormat() const { return DXGI_FORMAT_R16G16B16A16_FLOAT; }
    DXGI_FORMAT GetBackbufferFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
    DXGI_FORMAT GetDsvFormat() const { return DXGI_FORMAT_D32_FLOAT_S8X24_UINT; }

    const DeferredTargets& GetDeferredForFrame() const { return deferred_[currentFrameIndex_]; }

    // Utility functions
    void WaitForPreviousFrame();       // full synchronization (used during resize/destruction)
    void OnResize(UINT width, UINT height);

    ThreadCL BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE type, ID3D12PipelineState* initialPSO = nullptr);
    void EndThreadCommandList(ThreadCL& t, size_t batchIndex);
    ThreadCL BeginThreadCommandBundle(ID3D12PipelineState* initialPSO = nullptr);
    void EndThreadCommandBundle(ThreadCL& b, size_t batchIndex);

    void BeginSubmitTimeline();
    size_t BeginSubmitBatch(const std::string& passName);
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

	float GetFPS() const { return fps_; }
    void SetWireframeMode(bool w) { wireframeMode_ = w; }
    bool GetWireframeMode() const { return wireframeMode_; }

    void SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state);
    void ClearResourceState(ID3D12Resource* res);
    void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after);
    void UAVBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res);

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

    inline GpuDescHandle StageSrvUavTable(const std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>& src)
    {
        return StageDescriptorTableRange(GetDescAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, src.begin(), src.end());
    }

    inline GpuDescHandle StageSamplerTable(std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> src)
    {
        return StageDescriptorTableRange(GetSamplerAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, src.begin(), src.end());
    }

    inline GpuDescHandle StageSamplerTable(const std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>& src)
    {
        return StageDescriptorTableRange(GetSamplerAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, src.begin(), src.end());
    }

private:
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void CreateSwapChainAndRTVs(UINT width, UINT height);
    void CreateDepthResources(UINT width, UINT height);
    void WaitForFrame(UINT frameIndex);   // wait for a specific frame (by that frame's fence value)
    void SignalFrame(UINT frameIndex);    // signal the fence for a frame
    void RefreshCurrentFrameCaches();

    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvAt(UINT idx) const;

private:
    static constexpr UINT kFrameCount = 2;

    enum class DeferredRtvSlot : UINT { GB0, GB1, GB2, Light, Scene, SSR, SSRBlur, Count };
    enum class DeferredSrvSlot : UINT { GB0, GB1, GB2, Depth, Light, Scene, SSR, SSRBlur, Shadow, Count };
    enum class DeferredDsvSlot : UINT { Depth, Shadow, Count };

    static constexpr UINT kDeferredRtvPerFrame = (UINT)DeferredRtvSlot::Count;
    static constexpr UINT kDeferredSrvPerFrame = (UINT)DeferredSrvSlot::Count;
    static constexpr UINT kDeferredDsvPerFrame = (UINT)DeferredDsvSlot::Count;

    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvCPU(UINT frame, DeferredRtvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvCPU(UINT frame, DeferredSrvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvCPU(UINT frame, DeferredDsvSlot slot) const;

    D3D12_RESOURCE_STATES GetGlobalKnownState(ID3D12Resource* res);
    void RegisterCurrentThreadCL(ID3D12GraphicsCommandList* cl);
    void UnregisterCurrentThreadCL();

    struct PassBatch_ {
        std::string name;
        ID3D12GraphicsCommandList* driver = nullptr;              // DIRECT
        std::vector<ID3D12GraphicsCommandList*> bundles;          // TYPE_BUNDLE
        std::vector<ID3D12CommandList*>         directs;          // ready DIRECT command lists
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

    const CLState* FindCLStateForCmd(ID3D12CommandList* cmd) const;

    SamplerManager samplerManager_;
    MaterialManager materialManager_;
    InputLayoutManager inputLayoutManager_;
	MeshManager meshManager_;
    FontManager fontManager_;
    TextManager textManager_;
    MaterialDataManager materialDataManager_;
    RenderContextPool ctxPool_;
};
