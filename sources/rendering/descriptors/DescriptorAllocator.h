#pragma once
#include <atomic>
#include <cstdint>
#include "rendering/descriptors/DescriptorHeapGPU.h"
#include "rendering/core/MemoryReport.h"

namespace render {
// --upload-ring-check (FrameResource.cpp): a table staged into a frame's descriptor ring after the frame sealed it.
void ReportLateDescriptorStage(std::uint32_t count, std::uint32_t used, std::uint64_t frame);
} // namespace render

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
    GpuDescHandle Alloc() { NoteIfSealed_(1u); return heap_.Allocate(1); }
    GpuDescHandle Alloc(uint32_t n) { NoteIfSealed_(n); return heap_.Allocate(n); }
    // --upload-ring-check: the frame's recording has ended (FrameResource::SealUpload); a table staged here before the
    // slot's next ResetPerFrame is reported. Only the check ever seals, so otherwise Alloc tests one false flag.
    void SealForCheck(std::uint64_t frame) {
        sealedFrame_.store(frame, std::memory_order_relaxed);
        sealed_.store(true, std::memory_order_release);
    }
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
        sealed_.store(false, std::memory_order_release);
    }
    ID3D12DescriptorHeap* GetShaderVisibleHeap() const {return heap_.GetHeap();}
    UINT GetIncr() const {return heap_.GetDescriptorSize();}

private:
    void NoteIfSealed_(uint32_t n) {
        if (sealed_.load(std::memory_order_acquire)) {
            render::ReportLateDescriptorStage(n, heap_.Used(), sealedFrame_.load(std::memory_order_relaxed));
        }
    }

    DescriptorHeapGPU heap_;
    std::atomic<bool> sealed_{ false };
    std::atomic<std::uint64_t> sealedFrame_{ 0 };
};
