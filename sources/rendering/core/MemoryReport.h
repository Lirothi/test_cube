#pragma once
// The memory line in the session log.
//
// A leak that grows by gigabytes over a minute of flying (2026-09-03: the RT retire bin) was
// invisible to every existing instrument: mimalloc's exit statistics count only its own heap
// and only at exit, the profiler counts time, the HUD counts draws, and Visual Studio's heap
// snapshots see neither mimalloc's pages nor a D3D12 resource. What catches this class of bug
// is one line, every few seconds, that puts the process, the GPU and the subsystems that own
// resources side by side -- so a growing number stands out in the first minute, and its column
// says which owner to open.
//
//   [INFO][core] mem: private 1234 MB (+12) ws 1500 | vram local 2345/8000 MB (+30) nonlocal 12 |
//                mi commit 800 MB | rt.as 456 MB | rt.as.bin 12 MB
//
// Process numbers come from GetProcessMemoryInfo, VRAM from the device adapter's
// QueryVideoMemoryInfo (local = dedicated video memory, non-local = system memory the GPU maps),
// `mi commit` from mimalloc. Everything after the bar is a PROVIDER: a subsystem that owns
// memory the process counters cannot attribute registers a name and a byte-count callback.
// Deltas are against the previous line.
//
// Not a tracer: it says HOW MUCH and roughly WHO, never which allocation. A per-allocation
// trace with call stacks (Unreal Insights' memory channel is the model) is a later step, noted
// in docs/bug_rt_retire_bin_leak.md.

#include <cstdint>

struct ID3D12Device;

namespace render
{
using MemoryProviderFn = std::uint64_t (*)(const void* self);

// Register a byte-count provider under `name` (a static string). `self` is passed back to `fn`
// and is the key for Unregister. Called from the main thread only.
void RegisterMemoryProvider(const char* name, MemoryProviderFn fn, const void* self);
void UnregisterMemoryProvider(const void* self);

// Emit the line if `periodSec` has passed since the last one (the first call emits). Main thread,
// after the previous frame's work has been joined -- providers read their owners' state.
void TickMemoryReport(ID3D12Device* device, double nowSec, double periodSec = 5.0);

// THE WHOLE CARD, not this process's share of it.
//
// `QueryVideoMemoryInfo` above answers "how much have I got out", which is the right question
// for a leak in our own resources and the wrong one the moment something ELSE on the machine
// holds video memory. The local model server is a separate process: it had 18.6 GB of a 24 GB
// card and this process could not see a byte of it, so "vram local 1400 MB" was true and
// useless for deciding whether the next allocation would fit.
//
// Read through NVML, which ships with the NVIDIA driver and is loaded on demand -- no build
// dependency and no failure on a machine without it. Both zero when unavailable, and a caller
// that gets zero should say "unknown" rather than "0 MB".
void GpuMemoryTotals(std::uint64_t& outUsedBytes, std::uint64_t& outTotalBytes);
} // namespace render
