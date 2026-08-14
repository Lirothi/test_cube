#include "ocean/OceanSurfSim.h"

#include <cmath>
#include <cstring>

#include "core/Helpers.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "materials/Material.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderContextPool.h"
#include "rendering/core/TextureCreate.h"

using Microsoft::WRL::ComPtr;

void OceanSurfSim::EnsureResources(Renderer* renderer)
{
    if (created_ || !renderer)
    {
        return;
    }

    auto* device = renderer->GetDevice();
    if (!device)
    {
        return;
    }

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kResolution;
    texDesc.Height = kResolution;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    auto createPair = [&](GpuResource* pair, DXGI_FORMAT format, const wchar_t* nameA,
                          const wchar_t* nameB)
    {
        texDesc.Format = format;
        for (int i = 0; i < 2; ++i)
        {
            ComPtr<ID3D12Resource> res;
            ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE,
                texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, &res));
            pair[i].Attach(renderer->Declarations(), std::move(res),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, i == 0 ? nameA : nameB);
        }
    };

    // Height + vertical velocity for the S1 wave equation; foam is deposited/decayed in S3.
    createPair(wave_, DXGI_FORMAT_R16G16_FLOAT, L"Ocean.SurfSimWaveA", L"Ocean.SurfSimWaveB");
    createPair(foam_, DXGI_FORMAT_R16_FLOAT, L"Ocean.SurfSimFoamA", L"Ocean.SurfSimFoamB");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 8u; // 4 UAVs + 4 SRVs
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap_)));

    const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
    auto next = [&]()
    {
        D3D12_CPU_DESCRIPTOR_HANDLE current = handle;
        handle.ptr += incr;
        return current;
    };

    for (int i = 0; i < 2; ++i)
    {
        waveUav_[i] = next();
        device->CreateUnorderedAccessView(wave_[i].Get(), nullptr, nullptr, waveUav_[i]);
    }
    for (int i = 0; i < 2; ++i)
    {
        foamUav_[i] = next();
        device->CreateUnorderedAccessView(foam_[i].Get(), nullptr, nullptr, foamUav_[i]);
    }
    for (int i = 0; i < 2; ++i)
    {
        waveSrv_[i] = next();
        device->CreateShaderResourceView(wave_[i].Get(), nullptr, waveSrv_[i]);
    }
    for (int i = 0; i < 2; ++i)
    {
        foamSrv_[i] = next();
        device->CreateShaderResourceView(foam_[i].Get(), nullptr, foamSrv_[i]);
    }

    auto* materialMgr = renderer->GetMaterialManager();
    Material::ComputeDesc desc{};
    desc.shaderFile = L"shaders/ocean_surf_sim_cs.hlsl";
    desc.csEntry = "Update";
    updateMaterial_ = materialMgr->GetOrCreateCompute(renderer, desc);
    desc.csEntry = "Relocate";
    relocateMaterial_ = materialMgr->GetOrCreateCompute(renderer, desc);

    created_ = true;
}

void OceanSurfSim::TickWindow(Math::float2 cameraXZ)
{
    if (!created_)
    {
        return;
    }

    // Snap to a texel-aligned grid so a re-anchor is always a lossless whole-texel copy.
    const float snap = kTexelWorld * kSnapTexels;
    const Math::float2 target(
        std::floor(cameraXZ.x / snap) * snap + snap * 0.5f,
        std::floor(cameraXZ.y / snap) * snap + snap * 0.5f);

    if (!hasCenter_)
    {
        center_ = target;
        pendingCenter_ = target;
        pendingShiftX_ = 0;
        pendingShiftY_ = 0;
        hasCenter_ = true;
        return;
    }

    pendingShiftX_ = static_cast<int>(std::lround((target.x - center_.x) / kTexelWorld));
    pendingShiftY_ = static_cast<int>(std::lround((target.y - center_.y) / kTexelWorld));
    pendingCenter_ = target;
}

void OceanSurfSim::PrepareCompute(RenderGraphPassContext& ctx)
{
    if (!created_)
    {
        return;
    }

    // Everything is written as a UAV first (relocate and/or update)...
    ctx.NextPoint();
    ctx.Use(wave_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(wave_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(foam_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(foam_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ...then the frame's freshest height field is left readable for the surface's pixel shader.
    ctx.NextPoint();
    ctx.Use(wave_[CurrentAfterFrame()].Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void OceanSurfSim::RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl, float timeSeconds)
{
    if (!created_ || !renderer || !cl || !updateMaterial_ || !relocateMaterial_)
    {
        return;
    }

    GPU_SCOPE(cl, ProfilerScopes::kOceanSurfSim);

    const float dt = previousTime_ >= 0.0f ? std::max(0.0f, timeSeconds - previousTime_) : 0.0f;
    previousTime_ = timeSeconds;

    const UINT groups = (kResolution + 7u) / 8u;

    auto asUint = [](float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };

    for (int i = 0; i < 2; ++i)
    {
        renderer->Transition(cl, wave_[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(cl, foam_[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    auto dispatch = [&](const std::shared_ptr<Material>& material, uint32_t readIndex,
                        int shiftX, int shiftY, Math::float2 center)
    {
        auto uavTable = renderer->StageSrvUavTable({
            waveUav_[readIndex], waveUav_[1u - readIndex],
            foamUav_[readIndex], foamUav_[1u - readIndex] });

        auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = ctxHandle.ref();
        ctx.constants[0] = {
            kResolution,
            asUint(kTexelWorld),
            asUint(center.x),
            asUint(center.y),
            static_cast<uint32_t>(shiftX),
            static_cast<uint32_t>(shiftY),
            asUint(timeSeconds),
            asUint(dt),
        };
        ctx.uavTable[0] = uavTable.gpu;
        material->Bind(cl, ctx);
        cl->Dispatch(groups, groups, 1);
        renderer->UAVBarrier(cl, wave_[1u - readIndex].Get());
        renderer->UAVBarrier(cl, foam_[1u - readIndex].Get());
    };

    if (WillRelocate())
    {
        dispatch(relocateMaterial_, current_, pendingShiftX_, pendingShiftY_, pendingCenter_);
        current_ ^= 1u;
        center_ = pendingCenter_;
        pendingShiftX_ = 0;
        pendingShiftY_ = 0;
    }

    dispatch(updateMaterial_, current_, 0, 0, center_);
    current_ ^= 1u;

    renderer->Transition(cl, wave_[current_].Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

Math::float4 OceanSurfSim::GetWindowParams() const
{
    return Math::float4(center_.x, center_.y, 1.0f / kHalfExtent, kTexelWorld);
}

D3D12_CPU_DESCRIPTOR_HANDLE OceanSurfSim::GetWaveSrv() const
{
    return created_ ? waveSrv_[current_] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
