#include "ocean/OceanSurfSim.h"

#include <cmath>
#include <cstring>

#include "core/Helpers.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "materials/Material.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderContextPool.h"
#include "rendering/core/TextureCreate.h"

using Microsoft::WRL::ComPtr;

namespace
{
// CPU mirror of the shader's SurfSimCB (ocean_surf_sim_cs.hlsl) - keep the layouts in step.
struct SurfSimCB
{
    std::uint32_t resolution; float texelWorldSize; float centerX; float centerZ;
    float time; float deltaTime; std::int32_t shiftX; std::int32_t shiftY;
    float shoreCenterX; float shoreCenterZ; float shoreInvExtent; float shoreZNear;
    float shoreZFar; float shoreCamHeight; float pokeAmp; float sdfCenterX;
    float sdfCenterZ; float sdfInvExtent; float spawnCandX; float spawnCandZ;
    std::uint32_t spawnSlot; float spawnDistance; float segmentHalfLen; float spawnAmp;
    float spawnDuration; float spawnSigma; float pad0; float pad1;
};

constexpr float kSpawnDuration = 1.6f; // seconds of forcing per segment
constexpr float kSpawnSigma = 5.0f;    // metres, across-segment width
}

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

    // S2: the spawner slot buffer. UAV-resident for its whole life (Spawn writes, Update reads),
    // zero-initialized by creation => duration 0 => every slot starts free.
    {
        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = static_cast<UINT64>(kMaxSpawners) * kSpawnerStride;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> res;
        ThrowIfFailed(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res)));
        spawners_.Attach(renderer->Declarations(), std::move(res),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Ocean.SurfSimSpawners");
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 9u; // 4 sim UAVs + 4 sim SRVs + the spawner buffer UAV
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
    {
        spawnersUav_ = next();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = kMaxSpawners;
        uavDesc.Buffer.StructureByteStride = kSpawnerStride;
        device->CreateUnorderedAccessView(spawners_.Get(), nullptr, &uavDesc, spawnersUav_);
    }

    auto* materialMgr = renderer->GetMaterialManager();
    Material::ComputeDesc desc{};
    desc.shaderFile = L"shaders/ocean_surf_sim_cs.hlsl";
    desc.csEntry = "Update";
    updateMaterial_ = materialMgr->GetOrCreateCompute(renderer, desc);
    desc.csEntry = "Relocate";
    relocateMaterial_ = materialMgr->GetOrCreateCompute(renderer, desc);
    desc.csEntry = "Spawn";
    spawnMaterial_ = materialMgr->GetOrCreateCompute(renderer, desc);

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
    RenderGraphPassContext& ctx, float timeSeconds, const ShoreDepthWindow& shore,
    const Tuning& tuning)
{
    if (!created_ || !updateMaterial_ || !relocateMaterial_ || !spawnMaterial_ ||
        shore.srv.ptr == 0 || shore.resource == nullptr || shore.sdfSrv.ptr == 0)
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

    // S2: the spawner beat. Wind scales BOTH the amplitude and the cadence (invariant 3: a dead
    // calm starves the surf); the candidate is a random point in the window - the Spawn kernel
    // walks it to the waterline along the SDF and rejects candidates with no coast in reach.
    const float windFactor =
        std::clamp(1.0f - tuning.windCoupling * (1.0f - std::clamp(tuning.windAmount, 0.0f, 1.0f)),
                   0.0f, 1.0f);
    const float spawnAmp = tuning.amplitude * windFactor;
    const float spawnInterval = tuning.interval / std::max(windFactor, 0.05f);
    bool spawn = false;
    float candX = 0.0f;
    float candZ = 0.0f;
    uint32_t spawnSlot = 0;
    if (spawnAmp > 0.005f && timeSeconds - lastSpawnTime_ >= spawnInterval)
    {
        spawn = true;
        lastSpawnTime_ = timeSeconds;
        spawnSlot = nextSpawnSlot_++ % kMaxSpawners;
        auto rnd01 = [this]()
        {
            spawnSeed_ ^= spawnSeed_ << 13;
            spawnSeed_ ^= spawnSeed_ >> 17;
            spawnSeed_ ^= spawnSeed_ << 5;
            return static_cast<float>(spawnSeed_ & 0xFFFFFFu) / static_cast<float>(0x1000000);
        };
        candX = center_.x + (rnd01() * 2.0f - 1.0f) * (kHalfExtent * 0.8f);
        candZ = center_.y + (rnd01() * 2.0f - 1.0f) * (kHalfExtent * 0.8f);
    }

    if (substeps == 0 && !relocate && !spawn)
    {
        return {}; // nothing to integrate: no dispatches, no declarations, no barriers
    }

    const uint32_t swaps = (relocate ? 1u : 0u) + static_cast<uint32_t>(substeps);
    const uint32_t finalIndex = readIndex ^ (swaps & 1u);
    current_ = finalIndex; // committed before recording: GetWaveSrv is final from here on

    // ---- declarations, from the same locals ----
    // Everything is written as a UAV first (spawn, relocate and/or the substep chain); the
    // shore depth map is read at its canonical state (no barrier, the compile just sees the
    // read). The shore SDF is deliberately NOT declared: its canonical is its creation-time
    // UAV while it actually rests shader-readable after the one-shot jump flood — the caller
    // gates the sim on the SDF being built instead, exactly like the modern surface which
    // samples it undeclared.
    ctx.NextPoint();
    const std::uint32_t uavPoint = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(wave_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(wave_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(foam_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(foam_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(spawners_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(shore.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // ...then the frame's freshest height field is left readable for the surface's pixel shader.
    ctx.NextPoint();
    ctx.Use(wave_[finalIndex].Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ---- record, capturing the decisions by value ----
    return [this, timeSeconds, relocate, shiftX, shiftY, center, readIndex, substeps, pokeAmp,
            spawn, candX, candZ, spawnSlot, spawnAmp, shore, tuning,
            uavPoint](RenderGraphPassContext c)
    {
        Renderer* renderer = c.renderer;
        auto t = c.BeginCL();
        ID3D12GraphicsCommandList* cl = t.cl;
        {
            GPU_SCOPE(cl, ProfilerScopes::kOceanSurfSim);

            renderer->EmitPoint(cl, uavPoint);

            // The CB base every dispatch starts from; per-dispatch fields are patched in the
            // write lambda RecordComputeDispatch hands us.
            SurfSimCB base{};
            base.resolution = kResolution;
            base.texelWorldSize = kTexelWorld;
            base.centerX = center.x;
            base.centerZ = center.y;
            base.time = timeSeconds;
            base.shoreCenterX = shore.center.x;
            base.shoreCenterZ = shore.center.y;
            base.shoreInvExtent = shore.invExtent;
            base.shoreZNear = shore.zNear;
            base.shoreZFar = shore.zFar;
            base.shoreCamHeight = shore.camHeight;
            base.sdfCenterX = shore.sdfCenter.x;
            base.sdfCenterZ = shore.sdfCenter.y;
            base.sdfInvExtent = shore.sdfInvExtent;
            base.spawnDistance = tuning.spawnDistance;
            base.segmentHalfLen = tuning.segmentLength * 0.5f;
            base.spawnDuration = kSpawnDuration;
            base.spawnSigma = kSpawnSigma;

            const auto srvs = { shore.srv, shore.sdfSrv };
            const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};

            if (spawn)
            {
                SurfSimCB cb = base;
                cb.spawnCandX = candX;
                cb.spawnCandZ = candZ;
                cb.spawnSlot = spawnSlot;
                cb.spawnAmp = spawnAmp;
                RecordComputeDispatch(renderer, cl, spawnMaterial_.get(),
                    static_cast<UINT>(sizeof(SurfSimCB)),
                    [&cb](std::uint8_t* dst) { std::memcpy(dst, &cb, sizeof(cb)); },
                    srvs,
                    { waveUav_[readIndex], waveUav_[1u - readIndex],
                      foamUav_[readIndex], foamUav_[1u - readIndex], spawnersUav_ },
                    noSampler, 1, 1,
                    spawners_.Get());
            }

            auto dispatch = [&](const std::shared_ptr<Material>& material, uint32_t readIdx,
                                int sx, int sy, float dt, float pokeMetres)
            {
                SurfSimCB cb = base;
                cb.deltaTime = dt;
                cb.shiftX = sx;
                cb.shiftY = sy;
                cb.pokeAmp = pokeMetres;
                RecordComputeDispatch(renderer, cl, material.get(),
                    static_cast<UINT>(sizeof(SurfSimCB)),
                    [&cb](std::uint8_t* dst) { std::memcpy(dst, &cb, sizeof(cb)); },
                    srvs,
                    { waveUav_[readIdx], waveUav_[1u - readIdx],
                      foamUav_[readIdx], foamUav_[1u - readIdx], spawnersUav_ },
                    noSampler, kResolution, kResolution,
                    wave_[1u - readIdx].Get());
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
