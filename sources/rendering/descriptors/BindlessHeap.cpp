#include "rendering/descriptors/BindlessHeap.h"

#include "core/logging/Log.h"

#include <algorithm>

namespace render {

BindlessHeap* BindlessHeap::instance_ = nullptr;

bool BindlessHeap::Init(ID3D12Device* device)
{
    if (!device || heap_) { return heap_ != nullptr; }
    device_ = device;

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kCapacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_))) || !heap_)
    {
        heap_.Reset();
        LOG_ERROR(logging::LogCategory::Render, "[bindless] shader-visible heap of {} descriptors failed; per-frame heaps stay separate",
                  kCapacity);
        return false;
    }
    heap_->SetName(L"Bindless.Heap");
    incr_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The null texture slot: a Texture2D SRV over no resource samples as zero. Materials with a
    // missing map point their index here rather than at whatever the heap holds.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(nullptr, &sd, Cpu(kNullTextureIndex));
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_7 };
    const bool haveOptions = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
    const bool haveSm = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)));
    dynamicResources_ = haveOptions && haveSm &&
                        options.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_3 &&
                        static_cast<unsigned>(sm.HighestShaderModel) >= 0x66u;
    LOG_INFO(logging::LogCategory::Render,
             "[bindless] heap {} descriptors: frame ring {} x {}, rt region {} at {}, texture slots {} at {}; binding tier {} sm 0x{:x} -> dynamic resources {}",
             kCapacity, kFrameCount, kFrameRingCapacity, kRtRegionCapacity, kRtRegionBase, kCapacity - kTextureBase, kTextureBase,
             haveOptions ? static_cast<int>(options.ResourceBindingTier) : 0,
             haveSm ? static_cast<unsigned>(sm.HighestShaderModel) : 0u, dynamicResources_ ? "yes" : "NO");
    instance_ = this;
    return true;
}

void BindlessHeap::Shutdown()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (heap_)
    {
        LOG_INFO(logging::LogCategory::Render, "[bindless] shutdown: {} texture slots live, {} free, {} retired, high water {}",
                 live_, free_.size(), retired_.size(), nextSlot_ - kTextureBase - 1u);
    }
    instance_ = nullptr;
    heap_.Reset();
    device_ = nullptr;
    incr_ = 0;
    free_.clear();
    retired_.clear();
    nextSlot_ = kTextureBase + 1;
    live_ = 0;
    frameNo_ = 0;
}

D3D12_CPU_DESCRIPTOR_HANDLE BindlessHeap::Cpu(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_ ? heap_->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (h.ptr) { h.ptr += static_cast<SIZE_T>(index) * incr_; }
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE BindlessHeap::Gpu(UINT index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap_ ? heap_->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{};
    if (h.ptr) { h.ptr += static_cast<UINT64>(index) * incr_; }
    return h;
}

UINT BindlessHeap::AllocateSlot(D3D12_CPU_DESCRIPTOR_HANDLE src)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!heap_ || src.ptr == 0) { return kInvalidIndex; }
    UINT index = kInvalidIndex;
    if (!free_.empty())
    {
        index = free_.back();
        free_.pop_back();
    }
    else if (nextSlot_ < kCapacity)
    {
        index = nextSlot_++;
    }
    else
    {
        if (!exhaustedLogged_)
        {
            LOG_ERROR(logging::LogCategory::Render, "[bindless] texture slots exhausted ({} live, {} retired); textures now rewrite their slot in place",
                      live_, retired_.size());
            exhaustedLogged_ = true;
        }
        return kInvalidIndex;
    }
    device_->CopyDescriptorsSimple(1, Cpu(index), src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ++live_;
    return index;
}

void BindlessHeap::RetireSlot(UINT index)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!heap_ || index == kInvalidIndex || index <= kTextureBase || index >= kCapacity) { return; }
    retired_.push_back(Retired{ index, frameNo_ + kFrameCount });
    if (live_ > 0) { --live_; }
}

void BindlessHeap::RewriteSlot(UINT index, D3D12_CPU_DESCRIPTOR_HANDLE src)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!heap_ || src.ptr == 0 || index == kInvalidIndex || index <= kTextureBase || index >= kCapacity) { return; }
    device_->CopyDescriptorsSimple(1, Cpu(index), src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void BindlessHeap::BeginFrame(uint64_t frameNo)
{
    std::lock_guard<std::mutex> lock(mtx_);
    frameNo_ = frameNo;
    for (const Retired& r : retired_) { if (r.frame <= frameNo) { free_.push_back(r.index); } }
    std::erase_if(retired_, [frameNo](const Retired& r) { return r.frame <= frameNo; });
}

BindlessHeap::Stats BindlessHeap::GetStats() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    Stats s;
    s.slotsUsed = live_;
    s.slotsFree = static_cast<UINT>(free_.size());
    s.slotsRetired = static_cast<UINT>(retired_.size());
    s.slotsCapacity = kCapacity - kTextureBase - 1u;
    return s;
}

} // namespace render
