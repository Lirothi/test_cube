#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <stdexcept>
#include <atomic>

using Microsoft::WRL::ComPtr;

struct GpuDescHandle {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    uint32_t index = 0;
};

class DescriptorHeapGPU {
public:
    DescriptorHeapGPU() = default;

    void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible) {
        device_ = device;
        type_ = type;
        capacity_ = capacity;

        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = capacity_;
        desc.Type = type;
        desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)))) {
            throw std::runtime_error("CreateDescriptorHeap failed");
        }
        incr_ = device_->GetDescriptorHandleIncrementSize(type_);
        startCPU_ = heap_->GetCPUDescriptorHandleForHeapStart();
        startGPU_ = shaderVisible ? heap_->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{};
        cursor_ = 0;
    }

    GpuDescHandle Allocate(uint32_t count = 1) {
        uint32_t old = cursor_.load(std::memory_order_relaxed);
        for (;;) {
            if (old + count > capacity_) {
                throw std::runtime_error("DescriptorHeapGPU overflow");
            }
            if (cursor_.compare_exchange_weak(old, old + count, std::memory_order_relaxed)) {
                GpuDescHandle h{};
                h.index = old;
                h.cpu.ptr = startCPU_.ptr + SIZE_T(old) * incr_;
                h.gpu.ptr = startGPU_.ptr ? (startGPU_.ptr + UINT64(old) * incr_) : 0;
                return h;
            }
            // old обновится значением cursor_, цикл повторится
        }
    }

    void Reset() {
        cursor_ = 0;
    }

    ID3D12DescriptorHeap* GetHeap() const {
        return heap_.Get();
    }
    UINT GetDescriptorSize() const {
        return incr_;
    }

private:
    ID3D12Device* device_ = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE type_{};
    ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE startCPU_{};
    D3D12_GPU_DESCRIPTOR_HANDLE startGPU_{};
    UINT incr_ = 0;
    uint32_t capacity_ = 0;
    std::atomic<uint32_t> cursor_{ 0 };
};