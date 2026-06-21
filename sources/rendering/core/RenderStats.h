#pragma once
#include <atomic>
#include <cstdint>

namespace render
{
// Per-frame draw-call + primitive counters for the developer overlay. Geometry draws
// (Mesh::Draw / DrawInstanced) accumulate during recording; NextFrame() snapshots the
// completed frame's totals (read by the UI) and resets the accumulators. The UI shows the
// last completed frame, so reads are never torn against in-progress recording.
struct RenderStats
{
    std::atomic<uint32_t> drawCalls{0};
    std::atomic<uint64_t> primitives{0};
    uint32_t lastDrawCalls = 0;
    uint64_t lastPrimitives = 0;

    void AddDraw(uint32_t indexCount, uint32_t instanceCount)
    {
        drawCalls.fetch_add(1u, std::memory_order_relaxed);
        primitives.fetch_add(static_cast<uint64_t>(indexCount / 3u) * instanceCount, std::memory_order_relaxed);
    }

    void NextFrame()
    {
        lastDrawCalls = drawCalls.exchange(0u, std::memory_order_relaxed);
        lastPrimitives = primitives.exchange(0u, std::memory_order_relaxed);
    }
};

inline RenderStats g_renderStats;
} // namespace render
