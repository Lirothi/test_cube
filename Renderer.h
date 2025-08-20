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

using Microsoft::WRL::ComPtr;

class Renderer {
public:
    struct ThreadCL {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cl = nullptr;
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    };

    Renderer();
    ~Renderer();

    // Инициализация устройства/очереди/свапа/RTV/DSV + кадровые ресурсы/фэнс
    void InitD3D12(HWND window);
    void InitFence(); // оставлено для совместимости, делает ничего если уже инициализировано

    // Кадровой цикл
    void BeginFrame();                 // ждёт свой кадр, ресетит allocator и командный лист
    void EndFrame();                   // барьер RT->Present, Execute, Present, сигнал фэнса

    void Update(float dt);

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
    DescriptorAllocator& GetDescAlloc() { return frameResources_[currentFrameIndex_].GetDescAlloc(); }
    DescriptorAllocator& GetSamplerAlloc() { return frameResources_[currentFrameIndex_].GetSamplerAlloc(); }
    FrameResource& GetFrame() { return frameResources_[currentFrameIndex_]; }

    UINT GetCurrentFrameIndex() const { return currentFrameIndex_; }

    void InitTextSystem(ID3D12GraphicsCommandList* uploadCl, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive, const std::wstring& folder);

    SamplerManager& GetSamplerManager() { return samplerManager_; }
	ConstantBufferLayoutManager& GetCBManager() { return cbManager_; }
	MaterialManager& GetMaterialManager() { return materialManager_; }
	InputLayoutManager& GetInputLayoutManager() { return inputLayoutManager_; }
	MeshManager& GetMeshManager() { return meshManager_; }
    TextManager* GetTextManager() { return &textManager_; }
    FontManager* GetFontManager() { return &fontManager_; }

	float GetFPS() const { return fps_; }

    void SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state);
    void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after);
    void UAVBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res);

    template<class Alloc>
    inline GpuDescHandle StageDescriptorTable(
        Alloc& alloc,                                        // DescriptorAllocator ИЛИ DescriptorAllocatorSampler
        D3D12_DESCRIPTOR_HEAP_TYPE heapType,                 // D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV или _SAMPLER
        std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> srcCpuHandles)
    {
        const UINT n = static_cast<UINT>(srcCpuHandles.size());
        if (n == 0) {
            return {}; // пусто
        }

        // 1) Выделяем подряд n слотов в shader-visible heap
        GpuDescHandle block = alloc.Alloc(n);
        const UINT incr = alloc.GetIncr();

        // 2) Копируем дескрипторы «стенка-в-стенку»
        D3D12_CPU_DESCRIPTOR_HANDLE dst = block.cpu;
        for (auto h : srcCpuHandles) {
            device_->CopyDescriptorsSimple(1, dst, h, heapType);
            dst.ptr += incr;
        }

        // 3) Возвращаем старт таблицы (gpu указывает на t0/s0 и т.д.)
        return block;
    }

    // Удобные врапперы под твой Renderer с глобальными аллокаторами
    inline GpuDescHandle StageSrvUavTable(std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> srcCpuHandles)
    {
        return StageDescriptorTable(GetDescAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, srcCpuHandles);
    }

    inline GpuDescHandle StageSamplerTable(std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> srcCpuHandles)
    {
        return StageDescriptorTable(GetSamplerAlloc(),D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, srcCpuHandles);
    }

private:
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void CreateSwapChainAndRTVs(UINT width, UINT height);
    void CreateDepthResources(UINT width, UINT height);
    void WaitForFrame(UINT frameIndex);   // ожидание конкретного кадра (по fence value кадра)
    void SignalFrame(UINT frameIndex);    // сигнал на фэнсе для кадра

private:
    static constexpr UINT kFrameCount = 3;

    struct PassBatch_ {
        std::string name;
        ID3D12GraphicsCommandList* driver = nullptr;              // DIRECT
        std::vector<ID3D12GraphicsCommandList*> bundles;          // TYPE_BUNDLE
        std::vector<ID3D12CommandList*>         directs;          // готовые DIRECT-CL
    };
    std::vector<PassBatch_> submitTimeline_;
    std::mutex submitMtx_;

    // OS / размеры
    HWND  hWnd_ = nullptr;
    UINT  width_ = 1600;
    UINT  height_ = 900;

    float fps_ = 0.0f;
    float fpsAlpha_ = 0.95f; // экспоненциальное сглаживание: 0..1 (чем больше — тем плавнее)

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
    FrameResource                     frameResources_[kFrameCount];
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
};
