#pragma once
#include "DescriptorHeapGPU.h"

// Простой фасад: один глобальный shader-visible heap для CBV/SRV/UAV.
// Транзиентная схема: Reset() раз в кадр.
class DescriptorAllocator {
public:
    void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE HeapType, uint32_t capacity = 4096) {
        heap_.Init(device, HeapType, capacity, true);
    }
    GpuDescHandle Alloc() {return heap_.Allocate(1);}
    GpuDescHandle Alloc(uint32_t n) { return heap_.Allocate(n); }
    void ResetPerFrame() {heap_.Reset();}
    ID3D12DescriptorHeap* GetShaderVisibleHeap() const {return heap_.GetHeap();}
    UINT GetIncr() const {return heap_.GetDescriptorSize();}

private:
    DescriptorHeapGPU heap_;
};
