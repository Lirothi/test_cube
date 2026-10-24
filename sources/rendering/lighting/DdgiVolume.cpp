#include "rendering/lighting/DdgiVolume.h"

#include <algorithm>
#include <cmath>

#include "core/logging/Log.h"
#include "materials/Material.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/TextureCreate.h"

namespace
{
// Same rule as the shaders' (ddgi_common.hlsli): a probe's ray seed must not repeat frame to frame.
uint32_t HashU32(uint32_t s)
{
    s = (s ^ 61u) ^ (s >> 16);
    s *= 9u;
    s = s ^ (s >> 4);
    s *= 0x27d4eb2du;
    return s ^ (s >> 15);
}
float Unit(uint32_t h) { return float(h & 0xFFFFFFu) * (1.0f / 16777216.0f); }
} // namespace

DdgiVolume::DdgiVolume() = default;
DdgiVolume::~DdgiVolume() = default;

void DdgiVolume::Reset()
{
    rays_.Reset();
    irradiance_.Reset();
    depth_.Reset();
    state_.Reset();
    heap_.Reset();
    handles_ = {};
    traceMaterial_.reset();
    updateMaterial_.reset();
    ready_ = false;
    failed_ = false;
    spacing_ = 0.0f;
    tracedGeneration_ = 0;
    // The new resources start zeroed: no probe carries the valid bit, whatever the generation.
}

void DdgiVolume::Prepare(Renderer* renderer)
{
    if (ready_ || failed_ || !renderer) { return; }
    ID3D12Device* device = renderer->GetDevice();
    const auto fail = [this](const char* what) {
        LOG_ERROR(logging::LogCategory::Render, "GI probe volume: {} failed -- the bounce stays off", what);
        failed_ = true;
    };

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = static_cast<UINT>(handles_.size());
    if (FAILED(gpu::CreateDescriptorHeap(device, &hd, MemTag::Gi, L"Ddgi.Heap", IID_PPV_ARGS(&heap_)))) { fail("descriptor heap"); return; }
    const D3D12_CPU_DESCRIPTOR_HANDLE base = heap_->GetCPUDescriptorHandleForHeapStart();
    const UINT step = device->GetDescriptorHandleIncrementSize(hd.Type);
    for (size_t i = 0; i < handles_.size(); ++i) { handles_[i] = { base.ptr + i * step }; }

    // A 2D texture with an SRV and a UAV of its own format, created in its rest state.
    const auto makeTexture = [&](GpuResource& out, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* name,
                                 D3D12_CPU_DESCRIPTOR_HANDLE srv, D3D12_CPU_DESCRIPTOR_HANDLE uav) -> bool {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kRestState, nullptr, MemTag::Gi,
                                               name, &resource)))
        {
            return false;
        }
        out.Attach(renderer->Declarations(), resource, kRestState, name);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource.Get(), &sd, srv);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, uav);
        return true;
    };
    // The ray buffer holds every probe's row, so any divisor (down to 1) fits.
    if (!makeTexture(rays_, kRaysPerProbe, kProbeCount, DXGI_FORMAT_R16G16B16A16_FLOAT, L"Ddgi.Rays", handles_[0], handles_[1]) ||
        !makeTexture(irradiance_, kAtlasTilesX * kIrradianceTile, kAtlasTilesY * kIrradianceTile,
                     DXGI_FORMAT_R16G16B16A16_FLOAT, L"Ddgi.Irradiance", handles_[2], handles_[3]) ||
        !makeTexture(depth_, kAtlasTilesX * kDepthTile, kAtlasTilesY * kDepthTile, DXGI_FORMAT_R16G16_FLOAT,
                     L"Ddgi.Depth", handles_[4], handles_[5]))
    {
        fail("texture");
        return;
    }
    {
        constexpr UINT kStride = 16; // uint4 per probe
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = UINT64(kProbeCount) * kStride;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        // A buffer is created in COMMON whatever it is asked for (GBV id=1328); it rests in kRestState
        // from its first use (SdsmShadows' rule).
        if (FAILED(gpu::CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, MemTag::Gi, L"Ddgi.ProbeState", IID_PPV_ARGS(&resource))))
        {
            fail("probe state buffer");
            return;
        }
        state_.Attach(renderer->Declarations(), resource, D3D12_RESOURCE_STATE_COMMON, kRestState, L"Ddgi.ProbeState");
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = kProbeCount;
        sd.Buffer.StructureByteStride = kStride;
        device->CreateShaderResourceView(resource.Get(), &sd, handles_[6]);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kProbeCount;
        ud.Buffer.StructureByteStride = kStride;
        device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, handles_[7]);
    }

    Material::ComputeDesc cd{};
    cd.csEntry = "CSMain";
    cd.shaderFile = L"shaders/ddgi_trace_cs.hlsl";
    traceMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    cd.shaderFile = L"shaders/ddgi_update_cs.hlsl";
    updateMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    if (!traceMaterial_ || !traceMaterial_->GetPipelineState() || !updateMaterial_ || !updateMaterial_->GetPipelineState())
    {
        fail("PSO");
        return;
    }
    ready_ = true;
    LOG_INFO(logging::LogCategory::Render, "GI probe volume: {}x{}x{} probes, {} rays each, atlases {}x{} + {}x{}",
             kDimX, kDimY, kDimZ, kRaysPerProbe, kAtlasTilesX * kIrradianceTile, kAtlasTilesY * kIrradianceTile,
             kAtlasTilesX * kDepthTile, kAtlasTilesY * kDepthTile);
}

DdgiVolume::Frame DdgiVolume::BeginFrame(const Math::float3& cameraPos, float spacing, uint32_t divisor)
{
    Frame f{};
    spacing = std::clamp(spacing, 0.1f, 8.0f);
    if (spacing != spacing_)
    {
        // A different grid: what the slots hold belongs to the old one.
        spacing_ = spacing;
        ++generation_;
        if ((generation_ & 0xFFFFFFu) == 0u) { generation_ = 1u; } // 24 bits in the state word
        tracedGeneration_ = 0;
    }
    f.spacing = spacing_;
    f.generation = generation_;
    const int dims[3] = { kDimX, kDimY, kDimZ };
    const float cam[3] = { cameraPos.x, cameraPos.y, cameraPos.z };
    for (int a = 0; a < 3; ++a)
    {
        f.baseCell[a] = static_cast<int>(std::floor(cam[a] / spacing_)) - dims[a] / 2;
        lastBase_[a] = f.baseCell[a];
    }
    f.divisor = std::clamp(divisor, 1u, 64u);
    f.phase = frameCounter_ % f.divisor;
    f.probes = (kProbeCount + f.divisor - 1u) / f.divisor;
    f.seed = HashU32(frameCounter_ * 2654435761u + 17u);

    // A uniformly random rotation (Shoemake's quaternion), so successive updates of a probe sample
    // between each other's rays.
    const float u1 = Unit(HashU32(f.seed ^ 0x68bc21ebu));
    const float u2 = Unit(HashU32(f.seed ^ 0x02e5be93u));
    const float u3 = Unit(HashU32(f.seed ^ 0x967a889bu));
    const float s1 = std::sqrt(1.0f - u1), s2 = std::sqrt(u1);
    const float twoPi = 6.28318531f;
    const DirectX::XMVECTOR q = DirectX::XMVectorSet(s1 * std::sin(twoPi * u2), s1 * std::cos(twoPi * u2),
                                                     s2 * std::sin(twoPi * u3), s2 * std::cos(twoPi * u3));
    DirectX::XMStoreFloat4x4(&f.rayRotation.m, DirectX::XMMatrixRotationQuaternion(DirectX::XMQuaternionNormalize(q)));
    ++frameCounter_;
    return f;
}
