#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/Math.h"
#include "rendering/streaming/StreamingBounds.h"
#include "rendering/streaming/StreamingTexture.h"

class Renderer;
struct SceneFrameData;

namespace streaming {

class TextureStreaming;

// Plan A3: UE FRenderAssetStreamingManager + FRenderAssetStreamingMipCalcTask. Every
// framesForFullUpdate frames: snapshot (camera, bounds, textures) -> detached task (sizes, wanted
// mips, budget, load/cancel lists) -> apply (requests to TextureStreaming). Main thread except
// DoWork_, which only touches its own AsyncData.
class TextureStreamingManager
{
public:
    struct Stats
    {
        std::uint64_t poolBytes = 0;      // 0 = unlimited
        std::uint64_t budgetBytes = 0;    // effective budget after the pool/temp/margin rules
        std::uint64_t usedBytes = 0;      // resident mips of every registered texture
        std::uint64_t budgetedBytes = 0;  // what the last cycle decided to hold
        std::uint64_t wantedBytes = 0;    // perfect wanted, before the budget
        std::uint64_t bytesInCycle = 0;   // stream-in bytes requested by the last cycle
        std::uint64_t bytesOutCycle = 0;  // stream-out bytes requested by the last cycle
        unsigned textures = 0, withBounds = 0, bounds = 0;
        unsigned requestsIn = 0, requestsOut = 0, cancels = 0, refused = 0, inFlight = 0;
        unsigned cycles = 0;
        float screenSize = 0.0f;
        double calcMs = 0.0;
        std::array<unsigned, 9> deltaHistogram{}; // wanted - resident, bucket i = delta i-4 (clamped to +-4)
    };
    struct Row
    {
        std::wstring path;
        unsigned mipCount = 0, resident = 0, wanted = 0, budgeted = 0, requested = 0, maxAllowed = 0;
        unsigned visibleWanted = 0, hiddenWanted = 0; // "perfect" wanted from distance, before the budget
        float texelFactor = 0.0f, lastSeen = 0.0f, maxSize = 0.0f;
        int retention = 0, loadOrder = 0, bias = 0;
        bool unknownRef = false, terrain = false;
    };

    void Init();
    void Shutdown(); // joins the task if one is in flight
    void Tick(TextureStreaming& ts, Renderer* renderer, const SceneFrameData& frame, std::uint64_t frameNo);
    const Stats& GetStats() const { return stats_; }
    const std::vector<Row>& Rows() const { return rows_; }
    // Plan A3.6: 2048^2 (12 mips), texel factor 10 m, 20 m away, 2560 wide, boost 1 -> 10 mips.
    static bool SelfTest();

private:
    struct AsyncData
    {
        std::vector<StreamingTexture> textures;
        std::vector<BoundsEntry> bounds;
        float screenSize = 0.0f;
        Math::float3 viewOrigin{};
        float now = 0.0f;
        float hiddenScale = 0.5f;
        int globalMipBias = 0;
        unsigned minMipForSplit = 10;
        int dropMips = 0;
        bool perTextureBias = true;
        bool fullyLoadUsed = false;
        std::uint64_t poolBytes = 0;
        std::uint64_t tempBytes = 0;
        std::uint64_t marginBytes = 0;
        std::uint64_t memoryBudget = 0;           // in/out: persists across cycles
        std::uint64_t perfectResetThreshold = 0;  // in/out
        std::vector<std::uint32_t> loadRequests;  // texture indices, priority order, outs first
        std::vector<std::uint32_t> cancels;
        std::uint64_t memBudgeted = 0, memUsed = 0, memWanted = 0;
        double calcMs = 0.0;
    };
    struct Persist { std::uint32_t gen = 0; int budgetMipBias = 0; };

    void Snapshot_(TextureStreaming& ts, Renderer* renderer, const SceneFrameData& frame);
    static void DoWork_(AsyncData& d);
    void Apply_(TextureStreaming& ts);
    float Now_() const;

    std::unique_ptr<AsyncData> async_;
    std::atomic<bool> taskDone_{ true };
    bool taskLaunched_ = false;
    int stage_ = 0;
    bool selfTestDone_ = false;
    std::chrono::steady_clock::time_point start_{};
    std::unordered_map<const void*, float> lastSeen_;
    std::vector<Persist> persist_;
    std::uint64_t memoryBudget_ = 0;
    std::uint64_t perfectResetThreshold_ = 0;
    std::uint64_t dedicatedVram_ = 0;
    Stats stats_;
    std::vector<Row> rows_;
};

} // namespace streaming
