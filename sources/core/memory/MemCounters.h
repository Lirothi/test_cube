#pragma once
// Per-thread allocation counters -- memory profiling plan M0 (docs/memory_profiling_plan.md).
//
// Every C++ allocation in this executable goes through the replaced operator new/delete in
// core/memory/MemNew.cpp (mimalloc underneath, exactly as mimalloc-new-delete.h had it). Each
// call bumps a counter block owned by the calling thread: one writer per block, so no locked
// instruction and no shared cache line on the hot path. A reader (the memory line) sums the
// blocks and differences them against its previous read, which gives allocations per frame per
// thread -- a number the engine had no way to see before.
//
// What these counters are NOT: they carry no owner (that is M3's tags) and no live byte count.
// `allocBytes` is what callers REQUESTED; mimalloc's own commit (`mi commit` on the memory line)
// stays the measure of how much is held. C `malloc`, the D3D runtime and the driver do not come
// through here at all.

#include <cstdint>

namespace mem
{
struct ThreadAllocCounters
{
    std::uint32_t threadId = 0;   // 0 for the shared overflow block
    std::uint64_t allocs = 0;     // operator new calls that returned memory
    std::uint64_t frees = 0;      // operator delete calls on a non-null pointer
    std::uint64_t allocBytes = 0; // bytes requested by those allocations
};

// One block per thread that ever allocated, handed out in creation order and never recycled: a
// block keeps the counts of a thread that has exited, so every sum stays exact. Threads beyond
// the capacity share one overflow block (atomic adds, still exact, just slower).
inline constexpr std::uint32_t kMaxThreadCounterBlocks = 1024;

// Visits every block taken so far; `index` is stable for the life of the process (the overflow
// block is `kMaxThreadCounterBlocks`), so a caller can keep per-block history in an array of
// kMaxThreadCounterBlocks + 1. Values are cumulative since process start. Reads race with the
// owning threads by design: a relaxed load of a single-writer counter is at worst one count stale.
using ThreadCountersVisitor = void (*)(std::uint32_t index, const ThreadAllocCounters& counters, void* user);
void ForEachThreadCounters(ThreadCountersVisitor visit, void* user);

// The calling thread's block index (taking one if it has none yet).
std::uint32_t CurrentThreadCounterIndex();
} // namespace mem
