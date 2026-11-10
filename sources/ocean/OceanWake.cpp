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
    float shoreZFar; float breakRate; float foamFade; float foamDiffusion;
    float washStrength; float washThreshold; float crestStrength; float crestCoverage;
    float foamFadeDense; std::uint32_t frontCount; float washBow; float pad3;
    float hulls[16 * 3 * 4];
    float frontBounds[8 * 8 * 4];   // the bow waves' fronts, eight pieces each: (min x, min z, max x, max z) world
    float frontInfo[8 * 4];         // (points, outward side +-1, the residue's rate factor, 0)
    float frontPoints[8 * 128 * 4]; // (x, z, the band's foam, half width m), newest first
};
static_assert(sizeof(WakeCB) == 128 + 16 * 3 * 16 + 8 * 8 * 16 + 8 * 16 + 8 * 128 * 16, "WakeCB mirrors the shader's cbuffer");

constexpr float kGravity = 9.81f;
// The absorber: 1/8 of the window, 6/s at the border. A wave crossing it at a few m/s is gone long before it could wrap
// round the periodic spectrum; it is smooth enough (rate ~ distance^2) that it reflects nothing measurable.
constexpr float kEdgeShare = 0.125f;
constexpr float kEdgeRate = 6.0f;
constexpr float kPi = 3.14159265358979f;
// The foam spreads as nu = Tuning::foamSpread m2/s (0.3: a trail 2-3 m wider after 10 s); the 4-neighbour Laplacian is
// stable while nu dt / texel^2 stays under 1/4 -- capped just under it (0.3 at a 0.5 m texel and the step's 1/20 s cap
// is 0.06).
constexpr float kFoamDiffusionCap = 0.24f;
// A hull's head changing slower than this (m/s) leaves no white water: a boat drifting or idling stays clean.
constexpr float kWashThreshold = 1.0f;
// A hull that moved more than this in one step (m/s) jumped (placed, put back, teleported): it leaves no white water.
constexpr float kWashJumpSpeed = 40.0f;
// ---- the breaking bow wave's fronts (OceanWake.h says what they are) ----
// A point leaves each side this often (s); with the cap on a front's points, the oldest is kFrontMaxAge.
constexpr float kFrontSpacing = 0.125f;
constexpr float kFrontMaxAge = 15.5f;
// The sheet a chine throws: outward speed on top of the wave's at birth, gone e-fold in kFrontThrowSeconds, having
// carried the front Tuning::bowThrow seconds of the hull's speed outside the waterline (0.09: 1.4 m at 16 m/s, where
// the spray's replica lands its nearer drops).
constexpr float kFrontThrowSeconds = 0.35f;
// The arms come in with the hull's speed: none under this (m/s: a hull pushing the water aside without throwing it),
// whole from Tuning::bowSpeedFull -- the spray's own gate at its default.
constexpr float kFrontSpeedOn = 3.0f;
// The diverging wave's speed outward, m/s: Kelvin's wedge at displacement speeds (tan 19.47 degrees of the hull's),
// and past Froude ~0.57 the speed of the waves a hull of that length throws most of -- the wedge then narrows as
// 0.2 / Fr (Rabaud & Moisy 2013), i.e. the arms run out at 0.2 sqrt(g L) whatever the speed: 2 m/s for a 10 m hull,
// which is where the field's own outer crest runs (ocean.wakeDebug:1 at 15 m/s: 11 m out, 82 m behind).
constexpr float kFrontKelvin = 0.354f;
constexpr float kFrontFroude = 0.2f;
// The breaking band's half width is Tuning::bowWidth at birth (at least 1.5 texels) and widens at Tuning::bowWiden. The
// shader's profile reaches kFrontToe of it ahead of the line (the breaker's toe, sharp) and kFrontTail behind (MUST
// MATCH ocean_wake_cs.hlsl).
constexpr float kFrontToe = 0.6f;
constexpr float kFrontTail = 1.6f;
// The band's foam on a fresh front at bowFoam 1 and a planing hull (the wake's units: 1 and over = white). The share
// of it a texel keeps once the front has passed (the lace behind the arm) is Tuning::bowLace.
constexpr float kFrontFoam = 1.5f;
// The scallops: a leaving point's wave speed is 1 +- Tuning::bowScallop times a short random walk (lobes of two or
// three points). Small by default (0.2): the offset it makes grows with the point's age (0.5 stepped the arms' edges
// out by metres every 20 m).
// A hull further than this (m) from where the last step had it is another place, not a step of its way: its fronts go.
constexpr float kFrontJump = 8.0f;

float NextRandom(std::uint32_t& state) // 0..1, xorshift32
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

// A point's outward speed now, and how far it runs in the next `dt` (the throw's decay integrated exactly).
float FrontSpeed(float spread, float thrown, float age)
{
    return spread + thrown * std::exp(-age / kFrontThrowSeconds);
}

float FrontTravel(float spread, float thrown, float age, float dt)
{
    return spread * dt + thrown * kFrontThrowSeconds * (std::exp(-age / kFrontThrowSeconds) - std::exp(-(age + dt) / kFrontThrowSeconds));
}
}

OceanWake::FrontPoint OceanWake::BirthOf(const FrontSource& source, const Tuning& tuning, float spreadJitter, float throwJitter,
                                         float strengthJitter)
{
    FrontPoint point;
    point.position = source.root;
    point.outward = source.outward;
    const float waveSpeed = std::min(kFrontKelvin * source.speed, kFrontFroude * std::sqrt(kGravity * std::max(source.length, 1.0f)));
    point.spread = waveSpeed * tuning.bowSpread * spreadJitter;
    point.thrown = tuning.bowThrow * source.speed / kFrontThrowSeconds * throwJitter;
    point.strength = kFrontFoam * tuning.bowFoam * source.share * strengthJitter;
    point.age = 0.0f;
    return point;
}

// One step of every front: the points run outward and age, each side seen with a bow wave lets a new one go when it is
// due, and the fronts are packed for the foam's Pack -- a side's line starts ON the hull (a point made for this step at
// the root) and runs through its points, newest first.
void OceanWake::StepFronts(float dt, bool keep, const std::vector<FrontSource>& sources, const Tuning& tuning, RecordPass& r)
{
    if (!keep)
    {
        // A cleared field, a hull placed or put back: a line from where it was to where it is would cross the sea.
        fronts_.clear();
    }
    for (Front& front : fronts_)
    {
        front.seen = false;
        for (FrontPoint& point : front.points)
        {
            const float travel = FrontTravel(point.spread, point.thrown, point.age, dt);
            point.position = Math::float2(point.position.x + point.outward.x * travel, point.position.y + point.outward.y * travel);
            point.age += dt;
        }
        while (!front.points.empty() && front.points.back().age > kFrontMaxAge)
        {
            front.points.pop_back();
        }
    }

    r.frontCount = 0;
    const float texelFloor = 1.5f * r.texel;
    const float life = std::max(tuning.bowLife, 0.1f);
    const auto pack = [&](const FrontPoint& point)
    {
        const float halfWidth = std::max(tuning.bowWidth + tuning.bowWiden * point.age, texelFloor);
        // The breaking runs down with the front's age, and out over its last second (no step where the oldest point goes).
        // (How it comes IN where it leaves the hull is not here: BowGates, the surface's every frame.)
        const float alive = std::exp(-point.age / life) * std::clamp(kFrontMaxAge - point.age, 0.0f, 1.0f);
        return Math::float4(point.position.x, point.position.y, std::min(point.strength * alive, 2.0f), halfWidth);
    };
    const auto emit = [&](const Front& front, const FrontPoint* head)
    {
        const std::uint32_t count = static_cast<std::uint32_t>(std::min<size_t>(front.points.size() + (head ? 1u : 0u), kMaxFrontPoints));
        if (count < 2u)
        {
            return;
        }
        if (!(tuning.bowFoam > 0.0f))
        {
            return; // no arms asked for: the points run on, the GPU is handed none
        }
        if (r.frontCount >= kMaxFronts)
        {
            LOG_WARNING_ONCE(logging::LogCategory::Ocean, "wake: more than {} bow wave fronts -- the rest lay no foam", kMaxFronts);
            return;
        }
        Math::float4* out = r.frontPoints + static_cast<size_t>(r.frontCount) * kMaxFrontPoints;
        std::uint32_t written = 0;
        if (head)
        {
            out[written++] = pack(*head);
        }
        for (size_t i = 0; i < front.points.size() && written < count; ++i)
        {
            out[written++] = pack(front.points[i]);
        }
        // A box a PIECE of kFrontPiece segments (its points and the next piece's first): one box round a whole arm
        // lying across the axes covered most of the window, and every texel in it walked all its segments.
        const std::uint32_t pieces = kMaxFrontPoints / kFrontPiece;
        for (std::uint32_t piece = 0; piece < pieces; ++piece)
        {
            Math::float4 box(1.0e9f, 1.0e9f, -1.0e9f, -1.0e9f); // empty: no texel is inside
            for (std::uint32_t i = piece * kFrontPiece; i < written && i <= (piece + 1u) * kFrontPiece; ++i)
            {
                const float reach = out[i].w * kFrontTail;
                box.x = std::min(box.x, out[i].x - reach);
                box.y = std::min(box.y, out[i].y - reach);
                box.z = std::max(box.z, out[i].x + reach);
                box.w = std::max(box.w, out[i].y + reach);
            }
            r.frontBounds[r.frontCount * pieces + piece] = box;
        }
        // Which side of the line (walked from the hull aft) is outward: its right for port, its left for starboard --
        // the side's own, never read off a piece of the line. (Read off its first piece, which has no length on the
        // step a point leaves, the port arm's toe and tail swapped every eighth of a second: owner, watching it, "the
        // left edge of the foam jerks to and fro".)
        const FrontPoint& first = head ? *head : front.points.front();
        const float turn = front.side == 0 ? -1.0f : 1.0f;
        // The residue: a texel the band passes at the front's speed lies under (kFrontToe + kFrontTail) / 3 of its half
        // width of the (1 - d / reach)^2 profile, in half-width / speed seconds: this factor x foam / half width, a
        // second, leaves it bowLace of the band's foam.
        const float speed = FrontSpeed(first.spread, 0.0f, 0.0f);
        const float residue = tuning.bowLace * speed * 3.0f / (kFrontToe + kFrontTail);
        // The residue comes in along the front's first points as the breaker does over its gate (BowGates): in points,
        // for a front that still leaves its hull.
        const float fadePoints = head ? std::max(tuning.bowFadeIn, 0.0f) / kFrontSpacing : 0.0f;
        r.frontInfo[r.frontCount] = Math::float4(static_cast<float>(written), turn, residue, fadePoints);
        ++r.frontCount;
    };

    for (const FrontSource& source : sources)
    {
        auto it = std::find_if(fronts_.begin(), fronts_.end(),
                               [&](const Front& f) { return f.hull == source.hull && f.side == source.side; });
        if (it == fronts_.end())
        {
            if (!(source.share > 0.0f))
            {
                continue;
            }
            Front front;
            front.hull = source.hull;
            front.side = source.side;
            front.random = 0x9E3779B9u * static_cast<std::uint32_t>(fronts_.size() + 1u) + static_cast<std::uint32_t>(source.side) * 7919u + 1u;
            fronts_.push_back(std::move(front));
            it = fronts_.end() - 1;
        }
        Front& front = *it;
        front.seen = true;
        if (!(source.share > 0.0f) || !(dt > 0.0f))
        {
            emit(front, nullptr); // no bow wave now: what it threw runs on
            front.seen = false;   // (emitted; and forgotten below once its last point is gone)
            continue;
        }
        front.sinceRelease += dt;
        if (front.sinceRelease >= kFrontSpacing)
        {
            front.sinceRelease = 0.0f;
            front.wobble = 0.5f * front.wobble + (NextRandom(front.random) - 0.5f);
            const float spreadJitter = std::clamp(1.0f + tuning.bowScallop * front.wobble, 0.3f, 1.7f);
            const float throwJitter = 0.7f + 0.6f * NextRandom(front.random);
            const float strengthJitter = 0.7f + 0.6f * NextRandom(front.random);
            front.points.insert(front.points.begin(), BirthOf(source, tuning, spreadJitter, throwJitter, strengthJitter));
            if (front.points.size() > kMaxFrontPoints - 1u)
            {
                front.points.pop_back();
            }
        }
        const FrontPoint head = BirthOf(source, tuning, 1.0f, 1.0f, 1.0f);
        emit(front, &head);
    }
    for (const Front& front : fronts_)
    {
        const bool sourced = std::any_of(sources.begin(), sources.end(),
                                         [&](const FrontSource& s) { return s.hull == front.hull && s.side == front.side; });
        if (!sourced)
        {
            emit(front, nullptr); // its boat is gone: the wave it threw runs on
        }
    }
    fronts_.erase(std::remove_if(fronts_.begin(), fronts_.end(), [](const Front& f) { return !f.seen && f.points.empty(); }),
                  fronts_.end());
    std::uint32_t points = 0;
    for (const Front& front : fronts_)
    {
        points += static_cast<std::uint32_t>(front.points.size());
    }
    frontsAlive_.store(static_cast<std::uint32_t>(fronts_.size()), std::memory_order_relaxed);
    frontPoints_.store(points, std::memory_order_relaxed);
    frontsDrawn_.store(r.frontCount, std::memory_order_relaxed);
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
    create(jac_, L"Ocean.WakeJacobian");
    // The free height: what the CPU floats things on -- centimetres over metres, half precision is plenty, and the
    // readback ring carries a quarter of the bytes.
    texDesc.Format = DXGI_FORMAT_R16_FLOAT;
    create(free_, L"Ocean.WakeFreeHeight");
    // The foam: x 0..2, yz the way it was laid times it (the surface's streaks).
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    create(foam_[0], L"Ocean.WakeFoam0");
    create(foam_[1], L"Ocean.WakeFoam1");

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
    heapDesc.NumDescriptors = 16u;
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
    jacUav_ = next();
    device->CreateUnorderedAccessView(jac_.Get(), nullptr, nullptr, jacUav_);
    jacFftUav_ = next();
    device->CreateUnorderedAccessView(jac_.Get(), nullptr, &arrayDesc, jacFftUav_);
    for (std::uint32_t i = 0; i < 2u; ++i)
    {
        foamUav_[i] = next();
        device->CreateUnorderedAccessView(foam_[i].Get(), nullptr, nullptr, foamUav_[i]);
        foamSrv_[i] = next();
        device->CreateShaderResourceView(foam_[i].Get(), nullptr, foamSrv_[i]);
    }
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

// The lane beside a hull its gate covers, past the roots (m): an arm is thrown 1.5 m and runs 2 m/s, so within the gate's
// length it is well inside; another boat's arm crossing the lane further out is left alone.
constexpr float kBowGateLane = 4.0f;

std::uint32_t OceanWake::BowGates(Math::float4* out, std::uint32_t maxHulls, float fadeInSeconds) const
{
    std::uint32_t count = 0;
    for (const ocean::WakeHull& hull : hulls_)
    {
        if (count >= maxHulls)
        {
            break;
        }
        const float fl = std::sqrt(hull.forward.x * hull.forward.x + hull.forward.y * hull.forward.y);
        const float length = std::max(hull.speed, 0.0f) * fadeInSeconds;
        const Math::float2& port = hull.bowRoot[0];
        const Math::float2& starboard = hull.bowRoot[1];
        const bool rooted = port.x != 0.0f || port.y != 0.0f || starboard.x != 0.0f || starboard.y != 0.0f;
        if (!hull.id || !rooted || !(fl > 1e-4f) || !(length > 0.25f))
        {
            continue;
        }
        const float dx = port.x - starboard.x;
        const float dz = port.y - starboard.y;
        out[count * 2u] = Math::float4(0.5f * (port.x + starboard.x), 0.5f * (port.y + starboard.y), hull.forward.x / fl, hull.forward.y / fl);
        out[count * 2u + 1u] = Math::float4(length, 0.5f * std::sqrt(dx * dx + dz * dz) + kBowGateLane, 0.0f, 0.0f);
        ++count;
    }
    return count;
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
    const Tuning& tuning, const ShoreWindow* shore, bool hold, float stepInterval)
{
    if (!created_ || !packMaterial_ || !stepMaterial_ || !composeMaterial_ || !fftMaterial_)
    {
        active_ = false;
        return {};
    }

    RecordPass r{};
    r.hasShore = !hold && shore && shore->srv.ptr != 0 && shore->resource != nullptr && shore->invExtent > 0.0f;
    if (r.hasShore)
    {
        r.shore = *shore;
    }
    std::vector<FrontSource> frontSources;
    frontSources.reserve(hulls_.size() * 2u);
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
        for (int side = 0; side < 2 && hull.id; ++side)
        {
            // Outward across the hull's way: the footprint's across axis (-fz, fx) is port, [0].
            const float sign = side == 0 ? 1.0f : -1.0f;
            FrontSource source;
            source.hull = hull.id;
            source.side = side;
            source.root = hull.bowRoot[side];
            source.outward = Math::float2(-hull.forward.y / fl * sign, hull.forward.x / fl * sign);
            source.speed = std::max(hull.speed, 0.0f);
            // The bow wave's share: the bow in the water (the hull's), and the hull's speed -- none under kFrontSpeedOn, so
            // a boat at rest or idling keeps no front.
            source.share = std::clamp(hull.bowShare, 0.0f, 1.0f) *
                           std::clamp((source.speed - kFrontSpeedOn) / std::max(tuning.bowSpeedFull - kFrontSpeedOn, 0.1f), 0.0f, 1.0f);
            source.length = hull.length;
            frontSources.push_back(source);
        }
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
        previousTime_ = timeSeconds;
        stepTime_ = -1.0f;
        return {};
    }

    // ---- the step, at most every stepInterval (ocean::g_wakeStepHz) ----
    // A frame closer than that to the last step records nothing: the field, its foam, its window and the time it stands at
    // are kept, and the surface carries its height on by the vertical velocity (SecondsSinceStep). A new or cleared field
    // steps at once. A frame that re-renders the shore maps cannot read them: it hands the field over as it stands -- no
    // step, its time kept for the next one (a cleared field on such a frame steps by no time, as it always did).
    const bool fresh = needsClear_ || !everSimulated_ || previousTime_ < 0.0f;
    const bool due = timeSeconds < previousTime_ || timeSeconds - previousTime_ >= stepInterval;
    if (!fresh && (hold || !due))
    {
        return {};
    }
    const float elapsed = !fresh && !hold ? std::clamp(timeSeconds - previousTime_, 0.0f, kMaxStep) : 0.0f;
    previousTime_ = timeSeconds;
    stepTime_ = timeSeconds;

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
    // White water only from hulls moving as boats move: one placed, put back by Stop or teleported changes the head under
    // it in a step, and would leave a patch of foam where it stood.
    r.wash = elapsed > 0.0f && !r.clear && lastHullPositions_.size() == r.hullCount;
    // The bow waves' fronts are kept over a hitch: a frame that took long moves the hull further than this step's
    // (capped) time allows, which is no reason to drop fifteen seconds of both arms -- only a hull that is somewhere
    // else (placed, put back by Stop) is, or a cleared field.
    bool keepFronts = !r.clear && lastHullPositions_.size() == r.hullCount;
    for (std::uint32_t i = 0; i < r.hullCount && i < lastHullPositions_.size(); ++i)
    {
        const float dx = r.hulls[i].posX - lastHullPositions_[i].x;
        const float dz = r.hulls[i].posZ - lastHullPositions_[i].y;
        r.wash = r.wash && dx * dx + dz * dz <= (kWashJumpSpeed * elapsed) * (kWashJumpSpeed * elapsed);
        keepFronts = keepFronts && dx * dx + dz * dz <= kFrontJump * kFrontJump;
    }
    r.frontsLay = keepFronts && elapsed > 0.0f;
    lastHullPositions_.resize(r.hullCount);
    for (std::uint32_t i = 0; i < r.hullCount; ++i)
    {
        lastHullPositions_[i] = Math::float2(r.hulls[i].posX, r.hulls[i].posZ);
    }
    foamNext_ ^= 1u;
    r.foamNext = foamNext_;
    // The bow waves' fronts, on the field's clock.
    StepFronts(elapsed, keepFronts, frontSources, tuning, r);

    // ---- declarations, from the same decisions ----
    ctx.NextPoint();
    r.uavPoint = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(work_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(field_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(spec_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(free_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(jac_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(foam_[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(foam_[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (r.hasShore)
    {
        ctx.Use(r.shore.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ctx.NextPoint();
    ctx.Use(field_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.Use(foam_[r.foamNext].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

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
            cb.foamFade = rec.tuning.foamFade;
            cb.foamFadeDense = rec.tuning.foamFadeDense;
            cb.foamDiffusion = std::min(rec.tuning.foamSpread * rec.dt / (rec.texel * rec.texel), kFoamDiffusionCap);
            cb.washStrength = rec.wash ? rec.tuning.washFoam : 0.0f;
            cb.washThreshold = kWashThreshold;
            cb.washBow = rec.tuning.washBow;
            cb.crestStrength = rec.tuning.crestFoam;
            cb.crestCoverage = rec.tuning.crestCoverage;
            static_assert(sizeof(HullGpu) == 12 * sizeof(float), "three float4 a hull");
            std::memcpy(cb.hulls, rec.hulls, sizeof(HullGpu) * rec.hullCount);
            cb.frontCount = rec.frontsLay ? rec.frontCount : 0u;
            static_assert(sizeof(Math::float4) == 4 * sizeof(float), "a front's point is one float4");
            static_assert(sizeof(cb.frontBounds) == sizeof(rec.frontBounds) && sizeof(cb.frontInfo) == sizeof(rec.frontInfo) &&
                          sizeof(cb.frontPoints) == sizeof(rec.frontPoints), "the fronts' record mirrors WakeCB");
            std::memcpy(cb.frontBounds, rec.frontBounds, sizeof(Math::float4) * (kMaxFrontPoints / kFrontPiece) * rec.frontCount);
            std::memcpy(cb.frontInfo, rec.frontInfo, sizeof(Math::float4) * rec.frontCount);
            std::memcpy(cb.frontPoints, rec.frontPoints, sizeof(Math::float4) * kMaxFrontPoints * rec.frontCount);

            const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};
            const auto writeCb = [&cb](std::uint8_t* dst) { std::memcpy(dst, &cb, sizeof(cb)); };
            const D3D12_CPU_DESCRIPTOR_HANDLE shoreSrv = rec.hasShore ? rec.shore.srv : shoreNullSrv_;
            const auto srvs = { footprintsSrv_, shoreSrv };
            const auto uavs = { workUav_, fieldUav_, specUav_, freeUav_, jacUav_, foamUav_[rec.foamNext ^ 1u],
                                foamUav_[rec.foamNext] };
            ID3D12Resource* const foamNext = foam_[rec.foamNext].Get();

            RecordComputeDispatch(renderer, cl, packMaterial_.get(), static_cast<UINT>(sizeof(WakeCB)), writeCb,
                srvs, uavs, noSampler, kResolution, kResolution, work_.Get());
            renderer->UAVBarrier(cl, foamNext); // Compose adds to what Pack wrote

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
            renderer->UAVBarrier(cl, jac_.Get());

            fft(fieldFftUav_, field_.Get(), 1u);
            fft(specFftUav_, spec_.Get(), 1u);
            fft(jacFftUav_, jac_.Get(), 1u);

            // Compose reads and writes only its own texel of the field and of the foam: no barrier between a read and its
            // write.
            RecordComputeDispatch(renderer, cl, composeMaterial_.get(), static_cast<UINT>(sizeof(WakeCB)), writeCb,
                srvs, uavs, noSampler, kResolution, kResolution, field_.Get());

            renderer->EmitPoint(cl, rec.uavPoint + 1u); // the field and the foam to pixel-readable for the surface
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

D3D12_CPU_DESCRIPTOR_HANDLE OceanWake::GetFoamSrv() const
{
    return created_ ? foamSrv_[foamNext_] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}
