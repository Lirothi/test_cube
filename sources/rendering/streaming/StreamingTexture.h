#pragma once

#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>

namespace streaming {

inline constexpr unsigned kMaxMips = 16;

// One streamable texture as the manager's async task sees it -- UE FStreamingRenderAsset, the
// fields A3 uses. Mips count from the SMALLEST: `Size(m)` = bytes of the m smallest mips.
struct StreamingTexture
{
    std::uint32_t entry = 0;       // TextureStreaming registry slot
    std::uint32_t gen = 0;
    unsigned mipCount = 0;
    unsigned residentMips = 0;
    unsigned requestedMips = 0;    // in-flight target, == residentMips when idle
    unsigned wantedMips = 0;
    unsigned visibleWantedMips = 0;
    unsigned hiddenWantedMips = 0;
    unsigned budgetedMips = 0;
    unsigned maxAllowedMips = 0;
    unsigned minAllowedMips = 0;
    unsigned numNonStreamingMips = 0;
    int budgetMipBias = 0;
    int numMissingMips = 0;
    float lastRenderTime = FLT_MAX; // seconds since last seen (UE: age, not a timestamp)
    float maxSize = 0.0f;
    float maxSizeVisibleOnly = 0.0f;
    float texelFactorMax = 0.0f;    // readout only
    bool forceFullyLoad = false;
    bool forceFullyLoadHeuristic = false;
    bool isTerrain = false;
    bool looksLowRes = false;
    bool unknownRef = false;
    bool hasBounds = false;
    bool missingTooManyMips = false;
    int retentionPriority = 0;
    int loadOrderPriority = 0;
    std::array<std::uint64_t, kMaxMips + 1> bytesUpTo{}; // bytesUpTo[m] = bytes of the m smallest mips

    std::uint64_t Size(unsigned mips) const { return bytesUpTo[std::min(mips, mipCount)]; }
    unsigned PerfectWantedMips() const { return std::max(visibleWantedMips, hiddenWantedMips); }
    // StreamingTexture.cpp GetMaxAllowedSize: the screen size the max allowed mip serves.
    float MaxAllowedSize() const { return maxAllowedMips ? static_cast<float>(1u << (maxAllowedMips - 1u)) : 1.0f; }

    // StreamingTexture.cpp:306-333 GetWantedMipsFromSize (texture branch).
    unsigned WantedMipsFromSize(float size) const
    {
        if (size == FLT_MAX) { return maxAllowedMips; }
        const float wantedF = 1.0f + std::log2(std::max(1.0f, size));
        const int wanted = static_cast<int>(std::ceil(wantedF));
        return static_cast<unsigned>(std::clamp(wanted, static_cast<int>(minAllowedMips), static_cast<int>(maxAllowedMips)));
    }

    // StreamingTexture.cpp:336-377 SetPerfectWantedMips_Async.
    void SetPerfectWantedMips(float hiddenScale)
    {
        forceFullyLoadHeuristic = (maxSize == FLT_MAX || maxSizeVisibleOnly == FLT_MAX);
        visibleWantedMips = WantedMipsFromSize(maxSizeVisibleOnly);
        if (isTerrain || forceFullyLoadHeuristic || looksLowRes)
        {
            hiddenWantedMips = WantedMipsFromSize(maxSize);
            numMissingMips = 0;
        }
        else
        {
            hiddenWantedMips = WantedMipsFromSize(maxSize * hiddenScale);
            numMissingMips = std::max(static_cast<int>(WantedMipsFromSize(maxSize)) -
                                      static_cast<int>(std::max(visibleWantedMips, hiddenWantedMips)), 0);
        }
    }

    // StreamingTexture.cpp:383-412 UpdateRetentionPriority_Async; returns the budgeted bytes.
    std::uint64_t UpdateRetentionPriority()
    {
        budgetedMips = PerfectWantedMips();
        retentionPriority = 0;
        const bool isHuge = Size(budgetedMips) >= 8ull * 1024 * 1024;
        const bool shouldKeep = isTerrain || forceFullyLoadHeuristic || (looksLowRes && !isHuge);
        const bool isSmall = Size(budgetedMips) <= 200ull * 1024;
        const bool isVisible = visibleWantedMips >= hiddenWantedMips;
        if (shouldKeep) { retentionPriority += 2048; }
        if (isVisible) { retentionPriority += 1024; }
        if (!isHuge) { retentionPriority += 512; }
        if (isSmall) { retentionPriority += 256; }
        if (!isVisible) { retentionPriority += std::clamp(255 - static_cast<int>(lastRenderTime), 1, 255); }
        return Size(budgetedMips);
    }

    // StreamingTexture.cpp:493-540 UpdateLoadOrderPriority_Async; true when a request is needed.
    bool UpdateLoadOrderPriority(unsigned minMipForSplit)
    {
        loadOrderPriority = 0;
        missingTooManyMips = false;
        if (residentMips < visibleWantedMips && visibleWantedMips < budgetedMips &&
            budgetedMips >= minMipForSplit && !isTerrain)
        {
            wantedMips = visibleWantedMips;
        }
        else
        {
            wantedMips = budgetedMips;
        }
        if (wantedMips == requestedMips) { return false; }
        const bool isVisible = residentMips < visibleWantedMips;
        const bool mustLoadFirst = forceFullyLoadHeuristic || isTerrain;
        if (wantedMips > requestedMips)
        {
            const bool mipIsImportant = static_cast<int>(wantedMips) - static_cast<int>(residentMips) > (looksLowRes ? 1 : 2);
            missingTooManyMips = isVisible && mipIsImportant;
            if (isVisible) { loadOrderPriority += 1024; }
            if (mustLoadFirst) { loadOrderPriority += 512; }
            if (mipIsImportant) { loadOrderPriority += 256; }
            if (!isVisible) { loadOrderPriority += std::clamp(255 - static_cast<int>(lastRenderTime), 1, 255); }
        }
        else
        {
            if (!mustLoadFirst) { loadOrderPriority += 1024; }
            if (!isVisible) { loadOrderPriority += 512; }
        }
        return true;
    }

    // DropOneMip_Async / KeepOneMip_Async / DropMaxResolution_Async: bytes freed / taken.
    std::uint64_t DropOneMip()
    {
        if (budgetedMips <= minAllowedMips) { return 0; }
        --budgetedMips;
        return Size(budgetedMips + 1) - Size(budgetedMips);
    }
    std::uint64_t KeepOneMip()
    {
        // StreamingTexture.cpp:464-475: never above maxAllowedMips, or a lowered bias could not
        // stream out mips that are already resident while the pool has room.
        if (budgetedMips >= std::min(residentMips, maxAllowedMips)) { return 0; }
        ++budgetedMips;
        return Size(budgetedMips) - Size(budgetedMips - 1);
    }
    std::uint64_t DropMaxResolution(int numMips)
    {
        numMips = std::min(numMips, static_cast<int>(maxAllowedMips) - static_cast<int>(minAllowedMips));
        if (numMips <= 0) { return 0; }
        const std::uint64_t before = Size(budgetedMips);
        maxAllowedMips -= static_cast<unsigned>(numMips);
        budgetMipBias += numMips;
        budgetedMips = std::min(budgetedMips, maxAllowedMips);
        return before - Size(budgetedMips);
    }
};

} // namespace streaming
