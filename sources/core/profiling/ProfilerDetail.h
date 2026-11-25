#pragma once
// What the profiler's two source files share (architecture plan R1.2): core/profiling/Profiler.cpp (scopes, frames,
// the trace) and rendering/core/GpuProfiler.cpp (the GPU timestamps, whose resources come from gpu::Create*).
#include "core/profiling/Profiler.h"

#include <chrono>
#include <cstdint>

namespace profiler_detail {

inline uint64_t ToMicroseconds(Profiler::CpuClock::time_point tp) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count());
}

// Async-compute plan step 3: TWO GPU tracks. In a single-track trace an overlap is
// indistinguishable from a reordering, so this has to exist before any pass moves — otherwise
// there is no way to see whether async compute is doing anything at all.
//
// The GPU rows take tids 0 and 1 and every CPU thread shifts to +2, so the two GPU tracks stay
// adjacent at the top of the viewer. Trace-reading scripts that hard-coded `tid == 0` for "the GPU"
// must now read 0 AND 1.
constexpr uint32_t kGpuTraceThreadIndex = 0u;           // direct queue
constexpr uint32_t kGpuTraceThreadIndexCompute = 1u;    // async compute queue

} // namespace profiler_detail
