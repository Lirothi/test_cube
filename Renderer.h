#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "DescriptorAllocator.h"
#include "FrameResource.h"
#include <unordered_map>
#include "Samplermanager.h"
#include "CBManager.h"
#include "Material.h"
#include "InputLayoutManager.h"
#include "MeshManager.h"
#include "TextManager.h"
#include "FontManager.h"
#include "MaterialDataManager.h"

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
        // ресурсы
        ComPtr<ID3D12Resource> gb0;   // R8G8B8A8 (albedo+metal)
        ComPtr<ID3D12Resource> gb1;   // R10G10G10A2 (normalOcta+rough)
        ComPtr<ID3D12Resource> gb2;   // R11G11B10 (emissive)
        ComPtr<ID3D12Resource> depth; // D32
        ComPtr<ID3D12Resource> light; // R16G16B16A16F
        ComPtr<ID3D12Resource> scene; // R16G16B16A16F
        ComPtr<ID3D12Resource> ssr;     // R16G16B16A16F premultiplied
        ComPtr<ID3D12Resource> ssrBlur; // R16G16B16A16F

        // CPU дескрипторы
        D3D12_CPU_DESCRIPTOR_HANDLE gbRTV[3]{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        D3D12_CPU_DESCRIPTOR_HANDLE gbSRV[4]{}; // GB0,GB1,GB2,Depth(R32F)
        D3D12_CPU_DESCRIPTOR_HANDLE lightRTV{}, lightSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRTV{}, sceneSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE ssrRTV{}, ssrSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE ssrBlurRTV{}, ssrBlurSRV{};
    };

    Renderer();
    ~Renderer();
    void Shutdown();
    void ReportLiveObjects();

    // Инициализация устройства/очереди/свапа/RTV/DSV + кадровые ресурсы/фэнс
    void InitD3D12(HWND window);
    void InitFence(); // оставлено для совместимости, делает ничего если уже инициализировано

    // Кадровой цикл
    void BeginFrame();                 // ждёт свой кадр, ресетит allocator и командный лист
    void EndFrame();                   // барьер RT->Present, Execute, Present, сигнал фэнса

    void Tick(float dt);

    void CreateDeferredTargets(UINT width, UINT height);
    void DestroyDeferredTargets();

    void BindGBuffer(ID3D12GraphicsCommandList* cl, ClearMode mode);
    void BindLightTarget(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindSceneColor(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindSSRTarget(ID3D12GraphicsCommandList* cl, ClearMode mode);
    void BindSSRBlurTarget(ID3D12GraphicsCommandList* cl, ClearMode mode);

    // готовые SRV-таблицы (в shader-visible heap кадра)
    D3D12_GPU_DESCRIPTOR_HANDLE StageGBufferSrvTable(); // t0..t3 : GB0,GB1,GB2,Depth
    D3D12_GPU_DESCRIPTOR_HANDLE StageComposeSrvTable(); // t0..t1 : Light,GB2
    D3D12_GPU_DESCRIPTOR_HANDLE StageTonemapSrvTable(); // t0     : Scene

    // форматы
    DXGI_FORMAT GetLightTargetFormat() const { return DXGI_FORMAT_R16G16B16A16_FLOAT; }
    DXGI_FORMAT GetSceneColorFormat() const { return DXGI_FORMAT_R16G16B16A16_FLOAT; }
    DXGI_FORMAT GetBackbufferFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }

    const DeferredTargets& GetDeferredForFrame() const { return deferred_[currentFrameIndex_]; }

    // Сервис
    void WaitForPreviousFrame();       // полная синхронизация (используется при ресайзе/деструкторе)
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

    // Геттеры
    ID3D12Device* GetDevice() const { return device_.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return commandQueue_.Get(); }
    HWND GetHWND() const { return hWnd_; }
    UINT GetWidth() const { return width_; }
    UINT GetHeight() const { return height_; }

    // Доступ к глобальному аллокатору дескрипторов и текущему кадру
    DescriptorAllocator& GetDescAlloc() { return frameResources_[currentFrameIndex_]->GetDescAlloc(); }
    DescriptorAllocator& GetSamplerAlloc() { return frameResources_[currentFrameIndex_]->GetSamplerAlloc(); }
    FrameResource* GetFrameResource() { return frameResources_[currentFrameIndex_].get(); }

    UINT GetCurrentFrameIndex() const { return currentFrameIndex_; }

    void InitTextSystem(ID3D12GraphicsCommandList* uploadCl, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive, const std::wstring& folder);

    SamplerManager* GetSamplerManager() { return &samplerManager_; }
	ConstantBufferLayoutManager* GetCBManager() { return &cbManager_; }
	MaterialManager* GetMaterialManager() { return &materialManager_; }
	InputLayoutManager* GetInputLayoutManager() { return &inputLayoutManager_; }
	MeshManager* GetMeshManager() { return &meshManager_; }
    TextManager* GetTextManager() { return &textManager_; }
    FontManager* GetFontManager() { return &fontManager_; }
    MaterialDataManager* GetMaterialDataManager() { return &materialDataManager_; }

	float GetFPS() const { return fps_; }
    void SetWireframeMode(bool w) { wireframeMode_ = w; }
    bool GetWireframeMode() const { return wireframeMode_; }

    void SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state);
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
    void WaitForFrame(UINT frameIndex);   // ожидание конкретного кадра (по fence value кадра)
    void SignalFrame(UINT frameIndex);    // сигнал на фэнсе для кадра

    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvAt(UINT idx) const;

private:
    static constexpr UINT kFrameCount = 2;
    static constexpr UINT kDeferredRtvPerFrame = 7; // GB0,GB1,GB2, Light, Scene, SSR, SSRBlur
    static constexpr UINT kDeferredSrvPerFrame = 8; // GB0,GB1,GB2, Depth, Light, Scene, SSR, SSRBlur
    static constexpr UINT kDeferredDsvPerFrame = 1; // Depth

    enum class DeferredRtvSlot : UINT { GB0, GB1, GB2, Light, Scene, SSR, SSRBlur, Count = kDeferredRtvPerFrame };
    enum class DeferredSrvSlot : UINT { GB0, GB1, GB2, Depth, Light, Scene, SSR, SSRBlur, Count = kDeferredSrvPerFrame };
    enum class DeferredDsvSlot : UINT { Depth, Count = kDeferredDsvPerFrame };

    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvCPU(UINT frame, DeferredRtvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvCPU(UINT frame, DeferredSrvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvCPU(UINT frame, DeferredDsvSlot slot) const;

    struct PassBatch_ {
        std::string name;
        ID3D12GraphicsCommandList* driver = nullptr;              // DIRECT
        std::vector<ID3D12GraphicsCommandList*> bundles;          // TYPE_BUNDLE
        std::vector<ID3D12CommandList*>         directs;          // готовые DIRECT-CL
    };
    std::vector<PassBatch_> submitTimeline_;
    std::mutex submitMtx_;

    // Heaps CPU для offscreen-ресурсов
    ComPtr<ID3D12DescriptorHeap> deferredRtvHeap_;   // RTV: на все кадры
    ComPtr<ID3D12DescriptorHeap> deferredDsvHeap_;   // DSV: на все кадры
    ComPtr<ID3D12DescriptorHeap> deferredSrvCpuHeap_;// SRV CPU-only
    UINT deferredRtvIncr_ = 0, deferredDsvIncr_ = 0, deferredSrvIncr_ = 0;

    // per-frame наборы
    DeferredTargets deferred_[kFrameCount];

    // OS / размеры
    HWND  hWnd_ = nullptr;
    UINT  width_ = 1600;
    UINT  height_ = 900;

    bool wireframeMode_ = false;

    float fps_ = 0.0f;
    float fpsAlpha_ = 0.95f; // экспоненциальное сглаживание: 0..1 (чем больше — тем плавнее)

    uint64_t totalFrameNumber_ = 0;
    bool     shaderHotReloadEnabled_ = true;
    float    shaderWatchIntervalSec_ = 1.0f; // раз в секунду
    float    shaderWatchAccumSec_ = 0.0f;

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

    // Синхронизация
    ComPtr<ID3D12Fence>               fence_;
    HANDLE                            fenceEvent_ = nullptr;
    UINT64                            nextFenceValue_ = 1;                  // глобальный инкремент
    UINT64                            frameFenceValues_[kFrameCount] = {};  // последний сигнал для каждого кадра

    // Кадровые ресурсы (аллокатор + upload и т.п.)
	std::unique_ptr<FrameResource>    frameResources_[kFrameCount];
    UINT                              currentFrameIndex_ = 0;                   // 0..kFrameCount-1

    std::mutex knownStatesMtx_;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> knownStates_;

    SamplerManager samplerManager_;
    ConstantBufferLayoutManager cbManager_;
    MaterialManager materialManager_;
    InputLayoutManager inputLayoutManager_;
	MeshManager meshManager_;
    FontManager fontManager_;
    TextManager textManager_;
    MaterialDataManager materialDataManager_;
};
