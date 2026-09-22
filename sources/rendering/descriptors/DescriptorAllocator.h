#pragma once
#include "rendering/descriptors/DescriptorHeapGPU.h"

// Simple facade: a single global shader-visible heap for CBV/SRV/UAV.
// Transient usage: call Reset() once per frame.
class DescriptorAllocator {
public:
    void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE HeapType, uint32_t capacity = 4096) {
        heap_.Init(device, HeapType, capacity, true);
    }
    // A6: a partition of a shared shader-visible heap (render::BindlessHeap's frame ring).
    void InitView(ID3D12DescriptorHeap* heap, D3D12_DESCRIPTOR_HEAP_TYPE HeapType, uint32_t base, uint32_t capacity, UINT incr) {
        heap_.InitView(heap, HeapType, base, capacity, incr);
    }
    GpuDescHandle Alloc() {return heap_.Allocate(1);}
    GpuDescHandle Alloc(uint32_t n) { return heap_.Allocate(n); }
    void ResetPerFrame() {heap_.Reset();}
    ID3D12DescriptorHeap* GetShaderVisibleHeap() const {return heap_.GetHeap();}
    UINT GetIncr() const {return heap_.GetDescriptorSize();}

private:
    DescriptorHeapGPU heap_;
};
