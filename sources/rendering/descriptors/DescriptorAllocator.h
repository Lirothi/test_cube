#pragma once
#include "rendering/descriptors/DescriptorHeapGPU.h"
#include "rendering/core/MemoryReport.h"

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
    // A frame that filled its ring past three quarters says so before one fills it: an overflow throws (a crash), and
    // the 814 shadow views that staged a table each grew into one with nothing in the log beforehand (2026-10-27).
    void ResetPerFrame() {
        const uint32_t used = heap_.Used();
        if (heap_.Type() == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) { render::NoteDescriptorRingUse(used, heap_.Capacity()); }
        if (used * 4u > heap_.Capacity() * 3u) {
            LOG_WARNING_THROTTLED(std::chrono::seconds(10), logging::LogCategory::RenderRhi,
                "frame descriptor ring type={} used {} of {} last frame -- something stages a table per view or per draw",
                static_cast<unsigned>(heap_.Type()), used, heap_.Capacity());
        }
        heap_.Reset();
    }
    ID3D12DescriptorHeap* GetShaderVisibleHeap() const {return heap_.GetHeap();}
    UINT GetIncr() const {return heap_.GetDescriptorSize();}

private:
    DescriptorHeapGPU heap_;
};
