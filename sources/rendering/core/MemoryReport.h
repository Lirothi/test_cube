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
} // namespace render
