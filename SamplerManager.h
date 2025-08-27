#pragma once
#include <wrl.h>
#include <d3d12.h>
#include <unordered_map>
#include <vector>
#include <initializer_list>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>

class Renderer;
class DescriptorAllocatorSampler; // твой аллокатор shader-visible sampler heap

// Простая нормализация/хэш desc
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
        // примитивный хэш по байтам
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

    // Вернёт GPU-хэндл самплера для ТЕКУЩЕГО кадра (стейджит из CPU в shader-visible heap, если нужно)
    D3D12_GPU_DESCRIPTOR_HANDLE Get(Renderer* renderer, const D3D12_SAMPLER_DESC& desc);

    // Сформировать ТАБЛИЦУ из нескольких самплеров подряд и вернуть base GPU handle таблицы.
    D3D12_GPU_DESCRIPTOR_HANDLE GetTable(Renderer* renderer, std::initializer_list<D3D12_SAMPLER_DESC> descs);

    void Clear();

    // Быстрые пресеты
    static D3D12_SAMPLER_DESC LinearWrap();
    static D3D12_SAMPLER_DESC LinearClamp();
    static D3D12_SAMPLER_DESC PointClamp();
    static D3D12_SAMPLER_DESC FontMinPointMagLinearClamp();
    static D3D12_SAMPLER_DESC AnisoWrap(UINT aniso = 8);

private:
    struct Entry {
        UINT  cpuIndex = UINT(-1);                      // индекс в CPU heap
        UINT  lastFrame = UINT(-1);                     // кадр, когда стейджили последний раз
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};              // GPU handle в shader-visible heap (на кадр lastFrame)
    };

    std::mutex mtx_;

    // CPU heap
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpuHeap_;
    UINT cpuIncr_ = 0;
    UINT cpuCapacity_ = 0;
    UINT cpuCursor_ = 0; // bump

    ID3D12Device* device_ = nullptr;

    std::unordered_map<SamplerKey, Entry, SamplerKeyHasher> cache_;

    UINT ensureCpu_(const D3D12_SAMPLER_DESC& desc);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandleAt_(UINT idx) const;
};