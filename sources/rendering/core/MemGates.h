#pragma once
// Memory gates -- memory profiling plan M6 (docs/memory_profiling_plan.md).
//
// A detector is trusted only after it has caught a leak made on purpose (as --gbv-selftest proves a
// GBV mode), so the gates come with their own leak:
//
//   --mem-selftest=leak@<tag>[,kb=<n>][,cpu|gpu|both]
//       every frame, loses `kb` KB (default 64) under that tag: an operator new (cpu, the default)
//       and/or a committed GPU buffer (gpu). The session log says `mem self-test: injected ...`; a
//       gate whose run lacks that line proved nothing by staying silent.
//
//   --mem-readout=<seconds>[,warmup=<s>][,limit=<MB/s>]
//       after `warmup` (15 s) samples private bytes, vram local and every tag (GPU census + CPU
//       tags; the switch turns the M3 pointer map on like --memtrack) four times a second for
//       `seconds`, fits a least-squares slope to each (MallocLeakDetection.cpp:270-290 -- with the
//       determinant UE gets wrong, N*sum(x^2) - (sum x)^2), and FAILs a series that grows faster
//       than `limit` (1 MB/s, owner's Q5) AND keeps growing in each third of the window -- a
//       sustained climb, not one capacity step (M5 found those: a shadow ring resized mid-orbit).
//       Verdict `mem readout: PASS|FAIL <series> <MB/s>` in the session log, then the app quits
//       with exit code = the number of failing series.
//
// The --scene-stress revisit check (a level's tags on its return against its first visit) lives in
// app/diagnostics/SceneStress.cpp on top of the readers here.

#include <cstdint>
#include <string>
#include <vector>

#include "core/memory/MemTags.h"

struct ID3D12Device;

namespace memgate
{
// Both switches parsed from the narrow command line; false when absent (or malformed -- logged).
bool ParseAndConfigure(const char* cmdLine);

// Main loop, once per finished frame: the self-test's leak, the readout's sample. True = the
// readout is done and the app should quit (ExitCode() has the verdict).
bool OnFrameEnd(ID3D12Device* device, double nowSec);

// Before the device goes: the self-test's GPU buffers (a leak on purpose, not one at exit).
void ReleaseSelfTest();

// 0 unless a readout failed: the number of failing series.
int ExitCode();

// Live bytes per tag right now: the GPU census (resources by pool + descriptor heaps) plus the CPU
// tags when the pointer map is on. For the readout and the scene-stress revisit check.
void ReadTagBytes(std::vector<std::int64_t>& out);
} // namespace memgate
