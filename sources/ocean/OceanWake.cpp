#include "ocean/OceanWake.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "core/Helpers.h"
#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "materials/Material.h"
#include "ocean/BuoyancyLayout.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderContextPool.h"

using Microsoft::WRL::ComPtr;

namespace
{
// CPU mirror of WakeCB (shaders/ocean_wake_cs.hlsl) -- keep the two in step.
struct WakeCB
{
    std::uint32_t resolution; float texel; float centerX; float centerZ;
    std::int32_t shiftX; std::int32_t shiftY; float deltaTime; std::uint32_t clear;
    float gravity; float dampingK2; float strength; std::uint32_t hullCount;
    float edgeTexels; float edgeRate; std::uint32_t shoreValid; float shoreCamHeight;
    float shoreCenterX; float shoreCenterZ; float shoreInvExtent; float shoreZNear;
    float shoreZFar; float breakRate; float pad1; float pad2;
    float hulls[16 * 3 * 4];
};
static_assert(sizeof(WakeCB) == 96 + 16 * 3 * 16, "WakeCB mirrors the shader's cbuffer");

constexpr float kGravity = 9.81f;
// The absorber: 1/8 of the window, 6/s at the border. A wave crossing it at a few m/s is gone long before it could wrap
// round the periodic spectrum; it is smooth enough (rate ~ distance^2) that it reflects nothing measurable.
constexpr float kEdgeShare = 0.125f;
constexpr float kEdgeRate = 6.0f;
constexpr float kPi = 3.14159265358979f;
}

void OceanWake::EnsureResources(Renderer* renderer)
{
    if (created_ || !renderer || !renderer->GetDevice())
    {
        return;
    }
    ID3D12Device* device = renderer->GetDevice();

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
    // 32-bit: the FFT carries N^2 times the field through its butterflies, and the equilibrium under a hull (h = -head)
    // is the difference of two such sums.
    texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const auto create = [&](GpuResource& target, const wchar_t* name)
    {
        ComPtr<ID3D12Resource> res;
        ThrowIfFailed(gpu::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE, texDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, MemTag::Ocean, name, &res));
        target.Attach(renderer->Declarations(), std::move(res), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    create(work_, L"Ocean.WakeWork");
    create(field_, L"Ocean.WakeField");
    create(spec_, L"Ocean.WakeSpectrum");
    // The free height: what the CPU floats things on -- centimetres over metres, half precision is plenty, and the
    // readback ring carries a quarter of the bytes.
    texDesc.Format = DXGI_FORMAT_R16_FLOAT;
    create(free_, L"Ocean.WakeFreeHeight");

    // The footprints: written by the CPU once per new hull shape, read by Pack -- an upload buffer, never in a state the
    // graph tracks (GENERIC_READ for its whole life).
    {
        // A slot holds one footprint grid.
        const std::uint32_t floats = static_cast<std::uint32_t>(buoyancy::kHullFootprintAlong * buoyancy::kHullFootprintAcross);
        footprintFloats_ = floats;
        D3D12_HEAP_PROPERTIES upload{};
        upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = static_cast<UINT64>(kMaxFootprints) * floats * sizeof(float);
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(gpu::CreateCommittedResource(device, &upload, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, MemTag::Ocean, L"Ocean.WakeFootprints", IID_PPV_ARGS(&footprints_)));
        void* mapped = nullptr;
        const D3D12_RANGE none{ 0, 0 };
        ThrowIfFailed(footprints_->Map(0, &none, &mapped));
        footprintsMapped_ = static_cast<float*>(mapped);
        std::memset(footprintsMapped_, 0, static_cast<size_t>(bufDesc.Width));
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 10u;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(gpu::CreateDescriptorHeap(device, &heapDesc, MemTag::Ocean, L"Ocean.WakeHeap", IID_PPV_ARGS(&descriptorHeap_)));
    const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
    const auto next = [&]()
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE current = handle;
        handle.ptr += incr;
        return current;
    };
    workUav_ = next();
    device->CreateUnorderedAccessView(work_.Get(), nullptr, nullptr, workUav_);
    fieldUav_ = next();
    device->CreateUnorderedAccessView(field_.Get(), nullptr, nullptr, fieldUav_);
    // The FFT kernel addresses a Texture2DArray: the same textures seen as a one-slice array.
    D3D12_UNORDERED_ACCESS_VIEW_DESC arrayDesc{};
    arrayDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    arrayDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    arrayDesc.Texture2DArray.ArraySize = 1;
    workFftUav_ = next();
    device->CreateUnorderedAccessView(work_.Get(), nullptr, &arrayDesc, workFftUav_);
    fieldFftUav_ = next();
    device->CreateUnorderedAccessView(field_.Get(), nullptr, &arrayDesc, fieldFftUav_);
    specUav_ = next();
    device->CreateUnorderedAccessView(spec_.Get(), nullptr, nullptr, specUav_);
    specFftUav_ = next();
    device->CreateUnorderedAccessView(spec_.Get(), nullptr, &arrayDesc, specFftUav_);
    freeUav_ = next();
    device->CreateUnorderedAccessView(free_.Get(), nullptr, nullptr, freeUav_);
    shoreNullSrv_ = next();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc{};
        nullDesc.Format = DXGI_FORMAT_R32_FLOAT;
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &nullDesc, shoreNullSrv_);
    }
    fieldSrv_ = next();
    device->CreateShaderResourceView(field_.Get(), nullptr, fieldSrv_);
    footprintsSrv_ = next();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.NumElements = kMaxFootprints * footprintFloats_;
        srvDesc.Buffer.StructureByteStride = sizeof(float);
        device->CreateShaderResourceView(footprints_.Get(), &srvDesc, footprintsSrv_);
    }

    auto* materialMgr = renderer->GetMaterialManager();
    Material::ComputeDesc desc{};
    desc.shaderFile = L"shaders/ocean_wake_cs.hlsl";
    desc.csEntry = "Pack";
    packMaterial_ = materialMgr->GetOrCreateCompute(renderer, desc);
    desc.csEntry = "Step";
    stepMaterial_ = materialMgr->GetOrCreateCompute(renderer, desc);
    desc.csEntry = "Compose";
    composeMaterial_ = materialMgr->GetOrCreateCompute(renderer, desc);
    Material::ComputeDesc fftDesc{};
    fftDesc.shaderFile = L"shaders/ocean_fft.hlsl";
    fftDesc.csEntry = "Fft";
    fftDesc.defines.emplace_back("FFT_SIZE", std::to_string(kResolution));
    fftDesc.defines.emplace_back("FFT_LOG_SIZE", "9");
    static_assert(kResolution == 512u, "FFT_LOG_SIZE follows kResolution");
    fftMaterial_ = materialMgr->GetOrCreateCompute(renderer, fftDesc);

    created_ = true;
    LOG_INFO(logging::LogCategory::Ocean, "wake: field {} x {}, {} footprint slots, 4 depths", kResolution, kResolution,
             kMaxFootprints);
}

void OceanWake::SetHulls(std::vector<ocean::WakeHull> hulls)
{
    hulls_ = std::move(hulls);
}

void OceanWake::TickWindow(Math::float2 cameraXZ, float texel)
{
    if (!created_)
    {
        return;
    }
    if (texel != texel_)
    {
        texel_ = texel;
        hasCenter_ = false;
        needsClear_ = true;
    }
    const float snap = texel_ * kSnapTexels;
    const Math::float2 target(std::floor(cameraXZ.x / snap) * snap + snap * 0.5f,
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
    pendingShiftX_ = static_cast<int>(std::lround((target.x - center_.x) / texel_));
    pendingShiftY_ = static_cast<int>(std::lround((target.y - center_.y) / texel_));
    pendingCenter_ = target;
}

std::uint32_t OceanWake::FootprintSlot(const std::shared_ptr<const buoyancy::HullFootprint>& footprint)
{
    const auto it = footprintSlots_.find(footprint.get());
    if (it != footprintSlots_.end())
    {
        return it->second;
    }
    if (footprintSlots_.size() >= kMaxFootprints || !footprintsMapped_ ||
        static_cast<std::uint32_t>(footprint->along * footprint->across) > footprintFloats_)
    {
        LOG_WARNING_ONCE(logging::LogCategory::Ocean, "wake: more hull shapes than {} footprint slots -- the rest make no wake",
                         kMaxFootprints);
        return ~0u;
    }
    // A new slot: no frame in flight reads it yet, so the CPU may write it now.
    const std::uint32_t slot = static_cast<std::uint32_t>(footprintSlots_.size());
    std::memcpy(footprintsMapped_ + static_cast<size_t>(slot) * footprintFloats_, footprint->depth.data(),
                footprint->depth.size() * sizeof(float));
    footprintSlots_.emplace(footprint.get(), slot);
    footprintKeep_.push_back(footprint); // the key is a pointer: it must not be freed and reused by another hull
    return slot;
}

std::function<void(RenderGraphPassContext)> OceanWake::BuildPass(RenderGraphPassContext& ctx, float timeSeconds,
    const Tuning& tuning, const ShoreWindow* shore, bool hold)
{
    if (!created_ || !packMaterial_ || !stepMaterial_ || !composeMaterial_ || !fftMaterial_)
    {
        active_ = false;
        return {};
    }

    // ---- decisions, once ----
    // A frame that re-renders the shore maps hands the field over as it stands: a step of no time turns no mode, so the
    // coast it cannot read this frame holds anything that already met it.
    const float elapsed = previousTime_ >= 0.0f && !hold ? std::clamp(timeSeconds - previousTime_, 0.0f, kMaxStep) : 0.0f;
    previousTime_ = timeSeconds;

    RecordPass r{};
    r.hasShore = !hold && shore && shore->srv.ptr != 0 && shore->resource != nullptr && shore->invExtent > 0.0f;
    if (r.hasShore)
    {
        r.shore = *shore;
    }
    for (const ocean::WakeHull& hull : hulls_)
    {
        if (r.hullCount >= kMaxHulls)
        {
            break;
        }
        if (!hull.footprint || !hull.footprint->Valid() || !(hull.scale > 0.0f))
        {
            continue;
        }
        const std::uint32_t slot = FootprintSlot(hull.footprint);
        if (slot == ~0u)
        {
            continue;
        }
        const float fl = std::sqrt(hull.forward.x * hull.forward.x + hull.forward.y * hull.forward.y);
        if (!(fl > 1e-4f))
        {
            continue;
        }
        const buoyancy::HullFootprint& fp = *hull.footprint;
        HullGpu& g = r.hulls[r.hullCount++];
        g = HullGpu{ hull.position.x, hull.position.y, hull.forward.x / fl, hull.forward.y / fl,
                     hull.scale, fp.u0, fp.v0, 1.0f / fp.cellU,
                     1.0f / fp.cellV, static_cast<float>(fp.along), static_cast<float>(fp.across),
                     static_cast<float>(slot * footprintFloats_) };
    }
    if (r.hullCount > 0)
    {
        lastHullTime_ = timeSeconds;
    }
    const bool wasActive = active_;
    active_ = r.hullCount > 0 || (everSimulated_ && timeSeconds - lastHullTime_ < kLingerSeconds);
    if (!active_)
    {
        // Let the field go: the next boat starts on still water, wherever the camera has gone since.
        if (wasActive)
        {
            needsClear_ = true;
            hasCenter_ = false;
            LOG_INFO(logging::LogCategory::Ocean, "wake: no boat for {:.0f} s -- the field is let go", kLingerSeconds);
        }
        return {};
    }

    r.shiftX = pendingShiftX_;
    r.shiftY = pendingShiftY_;
    center_ = pendingCenter_;
    pendingShiftX_ = 0;
    pendingShiftY_ = 0;
    r.clear = needsClear_ || std::abs(r.shiftX) >= static_cast<int>(kResolution) || std::abs(r.shiftY) >= static_cast<int>(kResolution);
    needsClear_ = false;
    r.dt = elapsed;
    r.center = center_;
    r.texel = texel_;
    r.tuning = tuning;
    everSimulated_ = true;

    // ---- declarations, from the same decisions ----
    ctx.NextPoint();
    r.uavPoint = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(work_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(field_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(spec_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(free_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (r.hasShore)
    {
        ctx.Use(r.shore.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ctx.NextPoint();
    ctx.Use(field_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ranFrame_ = ctx.renderer ? ctx.renderer->GetTotalFrameNumber() : 0u;
    record_ = r;
    return [this](RenderGraphPassContext c)
    {
        const RecordPass& rec = record_;
        Renderer* renderer = c.renderer;
        auto t = c.BeginCL();
        SetCommandListName(t.cl, c.pass);
        ID3D12GraphicsCommandList* cl = t.cl;
        {
            GPU_SCOPE(cl, ProfilerScopes::kOceanWake);
            renderer->EmitPoint(cl, rec.uavPoint);

            WakeCB cb{};
            cb.resolution = kResolution;
            cb.texel = rec.texel;
            cb.centerX = rec.center.x;
            cb.centerZ = rec.center.y;
            cb.shiftX = rec.shiftX;
            cb.shiftY = rec.shiftY;
            cb.deltaTime = rec.dt;
            cb.clear = rec.clear ? 1u : 0u;
            cb.gravity = kGravity;
            const float kMax = kPi / rec.texel; // the shortest wave the field holds: 2 texels
            cb.dampingK2 = rec.tuning.damping / (kMax * kMax);
            cb.strength = rec.tuning.strength;
            cb.hullCount = rec.hullCount;
            cb.edgeTexels = kEdgeShare * static_cast<float>(kResolution);
            cb.edgeRate = kEdgeRate;
            cb.shoreValid = rec.hasShore ? 1u : 0u;
            cb.shoreCamHeight = rec.shore.camHeight;
            cb.shoreCenterX = rec.shore.center.x;
            cb.shoreCenterZ = rec.shore.center.y;
            cb.shoreInvExtent = rec.shore.invExtent;
            cb.shoreZNear = rec.shore.zNear;
            cb.shoreZFar = rec.shore.zFar;
            cb.breakRate = rec.hasShore ? rec.tuning.breaking : 0.0f;
            static_assert(sizeof(HullGpu) == 12 * sizeof(float), "three float4 a hull");
            std::memcpy(cb.hulls, rec.hulls, sizeof(HullGpu) * rec.hullCount);

            const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};
            const auto writeCb = [&cb](std::uint8_t* dst) { std::memcpy(dst, &cb, sizeof(cb)); };
            const D3D12_CPU_DESCRIPTOR_HANDLE shoreSrv = rec.hasShore ? rec.shore.srv : shoreNullSrv_;
            const auto srvs = { footprintsSrv_, shoreSrv };
            const auto uavs = { workUav_, fieldUav_, specUav_, freeUav_ };

            RecordComputeDispatch(renderer, cl, packMaterial_.get(), static_cast<UINT>(sizeof(WakeCB)), writeCb,
                srvs, uavs, noSampler, kResolution, kResolution, work_.Get());

            const auto fft = [&](D3D12_CPU_DESCRIPTOR_HANDLE target, ID3D12Resource* resource, std::uint32_t inverse)
            {
                const auto table = renderer->StageSrvUavTable({ target });
                for (std::uint32_t direction = 0; direction < 2u; ++direction)
                {
                    auto h = renderer->GetRenderContextPool()->Acquire();
                    RenderContext& rc = h.ref();
                    rc.constants[0] = { 1u, direction, inverse, 0u };
                    rc.uavTable[0] = table.gpu;
                    fftMaterial_->Bind(cl, rc);
                    cl->Dispatch(1, kResolution, 1);
                    renderer->UAVBarrier(cl, resource);
                }
            };
            fft(workFftUav_, work_.Get(), 0u);

            RecordComputeDispatch(renderer, cl, stepMaterial_.get(), static_cast<UINT>(sizeof(WakeCB)), writeCb,
                srvs, uavs, noSampler, kResolution, kResolution, field_.Get());
            renderer->UAVBarrier(cl, spec_.Get());

            fft(fieldFftUav_, field_.Get(), 1u);
            fft(specFftUav_, spec_.Get(), 1u);

            // Compose reads and writes only its own texel of the field: no barrier between its read and its write.
            RecordComputeDispatch(renderer, cl, composeMaterial_.get(), static_cast<UINT>(sizeof(WakeCB)), writeCb,
                srvs, uavs, noSampler, kResolution, kResolution, field_.Get());

            renderer->EmitPoint(cl, rec.uavPoint + 1u); // the field to pixel-readable for the surface
        }
        c.EndCL(t);
    };
}

Math::float4 OceanWake::GetWindowParams() const
{
    const float half = 0.5f * static_cast<float>(kResolution) * texel_;
    return Math::float4(center_.x, center_.y, 1.0f / half, texel_);
}

D3D12_CPU_DESCRIPTOR_HANDLE OceanWake::GetSrv() const
{
    return created_ ? fieldSrv_ : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
