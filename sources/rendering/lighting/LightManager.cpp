#include "rendering/lighting/LightManager.h"

#include <cstdint>
#include <cmath>
#include <cstring>

#include "core/math/Frustum.h"
#include "core/profiling/ProfilerScopes.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/Renderer.h"
#include "rendering/shadows/ShadowSettings.h" // plan S4: kLocalShadowMinRes
#include "rendering/shadows/ShadowAtlasPacker.h" // plan S9

#include <algorithm>

LightManager::~LightManager()
{
    Reset();
}

void LightManager::UpdateSpotLightCache()
{
    // Step A3: the total spot-light count is uncapped — every spot lights the
    // scene. Shadow casting is separately capped per frame by SelectShadowedSpots
    // (the view capacity, highest projected importance -- plan S9e grows it); spots outside that set
    // render shadowless via the shadowParams.y = -1 sentinel.
    cachedSpotLightCount_ = spotLights_.size();

    for (size_t i = 0; i < cachedSpotLightCount_; ++i)
    {
        spotLights_[i].UpdateCachedData();
    }
}

namespace
{
    // Local-light shadow plan S3: UE's CalculateShadowFadeAlpha (ShadowSetup.cpp:534-566) with its defaults --
    // r.Shadow.FadeResolution 64, r.Shadow.MinResolution 32, r.Shadow.FadeExponent 0.25 (:203-207, :306-316).
    float ShadowFadeAlpha(float unclampedResolution)
    {
        constexpr float kFadeResolution = 64.0f, kMinResolution = 32.0f, kExponent = 0.25f;
        if (unclampedResolution > kFadeResolution) { return 1.0f; }
        if (unclampedResolution <= kMinResolution) { return 0.0f; }
        const float inverseRange = 1.0f / (kFadeResolution - kMinResolution);
        const float firstFadeValue = std::pow(inverseRange, kExponent);
        const float sizeRatio = (unclampedResolution - kMinResolution) * inverseRange;
        // UE: rescaled so the step from "no fading" to the first faded value is small (less popping). Below
        // 33 texels this goes NEGATIVE, as UE's does -- kMinShadowFade is what keeps such a light shadowless.
        return (std::pow(sizeRatio, kExponent) - firstFadeValue) / (1.0f - firstFadeValue);
    }
    // UE creates a light's whole-scene shadow only while its fade is above 1/256 (ShadowSetup.cpp:4353).
    constexpr float kMinShadowFade = 1.0f / 256.0f;

    // UE r.Shadow.TexelsPerPixelSpotlight / ...Pointlight (ShadowSetup.cpp:282-292).
    constexpr float kTexelsPerPixelSpot = 2.0f * 1.27324f;
    constexpr float kTexelsPerPixelPoint = 1.27324f;
    constexpr float kMinLightDistance = 0.01f; // UE's FMath::Max(Distance, 1.0f) in cm

    // Plan S4: UE's whole-scene shadow size (ShadowSetup.cpp:4330-4347, :4448-4458) -- the wanted resolution clamped
    // to the maximum (ClampShadowRolution, :3387) and raised to r.Shadow.MinResolution. A spot's is then rounded DOWN
    // to a power of two, UE's `1 << (CeilLogTwo(x + 1) - 1)`: "resolution changes are always doubling or halving the
    // resolution, which increases filtering stability". UE also takes a 4-texel border off it; ours needs none, the
    // shader clamps its taps inside the light's square.
    float DesiredShadowResolution(float unclampedResolution, std::uint32_t maxRes)
    {
        return std::max(std::min(unclampedResolution, static_cast<float>(maxRes)),
                        static_cast<float>(render::kLocalShadowMinRes));
    }
    std::uint32_t SpotShadowResolution(float unclampedResolution, std::uint32_t maxRes)
    {
        const float desired = DesiredShadowResolution(unclampedResolution, maxRes);
        if (desired >= static_cast<float>(maxRes)) { return maxRes; }
        const std::uint32_t wanted = static_cast<std::uint32_t>(desired); // UE truncates to uint32 as well
        std::uint32_t res = render::kLocalShadowMinRes;
        while (res * 2u <= wanted) { res *= 2u; }
        return res;
    }
    // A point's cube face: the first of UE's five cube sizes not above the wanted resolution -- half the maximum ("cubemaps
    // use a lot of memory"), its half, quarter, eighth, and r.Shadow.MinResolution (GetCubeShadowDepthZIndex, :771-797).
    std::uint32_t PointShadowFaceResolution(float unclampedResolution, std::uint32_t maxRes)
    {
        const float desired = DesiredShadowResolution(unclampedResolution, maxRes);
        const std::uint32_t sizes[5] = { maxRes / 2u, maxRes / 4u, maxRes / 8u, maxRes / 16u, render::kLocalShadowMinRes };
        for (const std::uint32_t size : sizes)
        {
            if (desired >= static_cast<float>(size)) { return std::max(size, render::kLocalShadowMinRes); }
        }
        return render::kLocalShadowMinRes;
    }

    // Local-light plan S10c: the selected set's slots, STICKY. A light that stays selected keeps its slot -- VSM's views
    // are slots, so a light that changes slot re-renders every page (its new view starts empty) and hands its old view,
    // pages and all, to whichever light lands there; ascending-index slots shifted EVERY light after a newcomer or a
    // leaver. The hole a leaver leaves takes the LAST light (one light moves, never the rest), newcomers append in
    // ascending light index (the old order for a set that starts fresh). `previous` = last frame's slot order.
    std::vector<std::uint32_t> StickyShadowSlots(const std::vector<std::uint32_t>& previous, std::vector<std::uint32_t> selected)
    {
        std::sort(selected.begin(), selected.end());
        const auto chosen = [&selected](std::uint32_t light) { return std::binary_search(selected.begin(), selected.end(), light); };
        std::vector<std::uint32_t> slots;
        slots.reserve(selected.size());
        std::vector<std::uint8_t> hole;
        for (const std::uint32_t light : previous)
        {
            slots.push_back(light);
            hole.push_back(chosen(light) ? 0u : 1u);
        }
        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (!hole[i]) { continue; }
            while (!slots.empty() && hole.back()) { slots.pop_back(); hole.pop_back(); }
            if (i >= slots.size()) { break; }
            slots[i] = slots.back();
            hole[i] = 0u;
            slots.pop_back();
            hole.pop_back();
        }
        for (const std::uint32_t light : selected)
        {
            if (std::find(previous.begin(), previous.end(), light) == previous.end()) { slots.push_back(light); }
        }
        return slots;
    }
}

void LightManager::SelectShadowedSpots(const Math::float3& cameraPos, const Frustum& cameraFrustum, float screenScale,
                                       std::uint32_t maxRes, std::uint32_t maxCount)
{
    CPU_SCOPE(ProfilerScopes::kSelectShadowedSpots);

    // S3: who had a slot last frame (the hysteresis), before the reset.
    std::vector<std::uint8_t> wasShadowed(spotLights_.size(), 0u);
    for (const std::uint32_t index : shadowedSpotLightIndices_)
    {
        if (index < wasShadowed.size()) { wasShadowed[index] = 1u; }
    }

    const std::vector<std::uint32_t> previousSlots = shadowedSpotLightIndices_; // S10c: last frame's slot order

    // Reset every spot to "unshadowed"; sized to the full spot list so
    // GetSpotShadowSlot is safe for any lit index.
    spotShadowSlot_.assign(spotLights_.size(), -1);
    spotShadowFade_.assign(spotLights_.size(), 1.0f);
    spotShadowRes_.assign(spotLights_.size(), maxRes);
    spotShadowScore_.assign(spotLights_.size(), 0.0f);
    spotShadowPlace_.assign(spotLights_.size(), ShadowAtlasPlacement{});
    shadowedSpotLightIndices_.clear();
    wantedShadowedSpots_ = 0;

    const size_t candidateCount = cachedSpotLightCount_;
    if (candidateCount == 0)
    {
        return;
    }

    // Candidates = lit spots whose influence intersects the camera frustum. A spot
    // whose reach never touches the view cannot cast a visible shadow, so it never
    // consumes a scarce shadow slot even if it is physically close to the camera
    // (e.g. behind it). Among the survivors, pick the most important
    // min(count, maxCount) by projected cone-bound size and brightness.
    //
    // Influence bound: the cached world-space AABB of the finite outer cone. It
    // encloses the real cone, so this remains conservative, while rejecting many
    // false positives from the previous apex-centered sphere.
    struct Candidate { std::uint32_t index; float score; };
    std::vector<Candidate> candidates;
    candidates.reserve(candidateCount);
    for (size_t i = 0; i < candidateCount; ++i)
    {
        const SpotLight& spot = spotLights_[i];
        const SpotLightDesc& desc = spot.GetDesc();
        if (!desc.shadowsEnabled || spot.EffectiveFluxLm() <= 0.0f)
        {
            continue; // lights the scene, but opted out of casting shadows -- or lights nothing now (plan L, by day)
        }
        const AABB& influenceBounds = spot.GetConeBounds();
        if (!cameraFrustum.Intersects(influenceBounds))
        {
            continue;
        }

        // S3: UE's effective screen radius of a spot -- the sphere inscribed at the cone's end, written as UE writes it
        // (centre at R cos, radius R sin: SpotLightSceneProxy.cpp:117-135) -- and the shadow resolution that asks for.
        const float range = std::max(desc.range, 0.0f);
        const float cosOuter = spot.GetCosOuter();
        const float sinOuter = std::sqrt(std::max(1.0f - cosOuter * cosOuter, 0.0f));
        const Math::float3 sphereCentre = desc.position + spot.GetDirection() * (range * cosOuter);
        const Math::float3 toSphere = sphereCentre - cameraPos;
        const float sphereDistance = std::max(std::sqrt(toSphere.Dot(toSphere)), kMinLightDistance);
        const float unclampedResolution = screenScale * (range * sinOuter) / sphereDistance * kTexelsPerPixelSpot;
        const float fade = ShadowFadeAlpha(unclampedResolution);
        spotShadowFade_[i] = std::max(fade, 0.0f);
        spotShadowRes_[i] = SpotShadowResolution(unclampedResolution, maxRes); // S4
        if (fade <= kMinShadowFade)
        {
            continue; // UE: too small on screen to have a shadow
        }
        const float score = unclampedResolution * (wasShadowed[i] ? kShadowSlotHysteresis : 1.0f);
        spotShadowScore_[i] = score; // S9: the atlas pack shrinks the lowest-ranked first
        candidates.push_back({ static_cast<std::uint32_t>(i), score });
    }

    // Select WHICH spots are shadowed by the resolution they want (most first), up to the view capacity (S9e: UE has no
    // cap -- a frame that wants more takes the best that fit, and the capacity grows to wantedShadowedSpots_).
    wantedShadowedSpots_ = static_cast<std::uint32_t>(candidates.size());
    const size_t shadowCount = std::min<size_t>(candidates.size(), maxCount);
    std::partial_sort(candidates.begin(), candidates.begin() + shadowCount, candidates.end(),
        [](const Candidate& a, const Candidate& b)
        {
            if (a.score == b.score)
            {
                return a.index < b.index;
            }
            return a.score > b.score;
        });

    // Assign SLOTS to the selected set in a STABLE order, NOT in score order, so an unchanged shadowed set keeps the
    // same per-light slot every frame instead of reshuffling as scores drift. Plan S10c: and a CHANGED set too -- the
    // lights that stay keep their slots (StickyShadowSlots; VSM's page cache lives in the slot's view). Cross-frame
    // correctness of `shadowParams.y` is guaranteed by the per-frame ring buffering of the spot-light buffer (see
    // EnsureSpotLightBuffer), not by this ordering.
    std::vector<std::uint32_t> selected;
    selected.reserve(shadowCount);
    for (size_t s = 0; s < shadowCount; ++s) { selected.push_back(candidates[s].index); }
    shadowedSpotLightIndices_ = StickyShadowSlots(previousSlots, std::move(selected));
    for (size_t s = 0; s < shadowedSpotLightIndices_.size(); ++s)
    {
        spotShadowSlot_[shadowedSpotLightIndices_[s]] = static_cast<int>(s);
    }
}

void LightManager::SelectShadowedPoints(const Math::float3& cameraPos, const Frustum& cameraFrustum, float screenScale,
                                        std::uint32_t maxRes, std::uint32_t maxCount)
{
    CPU_SCOPE(ProfilerScopes::kSelectShadowedPoints);

    // Point-light analog of SelectShadowedSpots (see that function for the rationale
    // on frustum-cull-then-projected-size and the stable index-order slot assignment).
    // The only difference: a point light is omnidirectional, so its influence bound is
    // exactly a sphere (position, radius) rather than a cone AABB.
    std::vector<std::uint8_t> wasShadowed(pointLights_.size(), 0u); // S3 hysteresis
    for (const std::uint32_t index : shadowedPointLightIndices_)
    {
        if (index < wasShadowed.size()) { wasShadowed[index] = 1u; }
    }
    const std::vector<std::uint32_t> previousSlots = shadowedPointLightIndices_; // S10c: last frame's slot order
    pointShadowSlot_.assign(pointLights_.size(), -1);
    pointShadowFade_.assign(pointLights_.size(), 1.0f);
    pointShadowRes_.assign(pointLights_.size(), std::max(maxRes / 2u, render::kLocalShadowMinRes));
    pointShadowScore_.assign(pointLights_.size(), 0.0f);
    pointShadowPlace_.assign(pointLights_.size(), ShadowAtlasPlacement{});
    shadowedPointLightIndices_.clear();
    wantedShadowedPoints_ = 0;

    const size_t candidateCount = pointLights_.size();
    if (candidateCount == 0)
    {
        return;
    }

    struct Candidate { std::uint32_t index; float score; };
    std::vector<Candidate> candidates;
    candidates.reserve(candidateCount);
    for (size_t i = 0; i < candidateCount; ++i)
    {
        const PointLightDesc& desc = pointLights_[i].GetDesc();
        if (!desc.shadowsEnabled || desc.radius <= 0.0f || desc.luminousFluxLm <= 0.0f)
        {
            continue; // lights the scene, but never a cube-shadow caster -- or lights nothing now (plan L)
        }
        if (!cameraFrustum.Intersects(desc.position, desc.radius))
        {
            continue; // reach never touches the view -> no visible shadow
        }

        // S3: UE's effective screen radius of a point light (LocalLightSceneProxy.cpp:101-108) and its resolution.
        const Math::float3 toCenter = desc.position - cameraPos;
        const float centerDistance = std::max(std::sqrt(toCenter.Dot(toCenter)), kMinLightDistance);
        const float unclampedResolution = screenScale * desc.radius / centerDistance * kTexelsPerPixelPoint;
        const float fade = ShadowFadeAlpha(unclampedResolution);
        pointShadowFade_[i] = std::max(fade, 0.0f);
        pointShadowRes_[i] = PointShadowFaceResolution(unclampedResolution, maxRes); // S4
        if (fade <= kMinShadowFade)
        {
            continue; // UE: too small on screen to have a shadow
        }
        const float score = unclampedResolution * (wasShadowed[i] ? kShadowSlotHysteresis : 1.0f);
        pointShadowScore_[i] = score; // S9, as the spots'
        candidates.push_back({ static_cast<std::uint32_t>(i), score });
    }

    // Select WHICH points are shadowed by projected size, then assign slots in stable
    // ascending-index order (see SelectShadowedSpots for why slot order must be stable). S9e: up to the capacity.
    wantedShadowedPoints_ = static_cast<std::uint32_t>(candidates.size());
    const size_t shadowCount = std::min<size_t>(candidates.size(), maxCount);
    std::partial_sort(candidates.begin(), candidates.begin() + shadowCount, candidates.end(),
        [](const Candidate& a, const Candidate& b)
        {
            if (a.score == b.score)
            {
                return a.index < b.index;
            }
            return a.score > b.score;
        });
    // Plan S10c: sticky slots, as the spots'.
    std::vector<std::uint32_t> selected;
    selected.reserve(shadowCount);
    for (size_t s = 0; s < shadowCount; ++s) { selected.push_back(candidates[s].index); }
    shadowedPointLightIndices_ = StickyShadowSlots(previousSlots, std::move(selected));
    for (size_t s = 0; s < shadowedPointLightIndices_.size(); ++s)
    {
        pointShadowSlot_[shadowedPointLightIndices_[s]] = static_cast<int>(s);
    }
}

namespace
{
    // Plan S9: one pool of atlas pages. Packs the pool's shadowed lights at their UE sizes, reports how many pages
    // that wants (up to maxPages), then makes it fit the RESIDENT pages: EVERY light that can still shrink is halved
    // -- one step for all, as UE's VSM raises one global LOD bias when its page pool is short
    // (VirtualShadowMapCacheManager.cpp:155-187), so the lights keep the proportions their screen sizes gave them --
    // and when none can, the lowest-ranked is dropped (no shadow this frame). UE's classic path would open another
    // atlas instead; ours grow at the next residency point. `shadowed` keeps its order, so the slots it re-numbers keep
    // the stable order SelectShadowed* gave them (sticky since plan S10c).
    std::uint32_t PackShadowPool(std::vector<std::uint32_t>& shadowed, std::vector<int>& slots,
                                 std::vector<std::uint32_t>& res, const std::vector<float>& score,
                                 std::vector<LightManager::ShadowAtlasPlacement>& place, bool pointBlocks,
                                 std::uint32_t pageWidth, std::uint32_t pageHeight, std::uint32_t residentPages,
                                 std::uint32_t maxPages, std::uint32_t& shrunk, std::uint32_t& dropped)
    {
        std::vector<render::ShadowAtlasItem> items;
        std::vector<render::ShadowAtlasSlot> placed;
        const auto build = [&]()
        {
            items.clear();
            for (const std::uint32_t light : shadowed)
            {
                const std::uint32_t r = res[light];
                items.push_back(pointBlocks ? render::ShadowAtlasItem{ 3u * r, 2u * r, light }
                                            : render::ShadowAtlasItem{ r, r, light });
            }
        };
        build();
        const std::uint32_t wanted = std::max(render::PackShadowAtlas(items, pageWidth, pageHeight, maxPages, placed), 1u);

        std::vector<std::uint8_t> wasShrunk(res.size(), 0u);
        for (std::uint32_t guard = 0; guard < 4096u; ++guard)
        {
            build();
            render::PackShadowAtlas(items, pageWidth, pageHeight, residentPages, placed);
            if (std::all_of(placed.begin(), placed.end(), [](const render::ShadowAtlasSlot& s) { return s.page >= 0; }))
            {
                break;
            }
            // One step down for every light that still can.
            bool halved = false;
            for (const std::uint32_t light : shadowed)
            {
                if (res[light] / 2u < render::kLocalShadowMinRes) { continue; }
                res[light] /= 2u;
                halved = true;
                if (!wasShrunk[light]) { wasShrunk[light] = 1u; ++shrunk; }
            }
            if (halved) { continue; }
            // Nothing left to halve: drop the lowest-ranked light.
            std::uint32_t lowest = 0;
            for (std::uint32_t k = 1; k < shadowed.size(); ++k)
            {
                if (score[shadowed[k]] <= score[shadowed[lowest]]) { lowest = k; }
            }
            slots[shadowed[lowest]] = -1;
            shadowed.erase(shadowed.begin() + lowest);
            ++dropped;
        }
        // A light without a place (only if the guard ran out) loses its slot rather than render nowhere.
        std::vector<std::uint32_t> kept;
        kept.reserve(shadowed.size());
        for (std::uint32_t s = 0; s < shadowed.size(); ++s)
        {
            const std::uint32_t light = shadowed[s];
            if (s < placed.size() && placed[s].page >= 0)
            {
                place[light] = LightManager::ShadowAtlasPlacement{ placed[s].page, placed[s].x, placed[s].y };
                kept.push_back(light);
            }
            else
            {
                slots[light] = -1;
                ++dropped;
            }
        }
        shadowed.swap(kept);
        for (std::uint32_t s = 0; s < shadowed.size(); ++s) { slots[shadowed[s]] = static_cast<int>(s); }
        return wanted;
    }
}

void LightManager::ClearShadowDirect()
{
    spotShadowDirect_.assign(spotLights_.size(), ShadowAtlasPlacement{});
    pointShadowDirect_.assign(pointLights_.size(), ShadowAtlasPlacement{});
}

void LightManager::SetSpotShadowDirect(size_t lightIndex, ShadowAtlasPlacement place)
{
    if (lightIndex < spotShadowDirect_.size()) { spotShadowDirect_[lightIndex] = place; }
}

void LightManager::SetPointShadowDirect(size_t lightIndex, ShadowAtlasPlacement place)
{
    if (lightIndex < pointShadowDirect_.size()) { pointShadowDirect_[lightIndex] = place; }
}

void LightManager::PackShadowAtlases(const ShadowAtlasPages& pages)
{
    atlasShrunk_ = 0u;
    atlasDropped_ = 0u;
    // Spots: a res x res square in a spot page. Points: the six faces as one 3f x 2f block in a point page, which is
    // exactly one block of the largest face (3 * pointFaceMax by 2 * pointFaceMax).
    wantedSpotPages_ = PackShadowPool(shadowedSpotLightIndices_, spotShadowSlot_, spotShadowRes_, spotShadowScore_,
                                      spotShadowPlace_, false, pages.spotPageSize, pages.spotPageSize,
                                      pages.spotPages, pages.maxPages, atlasShrunk_, atlasDropped_);
    wantedPointPages_ = PackShadowPool(shadowedPointLightIndices_, pointShadowSlot_, pointShadowRes_, pointShadowScore_,
                                       pointShadowPlace_, true, 3u * pages.pointFaceMax, 2u * pages.pointFaceMax,
                                       pages.pointPages, pages.maxPages, atlasShrunk_, atlasDropped_);
}

void LightManager::ReleasePointLightBuffer(Renderer* renderer)
{
    if (pointLightBuffer_)
    {
        (void)renderer; // the wrapper unregisters itself
        pointLightBuffer_->Unmap(0, nullptr);
        pointLightBuffer_.Reset();
    }
    pointLightBufferCPU_ = nullptr;
    pointLightCapacity_ = 0;
    pointLightSrvHeap_.Reset();
    pointLightSrvHandles_.fill({});
}

void LightManager::ReleaseSpotLightBuffer(Renderer* renderer)
{
    if (spotLightBuffer_)
    {
        (void)renderer; // the wrapper unregisters itself
        spotLightBuffer_->Unmap(0, nullptr);
        spotLightBuffer_.Reset();
    }
    spotLightBufferCPU_ = nullptr;
    spotLightCapacity_ = 0;
    spotLightSrvHeap_.Reset();
    spotLightSrvHandles_.fill({});
}

bool LightManager::EnsurePointLightBuffer(Renderer* renderer, size_t requiredLights)
{
    if (!renderer || !renderer->GetDevice() || requiredLights == 0)
    {
        return false;
    }

    if (pointLightBuffer_ && pointLightBufferCPU_ && pointLightSrvHandles_[0].ptr != 0 && pointLightCapacity_ >= requiredLights)
    {
        return true;
    }

    ReleasePointLightBuffer(renderer);
    const size_t newCapacity = std::max<size_t>(requiredLights, 1);
    // Ring buffer: render::kFrameCount contiguous regions of newCapacity elements.
    const size_t totalElements = newCapacity * render::kFrameCount;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(totalElements * sizeof(PointLightGpu));
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    HRESULT hr = gpu::CreateCommittedResource(renderer->GetDevice(),
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        MemTag::Lights, L"LightManager.PointLightBuffer",
        IID_PPV_ARGS(buffer.GetAddressOf()));
    if (FAILED(hr) || !buffer)
    {
        return false;
    }

    D3D12_RANGE range{ 0, 0 };
    void* mapped = nullptr;
    hr = buffer->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped)
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = render::kFrameCount; // one SRV per ring region
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    hr = gpu::CreateDescriptorHeap(renderer->GetDevice(), &heapDesc, MemTag::Lights, L"LightManager.PointLightSrvHeap", IID_PPV_ARGS(srvHeap.GetAddressOf()));
    if (FAILED(hr) || !srvHeap)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE srvBase = srvHeap->GetCPUDescriptorHandleForHeapStart();
    if (srvBase.ptr == 0)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }
    const UINT srvIncr = renderer->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = static_cast<UINT64>(f) * newCapacity; // region f
        srvDesc.Buffer.NumElements = static_cast<UINT>(newCapacity);
        srvDesc.Buffer.StructureByteStride = sizeof(PointLightGpu);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        D3D12_CPU_DESCRIPTOR_HANDLE h{ srvBase.ptr + static_cast<SIZE_T>(f) * srvIncr };
        renderer->GetDevice()->CreateShaderResourceView(buffer.Get(), &srvDesc, h);
        pointLightSrvHandles_[f] = h;
    }

    // Upload-heap buffer: never transitioned, so it rests where it is created.
    pointLightBuffer_.Attach(renderer->Declarations(), buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, L"LightManager.PointLightBuffer");
    pointLightCapacity_ = newCapacity;
    pointLightBufferCPU_ = static_cast<PointLightGpu*>(mapped);
    pointLightSrvHeap_ = srvHeap;
    return true;
}

bool LightManager::EnsureSpotLightBuffer(Renderer* renderer, size_t requiredLights)
{
    if (!renderer || !renderer->GetDevice() || requiredLights == 0)
    {
        return false;
    }

    if (spotLightBuffer_ && spotLightBufferCPU_ && spotLightSrvHandles_[0].ptr != 0 && spotLightCapacity_ >= requiredLights)
    {
        return true;
    }

    ReleaseSpotLightBuffer(renderer);
    const size_t newCapacity = std::max<size_t>(requiredLights, 1);
    // Ring buffer: render::kFrameCount contiguous regions of newCapacity elements.
    const size_t totalElements = newCapacity * render::kFrameCount;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(totalElements * sizeof(SpotLightGpu));
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    HRESULT hr = gpu::CreateCommittedResource(renderer->GetDevice(),
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        MemTag::Lights, L"LightManager.SpotLightBuffer",
        IID_PPV_ARGS(buffer.GetAddressOf()));
    if (FAILED(hr) || !buffer)
    {
        return false;
    }

    D3D12_RANGE range{ 0, 0 };
    void* mapped = nullptr;
    hr = buffer->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped)
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = render::kFrameCount; // one SRV per ring region
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    hr = gpu::CreateDescriptorHeap(renderer->GetDevice(), &heapDesc, MemTag::Lights, L"LightManager.SpotLightSrvHeap", IID_PPV_ARGS(srvHeap.GetAddressOf()));
    if (FAILED(hr) || !srvHeap)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE srvBase = srvHeap->GetCPUDescriptorHandleForHeapStart();
    if (srvBase.ptr == 0)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }
    const UINT srvIncr = renderer->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = static_cast<UINT64>(f) * newCapacity; // region f
        srvDesc.Buffer.NumElements = static_cast<UINT>(newCapacity);
        srvDesc.Buffer.StructureByteStride = sizeof(SpotLightGpu);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        D3D12_CPU_DESCRIPTOR_HANDLE h{ srvBase.ptr + static_cast<SIZE_T>(f) * srvIncr };
        renderer->GetDevice()->CreateShaderResourceView(buffer.Get(), &srvDesc, h);
        spotLightSrvHandles_[f] = h;
    }

    // Upload-heap buffer: never transitioned, so it rests where it is created.
    spotLightBuffer_.Attach(renderer->Declarations(), buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, L"LightManager.SpotLightBuffer");
    spotLightCapacity_ = newCapacity;
    spotLightBufferCPU_ = static_cast<SpotLightGpu*>(mapped);
    spotLightSrvHeap_ = srvHeap;
    return true;
}

void LightManager::Reset()
{
    // Clear the CPU-side light lists, but DELIBERATELY retain the GPU light
    // buffers + their SRV descriptors. Reset() runs on every level load/switch
    // (JsonLevel::Load) and unload (Scene::Clear). Releasing the spot/point
    // light buffers here dangles their SRV descriptor: the spot-lights compute
    // dispatch (Pass_SpotLights) reads the spot-light structured buffer via a
    // staged SRV (table index 6), and freeing + recreating that buffer across a
    // level change left the descriptor pointing at a destroyed resource
    // (GPU-based validation: "Invalid resource pointed to by descriptor ...
    // resource has been destroyed", spotlight_cs.hlsl(120)), which manifested as
    // an intermittent DXGI_ERROR_DEVICE_HUNG on the spot-lights dispatch during
    // scene-lifecycle churn. The buffers are fully rewritten from the CPU every
    // frame (EnsureSpotLightBuffer keeps them when capacity suffices; the pass
    // repopulates the contents), so retaining them across a level change is
    // correct and avoids the use-after-free. The buffers are still released
    // normally by the destructor (member ComPtr teardown).
    pointLights_.clear();
    spotLights_.clear();
    cachedSpotLightCount_ = 0;
    spotShadowSlot_.clear();
    shadowedSpotLightIndices_.clear();
    pointShadowSlot_.clear();
    shadowedPointLightIndices_.clear();
    spotPose_.clear(); // S10e: a new level's lights are first seen standing
    pointPose_.clear();
    spotMovedFrame_.clear();
    pointMovedFrame_.clear();
    mobileLights_ = 0;
}

void LightManager::UpdateLightMobility(std::uint64_t frame, std::uint32_t activeFrames)
{
    mobilityFrame_ = frame;
    mobilityActiveFrames_ = std::max(activeFrames, 1u);
    const auto track = [frame](std::vector<ShadowPose>& poses, std::vector<std::uint64_t>& moved, size_t count,
                               const auto& poseOf)
    {
        if (poses.size() != count) // lights added or removed: those first seen stand, the rest keep their history
        {
            const size_t old = poses.size();
            poses.resize(count);
            moved.resize(count, kNeverMoved);
            for (size_t i = old; i < count; ++i) { poses[i] = poseOf(i); }
        }
        for (size_t i = 0; i < count; ++i)
        {
            const ShadowPose now = poseOf(i);
            if (std::memcmp(&now, &poses[i], sizeof(now)) != 0)
            {
                poses[i] = now;
                moved[i] = frame;
            }
        }
    };
    track(spotPose_, spotMovedFrame_, spotLights_.size(), [this](size_t i)
    {
        const SpotLight& light = spotLights_[i];
        const auto& d = light.GetDesc();
        const Math::float3 dir = light.GetDirection();
        return ShadowPose{ { d.position.x, d.position.y, d.position.z, dir.x, dir.y, dir.z, d.range, light.GetCosOuter(),
                             d.nearPlane } };
    });
    track(pointPose_, pointMovedFrame_, pointLights_.size(), [this](size_t i)
    {
        const auto& d = pointLights_[i].GetDesc();
        return ShadowPose{ { d.position.x, d.position.y, d.position.z, d.radius } };
    });
    mobileLights_ = 0;
    for (size_t i = 0; i < spotMovedFrame_.size(); ++i) { mobileLights_ += GetSpotMobility(i) > 0.0f ? 1u : 0u; }
    for (size_t i = 0; i < pointMovedFrame_.size(); ++i) { mobileLights_ += GetPointMobility(i) > 0.0f ? 1u : 0u; }
}

float LightManager::Mobility(const std::vector<std::uint64_t>& moved, size_t index) const
{
    // UE: 1 on the frame it moved, 1 - k / N k frames later, 0 from N on.
    if (index >= moved.size() || moved[index] == kNeverMoved || mobilityFrame_ < moved[index]) { return 0.0f; }
    const float age = static_cast<float>(mobilityFrame_ - moved[index]);
    return 1.0f - std::clamp(age / static_cast<float>(mobilityActiveFrames_), 0.0f, 1.0f);
}

