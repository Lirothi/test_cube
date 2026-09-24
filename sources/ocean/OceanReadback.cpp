#include "ocean/OceanReadback.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <immintrin.h> // F16C: four halves to four floats in one instruction

#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "ocean/OceanRenderable.h"
#include "ocean/OceanSimulation.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/Renderer.h"

namespace
{
    constexpr UINT64 kPlacement = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    constexpr std::chrono::microseconds kMinCopyInterval{ 1000000 / 30 };
    UINT64 AlignUp(UINT64 v, UINT64 a) { return (v + a - 1u) / a * a; }

    std::uint32_t CascadesFor(OceanSimulationSettings::ReadbackCascadesMode mode)
    {
        switch (mode)
        {
        case OceanSimulationSettings::ReadbackCascadesMode::One: return 1u;
        case OceanSimulationSettings::ReadbackCascadesMode::Two: return 2u;
        default: return 0u;
        }
    }

    // ocean_surface_common.hlsli EaseInOutClamped / LodWeights.
    float EaseInOutClamped(float x)
    {
        x = std::clamp(x, 0.0f, 1.0f);
        return 3.0f * x * x - 2.0f * x * x * x;
    }

    // One RGBA16F texel -> xyz.
    Math::float3 FetchTexel(const std::uint8_t* texel)
    {
        alignas(16) float v[4];
        _mm_store_ps(v, _mm_cvtph_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(texel))));
        return Math::float3(v[0], v[1], v[2]);
    }
}

bool OceanReadback::EnsureBuffer(Renderer* renderer, ID3D12Resource* displacement, ID3D12Resource* shore,
    std::uint32_t cascades)
{
    const D3D12_RESOURCE_DESC dd = displacement->GetDesc();
    const std::uint32_t resolution = static_cast<std::uint32_t>(dd.Width);
    D3D12_RESOURCE_DESC sd{};
    if (shore) { sd = shore->GetDesc(); }
    const std::uint32_t shoreW = shore ? static_cast<std::uint32_t>(sd.Width) : 0u;
    const std::uint32_t shoreH = shore ? static_cast<std::uint32_t>(sd.Height) : 0u;
    if (buffer_ && resolution == layoutResolution_ && cascades <= layoutCascades_ &&
        shoreW == layoutShoreW_ && shoreH == layoutShoreH_)
    {
        return true;
    }

    const std::uint64_t now = renderer->GetTotalFrameNumber();
    if (buffer_)
    {
        retired_.emplace_back(now, buffer_);
        buffer_.Reset();
        mapped_ = nullptr;
    }
    for (Slot& slot : slots_) { slot.frame = 0; }
    adoptedFrame_ = 0; // the adopted slot lived in the old ring
    shoreQueued_ = false;

    ID3D12Device* device = renderer->GetDevice();
    // Room for two cascades whatever the mode says, so flipping None/One/Two never reallocates.
    UINT64 bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    device->GetCopyableFootprints(&dd, 0u, 1u, 0u, &fp, nullptr, nullptr, &bytes);
    UINT64 offset = 0;
    for (D3D12_PLACED_SUBRESOURCE_FOOTPRINT& c : cascadeFootprint_)
    {
        c = fp;
        c.Offset = offset;
        offset += AlignUp(bytes, kPlacement);
    }
    shoreFootprint_ = {};
    if (shore)
    {
        UINT64 shoreBytes = 0;
        device->GetCopyableFootprints(&sd, 0u, 1u, 0u, &shoreFootprint_, nullptr, nullptr, &shoreBytes);
        shoreFootprint_.Offset = offset;
        offset += AlignUp(shoreBytes, kPlacement);
    }
    slotBytes_ = AlignUp(offset, kPlacement);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = slotBytes_ * render::kFrameCount;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(buffer_.GetAddressOf()))) ||
        FAILED(buffer_->Map(0, nullptr, &mapped)) || !mapped)
    {
        buffer_.Reset();
        LOG_ERROR(logging::LogCategory::Ocean, "ocean readback: could not create/map the {} byte ring", rd.Width);
        return false;
    }
    buffer_->SetName(L"Ocean.Readback");
    mapped_ = static_cast<const std::uint8_t*>(mapped);
    layoutResolution_ = resolution;
    layoutCascades_ = 2u;
    layoutShoreW_ = shoreW;
    layoutShoreH_ = shoreH;
    LOG_INFO(logging::LogCategory::Ocean,
        "ocean readback: ring {} x {:.2f} MB (2 cascades {}^2 RGBA16F + shore {}x{} D16), persistently mapped",
        render::kFrameCount, static_cast<double>(slotBytes_) / (1024.0 * 1024.0), resolution, shoreW, shoreH);
    return true;
}

std::function<void(RenderGraphPassContext)> OceanReadback::BuildPass(RenderGraphPassContext& ctx,
    const OceanRenderable& ocean)
{
    if (!requested_) { return {}; }
    requested_ = false;

    Renderer* renderer = ctx.renderer;
    const OceanSimulation* sim = ocean.GetSimulation();
    if (!renderer || !sim) { return {}; }
    ID3D12Resource* displacement = sim->GetDisplacementResource();
    const std::uint32_t cascades = std::min({ CascadesFor(sim->GetSettings().GetReadbackMode()),
                                              sim->GetCascadeCount(), 2u });
    if (!displacement || cascades == 0u) { return {}; }
    ID3D12Resource* shore = sim->GetShoreDepthResource();
    if (shore && sim->GetShoreDepthSrv().ptr == 0) { shore = nullptr; }
    // A re-rendered shore map is owed a copy even on a frame the rate limit below skips.
    if (sim->ShouldRenderShoreDepth()) { shoreQueued_ = false; }

    // At most 30 copies a second, whatever the frame rate: a swell with a period of seconds is fully
    // described at 30 Hz, and between copies every pontoon carries its height forward at its own
    // rate of change (OceanBuoyancy). At 60 fps this copies every other frame, at 340 fps about one
    // frame in eleven -- the 40 us of PCIe copy stops scaling with the frame rate.
    const auto now = std::chrono::steady_clock::now();
    if (lastCopy_ != std::chrono::steady_clock::time_point{} && now - lastCopy_ < kMinCopyInterval)
    {
        return {};
    }
    if (!EnsureBuffer(renderer, displacement, shore, cascades)) { return {}; }

    // The ring slot is chosen, not implied by the frame index: with copies spaced out, the ADOPTED
    // copy can stay current for many frames, and SampleHeight reads it in place -- a copy landing in
    // that slot would be written by the GPU while the CPU reads it. So: never the adopted slot, never
    // one whose copy is still in flight; if none is free, this copy waits for the next frame.
    UINT slotIndex = render::kFrameCount;
    for (UINT candidate = 0; candidate < render::kFrameCount; ++candidate)
    {
        const bool beingRead = adoptedFrame_ != 0 && candidate == adoptedSlot_;
        if (!beingRead && slots_[candidate].frame == 0) { slotIndex = candidate; break; }
    }
    if (slotIndex == render::kFrameCount) { return {}; }
    lastCopy_ = now;

    Slot& slot = slots_[slotIndex];
    slot.frame = renderer->GetTotalFrameNumber();
    slot.frameSlot = renderer->GetCurrentFrameIndex(); // whose fence says the copy has landed
    SurfaceParams& s = slot.surface;
    s.lengthScales = sim->GetLengthScales();
    const Math::float4 viewer = ocean.GetClipMapViewer();
    s.viewer = Math::float3(viewer.x, viewer.y, viewer.z);
    s.fadeScale = ocean.GetClipMapParams().w;
    s.warpStrength = ocean.GetWindParams0().w;
    s.waterLevel = ocean.GetWaterLevel();
    s.time = ocean.GetElapsedTime();
    s.cascades = cascades;
    s.simCascades = sim->GetCascadeCount();
    s.resolution = layoutResolution_;
    s.iterations = std::max(sim->GetSettings().GetSamplingIterations(), 1u);

    // The shore map follows the camera and is re-rendered only when it crosses a snap step: one
    // copy per rendering is all the CPU needs (re-armed above, on whichever frame it re-rendered).
    const bool copyShore = shore != nullptr && !shoreQueued_;
    slot.hasShore = copyShore;
    if (copyShore)
    {
        const Math::float4 depth = ocean.GetShoreDepthParams();
        slot.shore.view = ocean.GetShoreViewParams();
        slot.shore.depthRange = Math::float2(depth.x, depth.y);
        slot.shore.damp = ocean.GetShoreLegacyDampParams();
        slot.shore.width = layoutShoreW_;
        slot.shore.height = layoutShoreH_;
        shoreQueued_ = true;
    }

    // Slice 2c of the displacement array is cascade c's (Dx, Dy, Dz, dDz/dx); mip 0.
    const D3D12_RESOURCE_DESC dd = displacement->GetDesc();
    std::array<UINT, 2> subresource{};
    for (std::uint32_t c = 0; c < cascades; ++c) { subresource[c] = 2u * c * dd.MipLevels; }

    ctx.NextPoint();
    const std::uint32_t copyPoint = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(displacement, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (copyShore) { ctx.Use(shore, D3D12_RESOURCE_STATE_COPY_SOURCE); }
    ctx.NextPoint();
    const std::uint32_t restorePoint = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(displacement, OceanSimulation::kSimMapReadState);
    if (copyShore)
    {
        ctx.Use(shore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    ID3D12Resource* const dst = buffer_.Get();
    const UINT64 base = static_cast<UINT64>(slotIndex) * slotBytes_;
    const auto cascadeFp = cascadeFootprint_;
    const auto shoreFp = shoreFootprint_;
    return [dst, base, cascadeFp, shoreFp, subresource, cascades, copyShore, displacement, shore,
               copyPoint, restorePoint](RenderGraphPassContext pass)
    {
        auto token = pass.BeginCL();
        SetCommandListName(token.cl, pass.pass);
        ID3D12GraphicsCommandList* cl = token.cl;
        {
            // Scoped INSIDE the list's lifetime: this pass is not in a CL group, so EndCL really
            // closes the list, and a GPU scope still open then writes into a closed one.
            GPU_SCOPE(cl, ProfilerScopes::kPassOceanReadback);
            pass.renderer->EmitPoint(cl, copyPoint);
            const auto copy = [cl, dst, base](ID3D12Resource* source, UINT subresourceIndex,
                                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint)
            {
                D3D12_TEXTURE_COPY_LOCATION to{};
                to.pResource = dst;
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = footprint;
                to.PlacedFootprint.Offset += base;
                D3D12_TEXTURE_COPY_LOCATION from{};
                from.pResource = source;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = subresourceIndex;
                cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            };
            for (std::uint32_t c = 0; c < cascades; ++c) { copy(displacement, subresource[c], cascadeFp[c]); }
            if (copyShore) { copy(shore, 0u, shoreFp); }
            pass.renderer->EmitPoint(cl, restorePoint);
        }
        pass.EndCL(token);
    };
}

void OceanReadback::Poll(Renderer* renderer)
{
    if (!renderer) { return; }
    const std::uint64_t now = renderer->GetTotalFrameNumber();
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
        [now](const auto& entry) { return now > entry.first + render::kFrameCount + 1u; }), retired_.end());
    if (!buffer_ || !mapped_) { return; }

    // Every finished slot, oldest first: the newest brings the cascades, but an older one may be
    // the only carrier of a shore-map update and must not be skipped.
    std::array<UINT, render::kFrameCount> ready{};
    size_t count = 0;
    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        // The fence of the FRAME that recorded the copy. Once a later frame reuses that frame slot the
        // answer is about the later frame -- conservative (a finished copy may wait for it), never early.
        if (slots_[f].frame != 0 && renderer->IsFrameSlotComplete(slots_[f].frameSlot)) { ready[count++] = f; }
    }
    if (count == 0) { return; }
    std::sort(ready.begin(), ready.begin() + static_cast<std::ptrdiff_t>(count),
        [this](UINT a, UINT b) { return slots_[a].frame < slots_[b].frame; });

    for (size_t i = 0; i < count; ++i)
    {
        Slot& slot = slots_[ready[i]];
        if (slot.hasShore && shoreFootprint_.Footprint.Width != 0)
        {
            // The one copy: the shore map outlives its slot (it changes only with the camera).
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = shoreFootprint_;
            const std::uint8_t* src = mapped_ + static_cast<size_t>(ready[i]) * slotBytes_ + fp.Offset;
            const size_t rowBytes = static_cast<size_t>(fp.Footprint.Width) * sizeof(std::uint16_t);
            shoreTexels_.resize(static_cast<size_t>(fp.Footprint.Width) * fp.Footprint.Height);
            for (std::uint32_t row = 0; row < fp.Footprint.Height; ++row)
            {
                std::memcpy(shoreTexels_.data() + static_cast<size_t>(row) * fp.Footprint.Width,
                    src + static_cast<size_t>(row) * fp.Footprint.RowPitch, rowBytes);
            }
            shore_ = slot.shore;
            hasShore_ = true;
        }
        if (i + 1 == count)
        {
            surface_ = slot.surface;
            adoptedSlot_ = ready[i];
            latency_ = static_cast<std::uint32_t>(now - slot.frame);
            if (adoptedFrame_ == 0)
            {
                LOG_INFO(logging::LogCategory::Ocean,
                    "ocean readback: surface adopted ({} cascade(s), {} iterations, {} frame(s) old)",
                    surface_.cascades, surface_.iterations, latency_);
            }
            adoptedFrame_ = slot.frame;
        }
        slot.frame = 0;
    }
}

Math::float3 OceanReadback::SampleCascade(std::uint32_t cascade, float u, float v) const
{
    const std::int32_t res = static_cast<std::int32_t>(surface_.resolution);
    if (res <= 0 || !mapped_ || cascade >= 2u) { return Math::float3(0.0f); }
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = cascadeFootprint_[cascade];
    const std::uint8_t* base = mapped_ + static_cast<size_t>(adoptedSlot_) * slotBytes_ + fp.Offset;
    const size_t pitch = fp.Footprint.RowPitch;
    // LinearWrapSampler, mip 0.
    const float x = u * res - 0.5f;
    const float y = v * res - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    const auto wrap = [res](std::int64_t i) { return static_cast<std::int32_t>(((i % res) + res) % res); };
    const std::int32_t x0 = wrap(static_cast<std::int64_t>(fx)), x1 = wrap(static_cast<std::int64_t>(fx) + 1);
    const std::int32_t y0 = wrap(static_cast<std::int64_t>(fy)), y1 = wrap(static_cast<std::int64_t>(fy) + 1);
    const auto fetch = [&](std::int32_t ix, std::int32_t iy)
    {
        return FetchTexel(base + static_cast<size_t>(iy) * pitch + static_cast<size_t>(ix) * 8u);
    };
    const Math::float3 a = Math::float3::Lerp(fetch(x0, y0), fetch(x1, y0), tx);
    const Math::float3 b = Math::float3::Lerp(fetch(x0, y1), fetch(x1, y1), tx);
    return Math::float3::Lerp(a, b, ty);
}

float OceanReadback::SampleShoreDepth(float u, float v) const
{
    // LinearClampSampler over the D16 map.
    const std::int32_t w = static_cast<std::int32_t>(shore_.width), h = static_cast<std::int32_t>(shore_.height);
    if (w <= 0 || h <= 0 || shoreTexels_.size() < static_cast<size_t>(w) * h) { return 0.0f; }
    const float x = u * w - 0.5f, y = v * h - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    const auto clampX = [w](float i) { return std::clamp(static_cast<std::int32_t>(i), 0, w - 1); };
    const auto clampY = [h](float i) { return std::clamp(static_cast<std::int32_t>(i), 0, h - 1); };
    const auto fetch = [&](std::int32_t ix, std::int32_t iy)
    {
        return static_cast<float>(shoreTexels_[static_cast<size_t>(iy) * w + ix]) / 65535.0f;
    };
    const std::int32_t x0 = clampX(fx), x1 = clampX(fx + 1.0f), y0 = clampY(fy), y1 = clampY(fy + 1.0f);
    const float a = fetch(x0, y0) + (fetch(x1, y0) - fetch(x0, y0)) * tx;
    const float b = fetch(x0, y1) + (fetch(x1, y1) - fetch(x0, y1)) * tx;
    return a + (b - a) * ty;
}

Math::float3 OceanReadback::Displacement(float baseX, float baseZ) const
{
    const SurfaceParams& s = surface_;
    // The weights and the warp measure from the camera the copy's frame rendered with.
    const float dx = baseX - s.viewer.x, dy = -s.viewer.y, dz = baseZ - s.viewer.z;
    const float viewDist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float viewDistXzSquared = dx * dx + dz * dz;

    const float warpDistance = std::max(s.lengthScales.x, 1.0f) * 0.5f;
    const float warpScale = std::min(1.0f, viewDistXzSquared / std::max(warpDistance * warpDistance * 100.0f, 1.0f));
    const float warpInv = 1.0f / std::max(warpDistance, 1e-3f);
    const float warp = warpDistance * 0.4f * s.warpStrength * warpScale;
    const float uvX = warp != 0.0f ? baseX + std::sin(baseZ * warpInv) * warp : baseX;
    const float uvZ = warp != 0.0f ? baseZ + std::sin(baseX * warpInv) * warp : baseZ;

    const float lengths[4] = { s.lengthScales.x, s.lengthScales.y, s.lengthScales.z, s.lengthScales.w };
    Math::float3 d(0.0f);
    const std::uint32_t cascades = std::min(s.cascades, std::min(s.simCascades, 2u));
    for (std::uint32_t c = 0; c < cascades; ++c)
    {
        const float length = std::max(lengths[c], 1e-3f);
        const float fade = std::max(length * s.fadeScale, 1e-3f);
        const float weight = 1.0f - EaseInOutClamped((viewDist - fade) / fade);
        if (c == 0 || weight > 0.05f)
        {
            d += SampleCascade(c, uvX / length, uvZ / length) * weight;
        }
    }

    if (hasShore_)
    {
        const Math::float4& view = shore_.view;
        const float su = (uvX - view.x) * view.w + 0.5f;
        const float sv = 0.5f - (uvZ - view.y) * view.w;
        if (su >= 0.0f && su <= 1.0f && sv >= 0.0f && sv <= 1.0f)
        {
            const float depthSample = SampleShoreDepth(su, sv);
            if (depthSample > 0.0f)
            {
                const float viewDepth = shore_.depthRange.x + (shore_.depthRange.y - shore_.depthRange.x) * depthSample;
                const float waterDepth = -(view.z - viewDepth);
                const float depthFade = std::clamp(waterDepth / std::max(shore_.damp.z, 0.01f), 0.0f, 1.0f);
                const float vertical = (1.0f - std::clamp(shore_.damp.x, 0.0f, 1.0f)) +
                    std::clamp(shore_.damp.x, 0.0f, 1.0f) * depthFade;
                const float horizontal = (1.0f - std::clamp(shore_.damp.y, 0.0f, 1.0f)) +
                    std::clamp(shore_.damp.y, 0.0f, 1.0f) * depthFade;
                d.y *= vertical;
                d.x *= horizontal;
                d.z *= horizontal;
            }
        }
    }
    return d;
}

float OceanReadback::SampleHeight(float x, float z) const
{
    if (!HasSurface()) { return surface_.waterLevel; }
    // The surface point over (x, z) was pushed there from some grid point X: X + D.xz(X) = (x, z).
    float bx = x, bz = z;
    for (std::uint32_t i = 0; i < surface_.iterations; ++i)
    {
        const Math::float3 d = Displacement(bx, bz);
        bx = x - d.x;
        bz = z - d.z;
    }
    return surface_.waterLevel + Displacement(bx, bz).y;
}
