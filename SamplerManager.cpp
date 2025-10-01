#include "SamplerManager.h"
#include "Renderer.h"
#include "DescriptorAllocator.h" // for DescriptorAllocatorSampler

#include <array>

using Microsoft::WRL::ComPtr;

void SamplerManager::Init(ID3D12Device* device, UINT capacity) {
    device_ = device;
    cpuCapacity_ = capacity;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = cpuCapacity_;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpuHeap_)))) {
        throw std::runtime_error("SamplerManager: CreateDescriptorHeap CPU failed");
    }
    cpuHeap_->SetName(L"SampManager_DESCRIPTOR_HEAP");
    cpuIncr_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    cpuCursor_ = 0;
    cache_.clear();
}

D3D12_CPU_DESCRIPTOR_HANDLE SamplerManager::cpuHandleAt_(UINT idx) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(idx) * cpuIncr_;
    return h;
}

UINT SamplerManager::ensureCpu_(const D3D12_SAMPLER_DESC& desc) {
    std::lock_guard<std::mutex> lk(mtx_);
    SamplerKey key(desc);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.cpuIndex;

    if (cpuCursor_ >= cpuCapacity_) {
        throw std::runtime_error("SamplerManager CPU heap overflow (increase capacity in Init)");
    }
    const UINT idx = cpuCursor_++;
    device_->CreateSampler(&desc, cpuHandleAt_(idx));

    Entry e;
    e.cpuIndex = idx;
    e.lastFrame = UINT(-1);
    e.gpu.ptr = 0;
    cache_.emplace(key, e);
    return idx;
}

D3D12_GPU_DESCRIPTOR_HANDLE SamplerManager::Get(Renderer* renderer, const D3D12_SAMPLER_DESC& desc) {
    const UINT cpuIdx = ensureCpu_(desc);
    const UINT frame  = renderer->GetCurrentFrameIndex();

    SamplerKey key(desc);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& e = cache_.find(key)->second;
        if (e.lastFrame == frame && e.gpu.ptr) {
            return e.gpu;
        }
        auto& sa = renderer->GetSamplerAlloc();
        GpuDescHandle dst = sa.Alloc();
        renderer->GetDevice()->CopyDescriptorsSimple(1, dst.cpu, cpuHandleAt_(cpuIdx), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        e.gpu = dst.gpu;
        e.lastFrame = frame;
        return e.gpu;
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE SamplerManager::GetTableImpl(Renderer* renderer, const D3D12_SAMPLER_DESC* descs, size_t count) {
    D3D12_GPU_DESCRIPTOR_HANDLE null{}; null.ptr = 0;
    if (count == 0) {
        return null;
    }

    auto& sa = renderer->GetSamplerAlloc();
    GpuDescHandle block = sa.Alloc(static_cast<UINT>(count));

    D3D12_CPU_DESCRIPTOR_HANDLE dst = block.cpu;
    for (size_t i = 0; i < count; ++i) {
        const UINT cpuIdx = ensureCpu_(descs[i]);
        device_->CopyDescriptorsSimple(1, dst, cpuHandleAt_(cpuIdx), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        dst.ptr += sa.GetIncr();
    }

    return block.gpu; // base GPU handle of the table
}

void SamplerManager::Clear()
{
    cache_.clear();
    cpuHeap_.Reset();
}

namespace {

enum class SamplerPresetIndex : size_t {
    LinearWrap = 0,
    LinearClamp,
    PointClamp,
    FontMinPointMagLinearClamp,
    ComparisonLinearClamp,
    AnisoWrap4,
    AnisoWrap8,
    AnisoWrap16,
    Count
};

struct PrebakedSamplers {
    std::array<D3D12_SAMPLER_DESC,
               static_cast<size_t>(SamplerPresetIndex::Count)> descs{};

    PrebakedSamplers() {
        auto makeLinear = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
            D3D12_SAMPLER_DESC s{};
            s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU = s.AddressV = s.AddressW = mode;
            s.MinLOD = 0.0f;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            s.MaxAnisotropy = 1;
            return s;
        };

        auto makePointClamp = []() {
            D3D12_SAMPLER_DESC s{};
            s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            s.MinLOD = 0.0f;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            s.MaxAnisotropy = 1;
            return s;
        };

        auto makeFont = []() {
            D3D12_SAMPLER_DESC d{};
            d.Filter = D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
            d.AddressU = d.AddressV = d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            d.MipLODBias = 0.0f;
            d.MaxAnisotropy = 1;
            d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            d.MinLOD = 0.0f;
            d.MaxLOD = D3D12_FLOAT32_MAX;
            return d;
        };

        auto makeComparisonLinearClamp = []() {
            D3D12_SAMPLER_DESC d{};
            d.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            d.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            d.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            d.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            d.MipLODBias = 0.0f;
            d.MaxAnisotropy = 1;
            d.MinLOD = 0.0f;
            d.MaxLOD = 0.0f;
            d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
            return d;
        };

        auto makeAnisoWrap = [](UINT aniso) {
            D3D12_SAMPLER_DESC s{};
            s.Filter = D3D12_FILTER_ANISOTROPIC;
            s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            s.MinLOD = 0.0f;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            s.MaxAnisotropy = aniso;
            return s;
        };

        descs[static_cast<size_t>(SamplerPresetIndex::LinearWrap)] =
            makeLinear(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        descs[static_cast<size_t>(SamplerPresetIndex::LinearClamp)] =
            makeLinear(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        descs[static_cast<size_t>(SamplerPresetIndex::PointClamp)] = makePointClamp();
        descs[static_cast<size_t>(SamplerPresetIndex::FontMinPointMagLinearClamp)] = makeFont();
        descs[static_cast<size_t>(SamplerPresetIndex::ComparisonLinearClamp)] =
            makeComparisonLinearClamp();

        descs[static_cast<size_t>(SamplerPresetIndex::AnisoWrap4)] = makeAnisoWrap(4);
        descs[static_cast<size_t>(SamplerPresetIndex::AnisoWrap8)] = makeAnisoWrap(8);
        descs[static_cast<size_t>(SamplerPresetIndex::AnisoWrap16)] = makeAnisoWrap(16);
    }
};

const PrebakedSamplers& GetPrebakedSamplers() {
    static const PrebakedSamplers samplers;
    return samplers;
}

const D3D12_SAMPLER_DESC* ResolveSampler(SamplerPresetIndex preset) {
    const auto& samplers = GetPrebakedSamplers().descs;
    return &samplers[static_cast<size_t>(preset)];
}

} // namespace

// Presets
const D3D12_SAMPLER_DESC* SamplerManager::LinearWrap() {
    return ResolveSampler(SamplerPresetIndex::LinearWrap);
}

const D3D12_SAMPLER_DESC* SamplerManager::LinearClamp() {
    return ResolveSampler(SamplerPresetIndex::LinearClamp);
}

const D3D12_SAMPLER_DESC* SamplerManager::PointClamp() {
    return ResolveSampler(SamplerPresetIndex::PointClamp);
}

const D3D12_SAMPLER_DESC* SamplerManager::AnisoWrap(UINT aniso) {
    const UINT requested = aniso == 0 ? 8u : aniso;
    if (requested <= 4) {
        return ResolveSampler(SamplerPresetIndex::AnisoWrap4);
    }
    if (requested <= 8) {
        return ResolveSampler(SamplerPresetIndex::AnisoWrap8);
    }
    return ResolveSampler(SamplerPresetIndex::AnisoWrap16);
}

const D3D12_SAMPLER_DESC* SamplerManager::FontMinPointMagLinearClamp() {
    return ResolveSampler(SamplerPresetIndex::FontMinPointMagLinearClamp);
}

const D3D12_SAMPLER_DESC* SamplerManager::ComparisonLinearClamp() {
    return ResolveSampler(SamplerPresetIndex::ComparisonLinearClamp);
}
