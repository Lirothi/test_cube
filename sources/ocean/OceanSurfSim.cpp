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

// Pass-flow S3 pilot: the whole pass in one builder — see the header comment. Decisions are
// locals, the cross-frame state (ping-pong index, window centre, sim clock, substep accumulator)
// is committed HERE, the declarations are made from the same locals, and the returned record
// lambda captures them by value. The record body names no resource: EmitPoint markers emit
// whatever the compile produced for our two points.
std::function<void(RenderGraphPassContext)> OceanSurfSim::BuildPass(
    RenderGraphPassContext& ctx, float timeSeconds, const ShoreDepthWindow& shore)
{
    if (!created_ || !updateMaterial_ || !relocateMaterial_ || shore.srv.ptr == 0 ||
        shore.resource == nullptr)
    {
        return {};
    }

    // ---- decisions, once ----
    const bool relocate = WillRelocate();
    const int shiftX = pendingShiftX_;
    const int shiftY = pendingShiftY_;
    const uint32_t readIndex = current_;
    if (relocate)
    {
        center_ = pendingCenter_;
    }
    const Math::float2 center = center_;
    pendingShiftX_ = 0;
    pendingShiftY_ = 0;

    // S1: fixed-substep catch-up (Crest's LodDataMgrPersistent cadence). A frozen clock
    // (--wind-freeze) yields zero elapsed => zero substeps => the sim holds still with it.
    const float elapsed =
        previousTime_ >= 0.0f ? std::max(0.0f, timeSeconds - previousTime_) : 0.0f;
    previousTime_ = timeSeconds;
    substepAccum_ = std::min(substepAccum_ + elapsed, kFixedDt * (2.0f * kMaxSubsteps));
    int substeps = static_cast<int>(substepAccum_ / kFixedDt);
    substeps = std::min(substeps, kMaxSubsteps);
    substepAccum_ -= static_cast<float>(substeps) * kFixedDt;

    // Poke (the S1 gate's test hump): a UI button press or the --ocean-surf-poke cadence,
    // injected on the FIRST substep of this frame.
    bool poke = ocean::g_surfSimPokeRequest;
    if (ocean::g_surfSimPokeInterval > 0.0f &&
        timeSeconds - lastAutoPoke_ >= ocean::g_surfSimPokeInterval)
    {
        poke = true;
    }
    float pokeAmp = 0.0f;
    if (poke)
    {
        if (substeps == 0) { substeps = 1; } // a poke must land even on a frozen clock
        ocean::g_surfSimPokeRequest = false;
        lastAutoPoke_ = timeSeconds;
        pokeAmp = 0.6f; // metres
    }

    if (substeps == 0 && !relocate)
    {
        return {}; // nothing to integrate: no dispatches, no declarations, no barriers
    }

    const uint32_t swaps = (relocate ? 1u : 0u) + static_cast<uint32_t>(substeps);
    const uint32_t finalIndex = readIndex ^ (swaps & 1u);
    current_ = finalIndex; // committed before recording: GetWaveSrv is final from here on

    // ---- declarations, from the same locals ----
    // Everything is written as a UAV first (relocate and/or the substep chain); the shore depth
    // map is read at its canonical state (no barrier, the compile just sees the read)...
    ctx.NextPoint();
    const std::uint32_t uavPoint = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(wave_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(wave_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(foam_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(foam_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(shore.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // ...then the frame's freshest height field is left readable for the surface's pixel shader.
    ctx.NextPoint();
    ctx.Use(wave_[finalIndex].Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ---- record, capturing the decisions by value ----
    return [this, timeSeconds, relocate, shiftX, shiftY, center, readIndex, substeps, pokeAmp,
            shore, uavPoint](RenderGraphPassContext c)
    {
        Renderer* renderer = c.renderer;
        auto t = c.BeginCL();
        ID3D12GraphicsCommandList* cl = t.cl;
        {
            GPU_SCOPE(cl, ProfilerScopes::kOceanSurfSim);

            const UINT groups = (kResolution + 7u) / 8u;
            auto asUint = [](float value)
            {
                uint32_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                return bits;
            };

            renderer->EmitPoint(cl, uavPoint);

            auto srvTable = renderer->StageSrvUavTable({ shore.srv });

            auto dispatch = [&](const std::shared_ptr<Material>& material, uint32_t readIdx,
                                int sx, int sy, float dt, float pokeMetres)
            {
                auto uavTable = renderer->StageSrvUavTable({
                    waveUav_[readIdx], waveUav_[1u - readIdx],
                    foamUav_[readIdx], foamUav_[1u - readIdx] });

                auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
                auto& rctx = ctxHandle.ref();
                rctx.constants[0] = {
                    kResolution,
                    asUint(kTexelWorld),
                    asUint(center.x),
                    asUint(center.y),
                    asUint(timeSeconds),
                    asUint(dt),
                    static_cast<uint32_t>(sx),
                    static_cast<uint32_t>(sy),
                    asUint(shore.center.x),
                    asUint(shore.center.y),
                    asUint(shore.invExtent),
                    asUint(shore.zNear),
                    asUint(shore.zFar),
                    asUint(shore.camHeight),
                    asUint(pokeMetres),
                    0u,
                };
                rctx.srvTable[0] = srvTable.gpu;
                rctx.uavTable[0] = uavTable.gpu;
                material->Bind(cl, rctx);
                cl->Dispatch(groups, groups, 1);
                renderer->UAVBarrier(cl, wave_[1u - readIdx].Get());
                renderer->UAVBarrier(cl, foam_[1u - readIdx].Get());
            };

            uint32_t read = readIndex;
            if (relocate)
            {
                dispatch(relocateMaterial_, read, shiftX, shiftY, 0.0f, 0.0f);
                read ^= 1u;
            }
            for (int i = 0; i < substeps; ++i)
            {
                dispatch(updateMaterial_, read, 0, 0, kFixedDt, i == 0 ? pokeAmp : 0.0f);
                read ^= 1u;
            }

            // The SRV handoff point (the final wave side to pixel-readable for the surface).
            renderer->EmitPoint(cl, uavPoint + 1u);
        }
        c.EndCL(t);
    };
}

Math::float4 OceanSurfSim::GetWindowParams() const
{
    return Math::float4(center_.x, center_.y, 1.0f / kHalfExtent, kTexelWorld);
}

D3D12_CPU_DESCRIPTOR_HANDLE OceanSurfSim::GetWaveSrv() const
{
    return created_ ? waveSrv_[current_] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
