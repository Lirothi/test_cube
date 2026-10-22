#pragma once

// Allocation-cost bench, run via "test_cube.exe --mem-bench" instead of the app (memory profiling
// plan M0, docs/memory_profiling_plan.md). Times the process's operator new/delete -- with the
// per-thread counters of core/memory/MemNew.cpp -- against the same mimalloc calls made directly,
// in one binary: single-threaded and with several threads at once (the counter blocks must not
// share a cache line). Rounds alternate A/B and the best of each is reported, so the difference is
// the counters' own cost per allocation. M3 reuses it for the cost of `--memtrack`.
//
// Verdict lines go to the session log (category core); returns 0.
int RunMemBench(const char* commandLine);
