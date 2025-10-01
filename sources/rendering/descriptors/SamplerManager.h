#pragma once
#include <wrl.h>
#include <d3d12.h>
#include "third_party/robin_hood.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>

class Renderer;
class DescriptorAllocatorSampler; // Your allocator for the shader-visible sampler heap

// Simple normalization/hash wrapper for the sampler descriptor
struct SamplerKey {
    D3D12_SAMPLER_DESC d{};

    SamplerKey() = default;
    explicit SamplerKey(const D3D12_SAMPLER_DESC& in) : d(in) {}

    bool operator==(const SamplerKey& o) const {
        return std::memcmp(&d, &o.d, sizeof(D3D12_SAMPLER_DESC)) == 0;
    }
};
struct SamplerKeyHasher {
    size_t operator()(const SamplerKey& k) const noexcept {
        // Primitive byte-wise hash
        const uint64_t* p = reinterpret_cast<const uint64_t*>(&k.d);
        constexpr size_t N = sizeof(D3D12_SAMPLER_DESC)/sizeof(uint64_t);
        size_t h = 1469598103934665603ull;
        for (size_t i=0;i<N;i++) { h ^= (size_t)p[i]; h *= 1099511628211ull; }
        return h;
    }
};

class SamplerManager {
public:
    void Init(ID3D12Device* device, UINT capacity = 256);

    // Return the sampler's GPU handle for the CURRENT frame (stages from CPU to the shader-visible heap if needed)
    D3D12_GPU_DESCRIPTOR_HANDLE Get(Renderer* renderer, const D3D12_SAMPLER_DESC& desc);

    // Build a TABLE from consecutive samplers and return the base GPU handle of that table.
    template <size_t N>
    D3D12_GPU_DESCRIPTOR_HANDLE GetTable(Renderer* renderer, const std::array<D3D12_SAMPLER_DESC, N>& descs) {
        return GetTableImpl(renderer, descs.data(), N);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetTable(Renderer* renderer, const D3D12_SAMPLER_DESC& desc) {
        return GetTable(renderer, std::array<D3D12_SAMPLER_DESC, 1>{ desc });
    }

    void Clear();

    // Handy presets
    static const D3D12_SAMPLER_DESC* LinearWrap();
    static const D3D12_SAMPLER_DESC* LinearClamp();
    static const D3D12_SAMPLER_DESC* PointClamp();
    static const D3D12_SAMPLER_DESC* FontMinPointMagLinearClamp();
    static const D3D12_SAMPLER_DESC* AnisoWrap(UINT aniso = 8);
    static const D3D12_SAMPLER_DESC* ComparisonLinearClamp();

private:
    struct Entry {
        UINT  cpuIndex = UINT(-1);                      // Index within the CPU heap
        UINT  lastFrame = UINT(-1);                     // Frame when it was last staged
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};              // GPU handle in the shader-visible heap (for lastFrame)
    };

    std::mutex mtx_;

    // CPU heap
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpuHeap_;
    UINT cpuIncr_ = 0;
    UINT cpuCapacity_ = 0;
    UINT cpuCursor_ = 0; // bump pointer

    ID3D12Device* device_ = nullptr;

    robin_hood::unordered_map<SamplerKey, Entry, SamplerKeyHasher> cache_;

    UINT ensureCpu_(const D3D12_SAMPLER_DESC& desc);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandleAt_(UINT idx) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetTableImpl(Renderer* renderer, const D3D12_SAMPLER_DESC* descs, size_t count);
};
