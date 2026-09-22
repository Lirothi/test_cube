#include "rendering/streaming/TextureStreamingManager.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <thread>

#include "app/camera/Camera.h"
#include "app/scene/SceneFrameData.h"
#include "core/logging/Log.h"
#include "core/math/Frustum.h"
#include "core/task/TaskSystem.h"
#include "materials/Texture2D.h"
#include "rendering/core/MemoryReport.h"
#include "rendering/core/Renderer.h"
#include "rendering/streaming/StreamingSettings.h"
#include "rendering/streaming/TextureStreaming.h"

namespace streaming {

namespace {

constexpr std::uint64_t kUnlimited = std::uint64_t(1) << 62;

// TextureInstanceView.cpp:350-370 (new metrics): squared distance from the view to the box.
float DistSqToBox(const Math::float3& v, const Math::float3& c, const Math::float3& e)
{
    const float dx = std::max(std::fabs(v.x - c.x) - e.x, 0.0f);
    const float dy = std::max(std::fabs(v.y - c.y) - e.y, 0.0f);
    const float dz = std::max(std::fabs(v.z - c.z) - e.z, 0.0f);
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

void TextureStreamingManager::Init()
{
    start_ = std::chrono::steady_clock::now();
    stage_ = 0;
    taskDone_.store(true, std::memory_order_release);
    taskLaunched_ = false;
    memoryBudget_ = 0;
    perfectResetThreshold_ = 0;
    lastSeen_.clear();
    persist_.clear();
    rows_.clear();
    stats_ = {};
}

void TextureStreamingManager::Shutdown()
{
    // The task is microseconds of work; wait it out rather than freeing its data under it.
    while (!taskDone_.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    async_.reset();
    taskLaunched_ = false;
    stage_ = 0;
}

float TextureStreamingManager::Now_() const
{
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count();
}

void TextureStreamingManager::Tick(TextureStreaming& ts, Renderer* renderer, const SceneFrameData& frame, std::uint64_t /*frameNo*/)
{
    if (g_selftest && !selfTestDone_) { selfTestDone_ = true; SelfTest(); }
    if (!renderer || !ts.Ready() || !g_enabled || g_forceMips > 0 || !frame.camera)
    {
        stage_ = 0;
        return;
    }
    const int frames = std::max(g_framesForFullUpdate, 1);
    if (stage_ == 0)
    {
        if (!taskDone_.load(std::memory_order_acquire)) { return; } // previous task still running
        if (taskLaunched_) { Apply_(ts); taskLaunched_ = false; }
        Snapshot_(ts, renderer, frame);
        if (async_->textures.empty()) { return; }
        taskDone_.store(false, std::memory_order_release);
        taskLaunched_ = true;
        AsyncData* data = async_.get();
        std::atomic<bool>* done = &taskDone_;
        TaskSystem::Get().SubmitDetach([data, done]()
        {
            DoWork_(*data);
            done->store(true, std::memory_order_release);
        });
        stage_ = 1;
        return;
    }
    // Stages 1..frames-1: UE refreshes bounds incrementally here; a few hundred boxes are rebuilt
    // whole at the next snapshot instead. The final stage applies as soon as the task is done.
    if (stage_ < frames) { ++stage_; return; }
    if (!taskDone_.load(std::memory_order_acquire)) { return; }
    Apply_(ts);
    taskLaunched_ = false;
    stage_ = 0;
}

void TextureStreamingManager::Snapshot_(TextureStreaming& ts, Renderer* renderer, const SceneFrameData& frame)
{
    if (!async_) { async_ = std::make_unique<AsyncData>(); }
    AsyncData& d = *async_;
    d.textures.clear();
    d.bounds.clear();
    d.loadRequests.clear();
    d.cancels.clear();
    d.now = Now_();
    d.screenSize = static_cast<float>(renderer->GetWidth()) * 0.5f * std::max(g_boost, 0.01f); // AsyncTextureStreaming.cpp:90
    d.viewOrigin = frame.camera->GetPosition();
    d.hiddenScale = std::clamp(g_hiddenScale, 0.0f, 1.0f);
    d.globalMipBias = std::max(g_mipBias, 0);
    d.minMipForSplit = g_minMipForSplit > 0 ? static_cast<unsigned>(g_minMipForSplit) : kMaxMips + 1u;
    d.dropMips = g_dropMips;
    d.perTextureBias = g_perTextureBias;
    d.fullyLoadUsed = g_fullyLoadUsed;
    if (dedicatedVram_ == 0) { dedicatedVram_ = render::DedicatedVideoMemoryBytes(renderer->GetDevice()); }
    if (g_poolSizeMB > 0) { d.poolBytes = static_cast<std::uint64_t>(g_poolSizeMB) << 20; }
    else if (g_poolSizeMB < 0 && dedicatedVram_ > 0) { d.poolBytes = static_cast<std::uint64_t>(static_cast<double>(dedicatedVram_) * kPoolVramFraction); }
    else { d.poolBytes = 0; }
    d.tempBytes = static_cast<std::uint64_t>(std::max(g_tempMemoryMB, 1)) << 20;
    d.marginBytes = kMemoryMarginMB << 20; // UE [TextureStreaming] MemoryMargin=5 (BaseEngine.ini)
    d.memoryBudget = memoryBudget_;
    d.perfectResetThreshold = perfectResetThreshold_;

    const std::size_t count = ts.EntryCount();
    if (persist_.size() < count) { persist_.resize(count); }
    std::vector<int> textureSlot(count, -1);
    for (std::uint32_t e = 0; e < count; ++e)
    {
        Texture2D* tex = ts.EntryTexture(e);
        if (!tex || !tex->IsStreamable()) { continue; }
        const DdsMipTable& table = tex->GetMipTable();
        if (table.mipCount == 0 || table.mipCount > kMaxMips) { continue; }
        Persist& p = persist_[e];
        if (p.gen != ts.EntryGen(e)) { p.gen = ts.EntryGen(e); p.budgetMipBias = 0; }

        StreamingTexture t;
        t.entry = e;
        t.gen = p.gen;
        t.mipCount = table.mipCount;
        t.residentMips = tex->GetResidentMips();
        const unsigned inFlight = ts.InFlightTarget(e);
        t.requestedMips = inFlight ? inFlight : t.residentMips;
        t.numNonStreamingMips = std::min(kNonStreamingMips, t.mipCount);
        for (unsigned m = 0; m <= t.mipCount; ++m) { t.bytesUpTo[m] = table.TailBytes(t.mipCount - m); }
        // StreamingTexture.cpp:193-236: the biases cap the max, never the min.
        t.budgetMipBias = d.perTextureBias ? p.budgetMipBias : 0;
        const int lodBias = t.budgetMipBias + (d.perTextureBias ? 0 : d.globalMipBias);
        t.maxAllowedMips = static_cast<unsigned>(std::clamp(static_cast<int>(t.mipCount) - lodBias,
            static_cast<int>(t.numNonStreamingMips), static_cast<int>(t.mipCount)));
        t.minAllowedMips = t.numNonStreamingMips;
        textureSlot[e] = static_cast<int>(d.textures.size());
        d.textures.push_back(t);
    }

    const Frustum frustum = Frustum::FromViewProj(frame.camera->GetViewMatrix(), frame.camera->GetProjMatrixNoJitter());
    BuildBounds(frame, frustum, d.now, lastSeen_, textureSlot, d.bounds);
    for (const BoundsEntry& b : d.bounds)
    {
        for (const BoundsEntry::Ref& r : b.refs)
        {
            StreamingTexture& t = d.textures[r.texture];
            t.hasBounds = true;
            t.isTerrain = t.isTerrain || b.terrain;
            t.texelFactorMax = std::max(t.texelFactorMax, r.texelFactor);
        }
    }
    // Forget objects not seen for a while (level switches leave dead keys behind).
    if (lastSeen_.size() > 4096)
    {
        for (auto it = lastSeen_.begin(); it != lastSeen_.end();)
        {
            if (d.now - it->second > 120.0f) { it = lastSeen_.erase(it); } else { ++it; }
        }
    }
}

// AsyncTextureStreaming.cpp DoWork: UpdateBoundSizes -> UpdatePerfectWantedMips -> UpdateBudgetedMips
// -> UpdateLoadAndCancelationRequests, one view, textures only.
void TextureStreamingManager::DoWork_(AsyncData& d)
{
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<StreamingTexture>& textures = d.textures;
    for (StreamingTexture& t : textures)
    {
        t.maxSize = 0.0f;
        t.maxSizeVisibleOnly = 0.0f;
        t.lastRenderTime = t.hasBounds ? FLT_MAX : 0.0f; // no bounds = an unknown ref, treated as just seen
    }
    // TextureInstanceView.cpp:286-449 UpdateBoundSizes + ProcessElement (TexelFactor >= 0 branch).
    for (const BoundsEntry& b : d.bounds)
    {
        const float distSq = std::max(DistSqToBox(d.viewOrigin, b.center, b.halfExtents), 1.0e-4f); // UE clamps at 1 cm^2
        const float normSize = d.screenSize / std::sqrt(distSq);
        const bool visible = b.lastSeenAge < kVisibleWindowSec;
        for (const BoundsEntry::Ref& r : b.refs)
        {
            StreamingTexture& t = textures[r.texture];
            t.maxSize = std::max(t.maxSize, r.texelFactor * normSize);
            if (visible) { t.maxSizeVisibleOnly = std::max(t.maxSizeVisibleOnly, r.texelFactor * normSize); }
            t.lastRenderTime = std::min(t.lastRenderTime, b.lastSeenAge);
        }
    }
    // AsyncTextureStreaming.cpp:139-360 UpdatePerfectWantedMips_Async.
    for (StreamingTexture& t : textures)
    {
        const float maxAllowedSize = t.MaxAllowedSize();
        t.looksLowRes = false;
        t.unknownRef = false;
        if (d.fullyLoadUsed)
        {
            if (t.lastRenderTime < 300.0f || t.forceFullyLoad) { t.maxSizeVisibleOnly = FLT_MAX; }
        }
        else if (t.minAllowedMips == t.maxAllowedMips)
        {
            t.maxSizeVisibleOnly = t.maxSize = maxAllowedSize;
        }
        else
        {
            if ((t.maxSize > 0.0f || t.maxSizeVisibleOnly > 0.0f) && t.maxSize != FLT_MAX && t.maxSizeVisibleOnly != FLT_MAX)
            {
                t.looksLowRes = std::max({ t.maxSizeVisibleOnly, t.maxSize, maxAllowedSize }) / maxAllowedSize >= kExtraBoost * 2.0f;
                t.maxSize *= kExtraBoost;
                t.maxSizeVisibleOnly *= kExtraBoost;
            }
            t.unknownRef = (t.maxSize == 0.0f && t.maxSizeVisibleOnly == 0.0f);
            if (t.unknownRef && t.lastRenderTime < 90.0f)
            {
                t.maxSize = std::max(t.maxSize, maxAllowedSize);
                if (t.lastRenderTime < 5.0f) { t.maxSizeVisibleOnly = std::max(t.maxSizeVisibleOnly, maxAllowedSize); }
            }
            if (t.forceFullyLoad) { t.maxSize = FLT_MAX; }
        }
        t.SetPerfectWantedMips(d.hiddenScale);
    }

    // AsyncTextureStreaming.cpp:569-861 UpdateBudgetedMips_Async (textures only, one pool).
    std::uint64_t memBudgeted = 0, memUsed = 0, memWanted = 0;
    for (StreamingTexture& t : textures)
    {
        memBudgeted += t.UpdateRetentionPriority();
        memUsed += t.Size(t.residentMips);
        memWanted += t.Size(t.PerfectWantedMips());
    }
    bool resetMipBias = false;
    if (d.perfectResetThreshold > memBudgeted && d.perfectResetThreshold - memBudgeted > d.tempBytes + d.marginBytes)
    {
        d.perfectResetThreshold = memBudgeted;
        resetMipBias = true;
    }
    else if (memBudgeted > d.perfectResetThreshold)
    {
        d.perfectResetThreshold = memBudgeted;
    }
    std::uint64_t memoryBudget = d.memoryBudget;
    if (d.poolBytes == 0)
    {
        memoryBudget = kUnlimited;
    }
    else
    {
        const std::uint64_t available = d.poolBytes > d.marginBytes ? d.poolBytes - d.marginBytes : 0;
        if (available < memoryBudget) { memoryBudget = available; }
        else if (available - memoryBudget > d.tempBytes + d.marginBytes) { memoryBudget = available; resetMipBias = true; }
    }
    d.memoryBudget = memoryBudget;
    if (d.perTextureBias)
    {
        for (StreamingTexture& t : textures)
        {
            if (t.budgetMipBias > 0 &&
                (resetMipBias || std::max(t.visibleWantedMips, t.hiddenWantedMips + static_cast<unsigned>(t.numMissingMips)) < t.maxAllowedMips))
            {
                t.budgetMipBias = 0;
            }
        }
    }
    const auto byRetentionDesc = [&textures](std::uint32_t a, std::uint32_t b)
    {
        return textures[a].retentionPriority > textures[b].retentionPriority;
    };
    std::vector<std::uint32_t> prioritized;
    if (memBudgeted > memoryBudget)
    {
        for (std::uint32_t i = 0; i < textures.size(); ++i)
        {
            if (textures[i].budgetedMips > textures[i].minAllowedMips) { prioritized.push_back(i); }
        }
        std::sort(prioritized.begin(), prioritized.end(), byRetentionDesc);
        // TryDropMaxResolutions (:409-467): only as far as the global bias allows.
        if (d.perTextureBias)
        {
            for (int numDropped = 0; numDropped < d.globalMipBias && memBudgeted > memoryBudget; ++numDropped)
            {
                const std::uint64_t before = memBudgeted;
                for (std::size_t p = prioritized.size(); p-- > 0 && memBudgeted > memoryBudget;)
                {
                    StreamingTexture& t = textures[prioritized[p]];
                    if (t.budgetedMips <= t.minAllowedMips) { continue; }
                    if (static_cast<int>(t.maxAllowedMips) + t.budgetMipBias - numDropped <= static_cast<int>(t.budgetedMips))
                    {
                        const int numMipsToDrop = numDropped + 1 - t.budgetMipBias;
                        memBudgeted -= t.DropMaxResolution(numMipsToDrop);
                    }
                }
                if (before == memBudgeted) { break; }
            }
        }
        // TryDropMips (:469-528): one mip at a time from the least worth keeping.
        while (memBudgeted > memoryBudget)
        {
            const std::uint64_t before = memBudgeted;
            for (std::size_t p = prioritized.size(); p-- > 0 && memBudgeted > memoryBudget;)
            {
                StreamingTexture& t = textures[prioritized[p]];
                if (t.budgetedMips <= t.minAllowedMips) { continue; }
                if (t.numMissingMips > 0) { --t.numMissingMips; continue; }
                memBudgeted -= t.DropOneMip();
            }
            if (before == memBudgeted) { break; }
        }
    }
    if (memBudgeted < memoryBudget)
    {
        // TryKeepMips (:530-567): keep resident mips the budget can still afford, no IO.
        const std::uint64_t maxDelta = memoryBudget - memBudgeted;
        prioritized.clear();
        for (std::uint32_t i = 0; i < textures.size(); ++i)
        {
            const StreamingTexture& t = textures[i];
            if (t.budgetedMips < t.residentMips && t.Size(t.budgetedMips + 1) - t.Size(t.budgetedMips) <= maxDelta)
            {
                prioritized.push_back(i);
            }
        }
        std::sort(prioritized.begin(), prioritized.end(), byRetentionDesc);
        bool changing = true;
        while (memBudgeted < memoryBudget && changing)
        {
            changing = false;
            for (std::size_t p = 0; p < prioritized.size() && memBudgeted < memoryBudget; ++p)
            {
                if (prioritized[p] == ~0u) { continue; }
                StreamingTexture& t = textures[prioritized[p]];
                const std::uint64_t taken = t.KeepOneMip();
                if (taken > 0 && memBudgeted + taken <= memoryBudget) { memBudgeted += taken; changing = true; }
                else if (taken > 0) { t.DropOneMip(); prioritized[p] = ~0u; }
                else { prioritized[p] = ~0u; }
            }
        }
    }
    if (d.dropMips > 0)
    {
        for (StreamingTexture& t : textures)
        {
            t.budgetedMips = std::min(t.budgetedMips, d.dropMips == 1 ? t.PerfectWantedMips() : t.visibleWantedMips);
        }
    }

    // AsyncTextureStreaming.cpp:908-995 UpdateLoadAndCancelationRequests_Async.
    std::int64_t outBudget = static_cast<std::int64_t>(d.tempBytes);
    std::int64_t inBudget = static_cast<std::int64_t>(d.tempBytes);
    prioritized.clear();
    for (std::uint32_t i = 0; i < textures.size(); ++i)
    {
        StreamingTexture& t = textures[i];
        const bool wasMissingTooMany = t.missingTooManyMips;
        if (t.UpdateLoadOrderPriority(d.minMipForSplit))
        {
            if (t.requestedMips == t.residentMips)
            {
                prioritized.push_back(i);
            }
            else if (t.requestedMips > std::max(t.residentMips, t.wantedMips + (wasMissingTooMany ? 0u : 1u)) ||
                     t.requestedMips < (t.missingTooManyMips ? t.wantedMips : std::min(t.residentMips, t.wantedMips)))
            {
                d.cancels.push_back(i);
            }
        }
        const std::int64_t tempUsed = static_cast<std::int64_t>(t.Size(t.requestedMips));
        if (t.requestedMips < t.residentMips) { outBudget -= tempUsed; }
        else if (t.requestedMips > t.residentMips) { inBudget -= tempUsed; outBudget -= tempUsed; }
    }
    std::sort(prioritized.begin(), prioritized.end(), [&textures](std::uint32_t a, std::uint32_t b)
    {
        return textures[a].loadOrderPriority > textures[b].loadOrderPriority;
    });
    std::vector<std::uint32_t> outs, ins;
    for (std::uint32_t i : prioritized)
    {
        const StreamingTexture& t = textures[i];
        const std::int64_t required = static_cast<std::int64_t>(t.Size(t.wantedMips));
        if (t.wantedMips < t.residentMips && (required <= outBudget || outs.empty()))
        {
            outs.push_back(i);
            outBudget -= required;
        }
        else if (t.wantedMips > t.residentMips && (required <= inBudget || ins.empty()))
        {
            ins.push_back(i);
            inBudget -= required;
        }
    }
    d.loadRequests = outs;
    d.loadRequests.insert(d.loadRequests.end(), ins.begin(), ins.end());
    d.memBudgeted = memBudgeted;
    d.memUsed = memUsed;
    d.memWanted = memWanted;
    d.calcMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void TextureStreamingManager::Apply_(TextureStreaming& ts)
{
    if (!async_) { return; }
    AsyncData& d = *async_;
    memoryBudget_ = d.memoryBudget;
    perfectResetThreshold_ = d.perfectResetThreshold;

    Stats s;
    s.poolBytes = d.poolBytes;
    s.budgetBytes = d.memoryBudget == kUnlimited ? 0 : d.memoryBudget;
    s.usedBytes = d.memUsed;
    s.budgetedBytes = d.memBudgeted;
    s.wantedBytes = d.memWanted;
    s.bounds = static_cast<unsigned>(d.bounds.size());
    s.screenSize = d.screenSize;
    s.calcMs = d.calcMs;
    s.cycles = stats_.cycles + 1;
    rows_.clear();
    rows_.reserve(d.textures.size());
    for (const StreamingTexture& t : d.textures)
    {
        if (!ts.EntryAlive(t.entry, t.gen)) { continue; }
        ++s.textures;
        if (t.hasBounds) { ++s.withBounds; }
        if (d.perTextureBias && t.entry < persist_.size()) { persist_[t.entry].budgetMipBias = t.budgetMipBias; }
        const int delta = std::clamp(static_cast<int>(t.wantedMips) - static_cast<int>(t.residentMips), -4, 4);
        ++s.deltaHistogram[static_cast<std::size_t>(delta + 4)];
        if (ts.InFlightTarget(t.entry) != 0) { ++s.inFlight; }
        Row r;
        if (const Texture2D* tex = ts.EntryTexture(t.entry)) { r.path = tex->GetSourcePath(); }
        r.mipCount = t.mipCount; r.resident = t.residentMips; r.wanted = t.wantedMips; r.budgeted = t.budgetedMips;
        r.requested = t.requestedMips; r.maxAllowed = t.maxAllowedMips;
        r.texelFactor = t.texelFactorMax; r.lastSeen = t.lastRenderTime; r.maxSize = t.maxSize;
        r.retention = t.retentionPriority; r.loadOrder = t.loadOrderPriority; r.bias = t.budgetMipBias;
        r.unknownRef = t.unknownRef; r.terrain = t.isTerrain;
        rows_.push_back(std::move(r));
    }
    for (std::uint32_t i : d.cancels)
    {
        const StreamingTexture& t = d.textures[i];
        if (ts.EntryAlive(t.entry, t.gen) && ts.CancelRequest(t.entry)) { ++s.cancels; }
    }
    for (std::uint32_t i : d.loadRequests)
    {
        const StreamingTexture& t = d.textures[i];
        if (!ts.EntryAlive(t.entry, t.gen)) { continue; }
        if (!ts.RequestResident(t.entry, t.wantedMips)) { ++s.refused; continue; }
        if (t.wantedMips > t.residentMips) { ++s.requestsIn; s.bytesInCycle += t.Size(t.wantedMips) - t.Size(t.residentMips); }
        else { ++s.requestsOut; s.bytesOutCycle += t.Size(t.residentMips) - t.Size(t.wantedMips); }
    }
    stats_ = s;
}

bool TextureStreamingManager::SelfTest()
{
    // Plan A3.6: size = 10 * (2560 * 0.5) / 20 * 0.71 = 454.4 -> wanted = ceil(1 + log2 454.4) = 10.
    StreamingTexture t;
    t.mipCount = 12;
    t.numNonStreamingMips = 7;
    t.minAllowedMips = 7;
    t.maxAllowedMips = 12;
    const float screenSize = 2560.0f * 0.5f * 1.0f;
    const float distSq = std::max(DistSqToBox(Math::float3(0, 0, 0), Math::float3(20, 0, 0), Math::float3(0, 0, 0)), 1.0e-4f);
    const float size = 10.0f * screenSize / std::sqrt(distSq) * kExtraBoost;
    const unsigned wanted = t.WantedMipsFromSize(size);
    const bool pass = wanted == 10u;
    LOG_INFO(logging::LogCategory::Render, "[texstream] self-test: size {:.1f} -> wanted {} mips (expected 10): {}",
             size, wanted, pass ? "PASS" : "FAIL");
    return pass;
}

} // namespace streaming
