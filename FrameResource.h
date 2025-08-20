#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <vector>
#include "DescriptorAllocator.h"

using Microsoft::WRL::ComPtr;

class FrameResource {
public:
    struct DynamicAlloc {
        void* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        UINT size = 0;
        UINT offset = 0; // смещение внутри ресурса
    };

    // Инициализация/переинициализация upload-буфера кадра (персистентный MAP)
    void InitUpload(ID3D12Device* dev, UINT bytes);

    // Сброс смещения в начале кадра (после ожидания fence)
    void ResetUpload();

    // Потокобезопасная линейная аллокация из upload-буфера кадра.
// align должен быть степенью двойки (по умолчанию 16).
    DynamicAlloc AllocDynamic(UINT size, UINT align = 16);

    void ResetCommandAllocators(ID3D12Device* dev) {
        commandAllocPools_.ResetAll(dev);
    }
    ID3D12CommandAllocator* AcquireCommandAllocator(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type) {
        return commandAllocPools_.Acquire(dev, type);
    }

    void ResetCommandListsUsage() { clPools_.ResetUsage(); }
    ID3D12GraphicsCommandList* AcquireCommandList(ID3D12Device* dev,
        D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator* alloc,
        ID3D12PipelineState* pso = nullptr) {
        return clPools_.Acquire(dev, type, alloc, pso);
    }

    DescriptorAllocator& GetDescAlloc() { return descAlloc_; }
    DescriptorAllocator& GetSamplerAlloc() { return samplerAlloc_; }

private:
    static constexpr int kQueueCount_ = 4; // Direct, Compute, Copy, Bundle
    static int QueueIndex_(D3D12_COMMAND_LIST_TYPE t) {
        if (t == D3D12_COMMAND_LIST_TYPE_DIRECT) { return 0; }
        if (t == D3D12_COMMAND_LIST_TYPE_COMPUTE) { return 1; }
        if (t == D3D12_COMMAND_LIST_TYPE_COPY) { return 2; }
        if (t == D3D12_COMMAND_LIST_TYPE_BUNDLE) { return 3; }
        return 0; // fallback на Direct
    }

    struct CommandAllocPools_ {
        std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> pools[kQueueCount_];
        std::atomic<UINT> used[kQueueCount_] = { 0, 0, 0, 0 };
        std::mutex mtx[kQueueCount_];

        void ResetAll(ID3D12Device* dev) {
            for (int qi = 0; qi < kQueueCount_; ++qi) {
                std::lock_guard<std::mutex> lk(mtx[qi]);
                for (auto& a : pools[qi]) {
                    if (a) {
                        a->Reset();
                    }
                }
                used[qi] = 0;
            }
        }

        ID3D12CommandAllocator* Acquire(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type) {
            const int qi = QueueIndex_(type);
            const UINT index = used[qi].fetch_add(1, std::memory_order_acq_rel);
            auto& queuePool = pools[qi];

            // Быстрый путь: уже есть аллокатор с таким индексом.
            if (index < queuePool.size()) {
                return queuePool[index].Get();
            }
            else {
                // Медленный путь: нужно дорастить вектор. Лочим только свою очередь.
                std::lock_guard<std::mutex> lk(mtx[qi]);
                // Возможно, кто-то уже вырастил — проверим ещё раз.
                while (index >= queuePool.size()) {
                    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> ca;
                    if (FAILED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&ca)))) {
                        throw std::runtime_error("CreateCommandAllocator failed");
					}
                    queuePool.push_back(ca);
                }
                return queuePool[index].Get();
            }
        }
    };

    struct CommandListPools_ {
        std::vector<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>> pools[kQueueCount_];
        std::atomic<UINT> used[kQueueCount_] = { 0, 0, 0, 0 };
        std::mutex mtx[kQueueCount_];

        void ResetUsage() {
            for (int qi = 0; qi < kQueueCount_; ++qi) {
                used[qi].store(0u, std::memory_order_release);
            }
        }

        ID3D12GraphicsCommandList* Acquire(ID3D12Device* dev,
            D3D12_COMMAND_LIST_TYPE type,
            ID3D12CommandAllocator* alloc,
            ID3D12PipelineState* initialPSO = nullptr) {
            const int qi = QueueIndex_(type);
            const UINT index = used[qi].fetch_add(1u, std::memory_order_acq_rel);
            auto& vec = pools[qi];

            if (index >= vec.size()) {
                std::lock_guard<std::mutex> lk(mtx[qi]);
                while (index >= vec.size()) {
                    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
                    if (FAILED(dev->CreateCommandList(0, type, alloc, initialPSO, IID_PPV_ARGS(&cl)))) {
                        throw std::runtime_error("CreateCommandList failed");
					}
                    // Закроем сразу — мы всё равно Reset'им при каждом Acquire
                    if (FAILED(cl->Close())) {
                        throw std::runtime_error("CreateCommandList Close failed");
                    }
                    vec.push_back(cl);
                }
            }

            ID3D12GraphicsCommandList* cl = vec[index].Get();
            if (FAILED(cl->Reset(alloc, initialPSO))) {
                throw std::runtime_error("CommandList Reset failed");
			}
            return cl;
        }
    };

    DescriptorAllocator descAlloc_;
    DescriptorAllocator samplerAlloc_;
    
    CommandAllocPools_ commandAllocPools_;
    CommandListPools_ clPools_;

    // ==== Upload ring data ====
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_;
    void* uploadCPU_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS uploadGPU_ = 0;
    UINT uploadSize_ = 0;
    std::atomic<UINT> uploadOffset_{ 0 };

    // Фолбэк-чанки (если основной буфер переполнен). Живут до конца кадра.
    struct UploadChunk_ {
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        void* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        UINT size = 0;
        std::atomic<UINT> offset{ 0 };

        UploadChunk_() = default;

        // запрещаем копирование
        UploadChunk_(const UploadChunk_&) = delete;
        UploadChunk_& operator=(const UploadChunk_&) = delete;

        // разрешаем перемещение
        UploadChunk_(UploadChunk_&& o) noexcept
            : res(std::move(o.res)),
            cpu(o.cpu),
            gpu(o.gpu),
            size(o.size) {
            const UINT off = o.offset.load(std::memory_order_relaxed);
            offset.store(off, std::memory_order_relaxed);
            o.cpu = nullptr;
            o.gpu = 0;
            o.size = 0;
            o.offset.store(0u, std::memory_order_relaxed);
        }

        UploadChunk_& operator=(UploadChunk_&& o) noexcept {
            if (this != &o) {
                res = std::move(o.res);
                cpu = o.cpu;
                gpu = o.gpu;
                size = o.size;
                const UINT off = o.offset.load(std::memory_order_relaxed);
                offset.store(off, std::memory_order_relaxed);
                o.cpu = nullptr;
                o.gpu = 0;
                o.size = 0;
                o.offset.store(0u, std::memory_order_relaxed);
            }
            return *this;
        }
    };
    std::vector<UploadChunk_> extraUploads_;
    std::mutex uploadGrowMtx_;
};