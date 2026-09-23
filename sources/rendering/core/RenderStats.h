#pragma once
#include <atomic>
#include <cstdint>

namespace render
{
// Per-frame draw-call + primitive counters for the developer overlay.
//
// TWO SOURCES, because neither alone is the truth:
//   * CPU: every draw API call the engine records -- the Mesh helpers and the direct
//     DrawInstanced / DrawIndexedInstanced sites -- plus each ExecuteIndirect as ONE call with
//     the number of draws it MAY issue. The CPU cannot know how many of those the GPU cull
//     actually emitted, so it says "up to".
//   * GPU: a PIPELINE_STATISTICS query around every direct command list of the frame
//     (Renderer::BeginListStats / EndListStats), read back when the frame slot comes round
//     again. This is what the GPU actually processed -- the indirect G-buffer, the VSM page
//     render and every bundle included -- and the only honest triangle count once most of the
//     scene is GPU-driven. It used to say "Draw calls: 2, Primitives: 0.559M" on wind_test:
//     the two CPU draws left after the indirect path took the palms, and their triangles only.
//
// NextFrame() snapshots the completed frame's CPU totals and resets the accumulators; the GPU
// block is published by the Renderer at BeginFrame, kFrameCount frames behind. The UI reads
// the last completed values, so reads are never torn against in-progress recording.
struct RenderStats
{
    std::atomic<uint32_t> drawCalls{0};
    std::atomic<uint64_t> primitives{0};
    std::atomic<uint32_t> indirectCalls{0};
    std::atomic<uint64_t> indirectMaxDraws{0};
    uint32_t lastDrawCalls = 0;
    uint64_t lastPrimitives = 0;
    uint32_t lastIndirectCalls = 0;
    uint64_t lastIndirectMaxDraws = 0;

    void AddDraw(uint32_t indexCount, uint32_t instanceCount)
    {
        drawCalls.fetch_add(1u, std::memory_order_relaxed);
        primitives.fetch_add(static_cast<uint64_t>(indexCount / 3u) * instanceCount, std::memory_order_relaxed);
    }

    // One ExecuteIndirect: one API call that issues up to `maxCommands` draws on the GPU.
    void AddIndirect(uint32_t maxCommands)
    {
        indirectCalls.fetch_add(1u, std::memory_order_relaxed);
        indirectMaxDraws.fetch_add(maxCommands, std::memory_order_relaxed);
    }

    void NextFrame()
    {
        lastDrawCalls = drawCalls.exchange(0u, std::memory_order_relaxed);
        lastPrimitives = primitives.exchange(0u, std::memory_order_relaxed);
        lastIndirectCalls = indirectCalls.exchange(0u, std::memory_order_relaxed);
        lastIndirectMaxDraws = indirectMaxDraws.exchange(0u, std::memory_order_relaxed);
    }

    // --- GPU truth (published by Renderer::PublishListStats) --------------------------------
    static constexpr int kTopLists = 8;
    struct ListShare
    {
        char name[48] = {};
        uint64_t primitives = 0; // IAPrimitives of every list carrying this name, summed
    };
    bool gpuValid = false;
    uint64_t gpuPrimitives = 0;     // IAPrimitives: triangles the input assembler fed in
    uint64_t gpuRasterized = 0;     // CPrimitives: what left the clipper for the rasterizer
    uint64_t gpuPixelShaded = 0;    // PSInvocations: pixel-shader invocations (overdraw)
    uint32_t gpuLists = 0;          // direct lists measured
    uint32_t gpuListsUnmeasured = 0; // lists past the query budget -- nonzero means the sum is SHORT
    int gpuTopCount = 0;
    ListShare gpuTop[kTopLists];
};

inline RenderStats g_renderStats;

// The GPU half above costs one PIPELINE_STATISTICS query per direct list, ~9 us each (measured
// +0.36 ms on a 38-list wind_test frame). So by default it is recorded ON DEMAND -- only while
// something asks for it every frame (the Frame tab of Developer Controls calls
// RequestPipelineStats while it is drawn), and then on one frame in 32; a closed panel costs
// nothing. `--set=stats.pipeline:0|1|2` = off | every frame | on demand (default).
inline int g_pipelineStatsMode = 2;
inline std::atomic<bool> g_pipelineStatsRequested{ false };
inline void RequestPipelineStats() { g_pipelineStatsRequested.store(true, std::memory_order_relaxed); }
} // namespace render
