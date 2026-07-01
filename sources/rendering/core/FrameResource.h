#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <vector>
#include "rendering/descriptors/DescriptorAllocator.h"

using Microsoft::WRL::ComPtr;

class FrameResource {
public:
    struct DynamicAlloc {
        void* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        UINT size = 0;
        UINT offset = 0; // offset within the resource
    };

    // Initialize or reinitialize the frame upload buffer (persistent MAP)
    void InitUpload(ID3D12Device* dev, UINT bytes);

    // Reset the offset at the start of the frame (after waiting for the fence)
    void ResetUpload();

    // Thread-safe linear allocation from the frame upload buffer.
// align must be a power of two (defaults to 16).
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
        return 0; // fallback to Direct
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
                used[qi].store(0u, std::memory_order_relaxed);
            }
        }

        ID3D12CommandAllocator* Acquire(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type) {
            const int qi = QueueIndex_(type);
            const UINT index = used[qi].fetch_add(1u, std::memory_order_relaxed);
            auto& queuePool = pools[qi];

            // Fast path: an allocator with this index already exists.
            if (index < queuePool.size()) {
                return queuePool[index].Get();
            }
            else {
                // Slow path: grow the vector while holding only this queue's lock.
                std::lock_guard<std::mutex> lk(mtx[qi]);
                // Another thread may have grown it already—check again.
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
        std::atomic<uint32_t> epoch_{ 0 };

        static constexpr UINT kChunkSize_ = 8u;

        void ResetUsage() {
            for (int qi = 0; qi < kQueueCount_; ++qi) {
                used[qi].store(0u, std::memory_order_relaxed);
            }
            epoch_.fetch_add(1u, std::memory_order_release);
        }

        ID3D12GraphicsCommandList* Acquire(ID3D12Device* dev,
            D3D12_COMMAND_LIST_TYPE type,
            ID3D12CommandAllocator* alloc,
            ID3D12PipelineState* initialPSO = nullptr) {
            const int qi = QueueIndex_(type);

#if USE_CL_THREAD_CACHE
            struct ThreadCache_ {
                CommandListPools_* owner = nullptr;
                uint32_t epoch = 0;
                UINT next = 0;
                UINT end = 0;
            };

            static thread_local ThreadCache_ cache[kQueueCount_];
            ThreadCache_& entry = cache[qi];

            const uint32_t epoch = epoch_.load(std::memory_order_acquire);
            if (entry.owner != this || entry.epoch != epoch) {
                entry.owner = this;
                entry.epoch = epoch;
                entry.next = 0;
                entry.end = 0;
            }

            if (entry.next == entry.end) {
                const UINT start = used[qi].fetch_add(kChunkSize_, std::memory_order_relaxed);
                entry.next = start;
                entry.end = start + kChunkSize_;
            }
            const UINT index = entry.next++;
#else
            const UINT index = used[qi].fetch_add(1u, std::memory_order_relaxed);
#endif
            auto& vec = pools[qi];

            if (index >= vec.size()) {
                std::lock_guard<std::mutex> lk(mtx[qi]);
                while (index >= vec.size()) {
                    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
                    if (FAILED(dev->CreateCommandList(0, type, alloc, initialPSO, IID_PPV_ARGS(&cl)))) {
                        throw std::runtime_error("CreateCommandList failed");
                    }
                    // Close immediately—we reset on every Acquire anyway
                    if (FAILED(cl->Close())) {
                        throw std::runtime_error("CreateCommandList Close failed");
                    }
                    vec.push_back(cl);
                }
            }

            ID3D12GraphicsCommandList* cl = vec[index].Get();
            const HRESULT resetHr = cl->Reset(alloc, initialPSO);
            if (FAILED(resetHr)) {
                // Reset fails on (a) device removal (a GPU fault upstream — then
                // every later D3D12 call fails) or (b) a command list left in the
                // recording state (a pass opened it but didn't close it). Surface
                // both so this is diagnosable instead of an opaque "Reset failed".
                const HRESULT removedReason = dev->GetDeviceRemovedReason();
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                    "CommandList Reset failed: hr=0x%08X deviceRemovedReason=0x%08X",
                    static_cast<unsigned int>(resetHr), static_cast<unsigned int>(removedReason));
                throw std::runtime_error(msg);
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

    // Fallback chunks (used when the main buffer overflows). They live until the end of the frame.
    struct UploadChunk_ {
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        void* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        UINT size = 0;
        std::atomic<UINT> offset{ 0 };

        UploadChunk_() = default;

        // Disable copying
        UploadChunk_(const UploadChunk_&) = delete;
        UploadChunk_& operator=(const UploadChunk_&) = delete;

        // Allow moves
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