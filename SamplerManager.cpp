#include "SamplerManager.h"
#include "Renderer.h"
#include "DescriptorAllocator.h" // для DescriptorAllocatorSampler

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

    return block.gpu; // base GPU handle таблицы
}

void SamplerManager::Clear()
{
    cache_.clear();
    cpuHeap_.Reset();
}

// Presets
D3D12_SAMPLER_DESC SamplerManager::LinearWrap() {
    D3D12_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.MinLOD = 0.0f; s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.MaxAnisotropy = 1;
    return s;
}
D3D12_SAMPLER_DESC SamplerManager::LinearClamp() {
    D3D12_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MinLOD = 0.0f; s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.MaxAnisotropy = 1;
    return s;
}
D3D12_SAMPLER_DESC SamplerManager::PointClamp() {
    D3D12_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MinLOD = 0.0f; s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.MaxAnisotropy = 1;
    return s;
}
D3D12_SAMPLER_DESC SamplerManager::AnisoWrap(UINT aniso) {
    D3D12_SAMPLER_DESC s{};
    s.Filter = D3D12_FILTER_ANISOTROPIC;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.MinLOD = 0.0f; s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.MaxAnisotropy = aniso ? aniso : 8;
    return s;
}
D3D12_SAMPLER_DESC SamplerManager::FontMinPointMagLinearClamp() {
    D3D12_SAMPLER_DESC d = {};
    d.Filter = D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    d.AddressU = d.AddressV = d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    d.MipLODBias = 0.0f;
    d.MaxAnisotropy = 1;
    d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    d.MinLOD = 0.0f;
    d.MaxLOD = D3D12_FLOAT32_MAX;
    return d;
}
D3D12_SAMPLER_DESC SamplerManager::ComparisonLinearClamp() {
    D3D12_SAMPLER_DESC d{};
    d.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT; // или POINT
    d.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    d.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    d.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    d.MipLODBias = 0.0f;
    d.MaxAnisotropy = 1;
    d.MinLOD = 0.0f;
    d.MaxLOD = 0.0f;   // будем звать SampleCmpLevelZero
    d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
    return d;
}