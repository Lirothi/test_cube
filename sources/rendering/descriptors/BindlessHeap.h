#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <vector>

#include "rendering/core/RenderConstants.h"

namespace render {

// Texture streaming plan A6 (docs/texture_streaming_vt_plan.md): THE shader-visible CBV_SRV_UAV
// heap. D3D12 binds one such heap per command list, so everything a shader reaches by index has to
// live in the same object: the per-frame staging rings (what FrameResource used to own as three
// separate 4096-descriptor heaps), the RT bindless region (rt::BindlessTable's former private
// heap) and the immutable texture slots the raster shaders address through SM6.6
// ResourceDescriptorHeap[]. Layout, in absolute descriptor indices:
//
//   [0, kFrameCount * kFrameRingCapacity)     frame ring f at f * kFrameRingCapacity (reset per frame)
//   [kRtRegionBase, +kRtRegionCapacity)        rt::BindlessTable, indices exactly as before + base
//   [kTextureBase, kCapacity)                  Texture2D slots; kTextureBase itself = a null SRV
//
// A texture slot is IMMUTABLE once written. A texture whose SRV changes (a streaming swap, a mip
// fade clamp) takes a NEW slot and retires the old one, which becomes reusable kFrameCount frames
// later -- frames still in flight index the old slot, and rewriting it under them is the ABA race
// the plan (and memory gpu-helpers-have-fixed-shapes) forbids. UE's FD3D12BindlessResourceManager
// solves the same problem by versioning whole heaps ("has to handle renames on command lists");
// per-slot retirement is the same guarantee at descriptor granularity.
class BindlessHeap
{
public:
    static constexpr UINT kInvalidIndex = 0xFFFFFFFFu;
    static constexpr UINT kFrameRingCapacity = 4096;
    static constexpr UINT kRtRegionCapacity = 8192;
    static constexpr UINT kCapacity = 65536;
    static constexpr UINT kFrameRingBase = 0;
    static constexpr UINT kRtRegionBase = kFrameRingBase + kFrameRingCapacity * kFrameCount;
    static constexpr UINT kTextureBase = kRtRegionBase + kRtRegionCapacity;
    static constexpr UINT kNullTextureIndex = kTextureBase; // a null Texture2D SRV: samples as zero

    // The one instance, for owners that outlive nothing in particular (Texture2D retires its slot
    // from its destructor). Null before Init and after Shutdown; every call on it is then a no-op.
    static BindlessHeap* Instance() { return instance_; }

    bool Init(ID3D12Device* device);
    void Shutdown();
    bool Ready() const { return heap_ != nullptr; }
    // ResourceBindingTier 3 + HighestShaderModel >= 6.6: what ResourceDescriptorHeap[] in a raster
    // shader needs (the RtSmoke probe, made a property).
    bool DynamicResourcesSupported() const { return dynamicResources_; }

    ID3D12DescriptorHeap* Heap() const { return heap_.Get(); }
    UINT Increment() const { return incr_; }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index) const;
    UINT FrameRingBase(UINT frameIndex) const { return kFrameRingBase + frameIndex * kFrameRingCapacity; }

    // Texture slots. Copies `src` into a fresh slot and returns its absolute index, or
    // kInvalidIndex when the region is exhausted (logged once).
    UINT AllocateSlot(D3D12_CPU_DESCRIPTOR_HANDLE src);
    // Hands the slot back kFrameCount frames from now. Safe with kInvalidIndex.
    void RetireSlot(UINT index);
    // The exhaustion fallback ONLY: overwrites a live slot under frames that may still read it.
    void RewriteSlot(UINT index, D3D12_CPU_DESCRIPTOR_HANDLE src);
    // Frame boundary, after the frame slot's fence wait: retired slots whose frame came return to
    // the free list.
    void BeginFrame(uint64_t frameNo);

    struct Stats
    {
        UINT slotsUsed = 0;     // texture slots handed out and live
        UINT slotsFree = 0;     // on the free list
        UINT slotsRetired = 0;  // waiting for their frame
        UINT slotsCapacity = 0; // the texture region
    };
    Stats GetStats() const;

private:
    static BindlessHeap* instance_;

    ID3D12Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    UINT incr_ = 0;
    bool dynamicResources_ = false;
    bool exhaustedLogged_ = false;

    mutable std::mutex mtx_;
    UINT nextSlot_ = kTextureBase + 1; // slot kTextureBase is the null SRV
    UINT live_ = 0;
    std::vector<UINT> free_;
    struct Retired { UINT index; uint64_t frame; };
    std::vector<Retired> retired_;
    uint64_t frameNo_ = 0;
};

} // namespace render
