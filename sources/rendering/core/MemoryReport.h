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
//   [INFO][core] mem: private 2951 MB (+0) ws 2421 | vram local 1804/23370 MB (+0) nonlocal 245 |
//                gpu local 1524 nonlocal 76 MB (823 res) untracked 279 | mi commit 182 MB |
//                heaps 202 MB (9) | other 2566 MB | new/frame 1510 (main 1430) 0.20 MB live 30335 |
//                tex.ret 0.0 MB | rt.as.bin 0.0 MB | rt.as.retired 8.1 MB
//
// Process numbers come from GetProcessMemoryInfo, VRAM from the device adapter's
// QueryVideoMemoryInfo (local = dedicated video memory, non-local = system memory the GPU maps).
// `gpu` is the GPU census (rendering/core/GpuAlloc.h): what the engine's own objects hold, by pool;
// `untracked` = vram local - gpu local (the driver, DLSS/Streamline, the ImGui backend).
// `mi commit` from mimalloc's own statistics (mi_stats_get; NOT mi_process_info, whose "commit"
// is the process's private bytes again), `heaps` from HeapSummary over every NT heap of the process (CRT
// malloc, the D3D12 runtime, the driver), `other` = private - mi commit - heaps. On this machine
// `other` is mostly WRITE-COMBINED pages that track the process's video memory one for one (M1:
// DLSS off moved vram +681 MB and write-combined +633), so private counts the VRAM a second time;
// the Debug line `mem regions:` (once a minute) splits it. `new/frame` is operator new calls per frame since the previous line,
// all threads and the main thread's share, from the per-thread counters of
// core/memory/MemCounters.h; `live` = news minus deletes since process start. A Debug line
// `mem threads:` names the busiest allocating threads (memory profiling plan M0), `mem gpu:` the
// census by tag. Everything after that is a PROVIDER: a counter the census cannot give (a retire
// bin's occupancy, a cumulative total) registered as a name and a byte-count callback. Deltas are
// against the previous line.
//
// Not a tracer: it says HOW MUCH and WHO, never which allocation. The per-allocation trace with
// call stacks (Unreal Insights' memory channel is the model) is docs/memory_profiling_plan.md M4.

#include <cstdint>
#include <string>

#include "core/memory/MemCounters.h"
#include "rendering/core/GpuAlloc.h"

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

// The frame's CBV/SRV/UAV descriptor ring as a frame left it (DescriptorAllocator::ResetPerFrame): the mem: line prints
// the period's peak as `desc ring <peak>/<capacity>`. An overflow is a crash; this is how close the frames come.
void NoteDescriptorRingUse(std::uint32_t used, std::uint32_t capacity);

// The memory REPORT (UE's MemReport / rhi.DumpResourceMemory, memory profiling plan M1-M2): one
// multi-line Info dump into the session log -- the platform breakdown with the address-space walk,
// the GPU census as a tag tree, and every live GPU object by size. "--memreport[=<sec>]" schedules
// one `afterSec` after the first TickMemoryReport (default 10 s); RequestMemoryReport asks for one
// on the next tick. Main thread.
void ScheduleMemoryReport(double afterSec);
void RequestMemoryReport();

// The numbers of the memory line, read now (memory profiling plan M2): for the Memory tab and the
// MCP `get_memory`. The allocation rates are the latest line's (they are per-period numbers).
struct MemoryPlatform
{
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSetBytes = 0;
    std::uint64_t mimallocCommitBytes = 0; // 0 = unknown (a mimalloc without OS statistics)
    std::uint64_t ntHeapsBytes = 0;
    std::uint32_t ntHeapCount = 0;
    bool haveVram = false;
    std::uint64_t vramLocalBytes = 0;
    std::uint64_t vramBudgetBytes = 0;
    std::uint64_t vramNonLocalBytes = 0;
    double newPerFrame = 0.0;     // operator new calls per frame, all threads, latest line
    double mainNewPerFrame = 0.0; // the main thread's share
    double newBytesPerFrame = 0.0;
    std::uint64_t liveAllocations = 0;
    std::uint64_t frame = 0;      // TickMemoryReport calls so far
};
MemoryPlatform ReadMemoryPlatform(ID3D12Device* device);

// A named snapshot of the platform and the GPU census by tag (Insights' "Size A / Size B / Diff",
// MemTagTable.cpp:289-497): SaveMemorySnapshot stores the current state under `label`, replacing
// one of the same name; FindMemorySnapshot copies it out, false when there is none. Main thread.
struct MemorySnapshot
{
    MemoryPlatform platform;
    gpu::CensusTotals census;
    mem::CpuTagTotals cpu; // zeros without --memtrack
    double timeSec = 0.0;  // QueryPerformanceCounter seconds when taken
};
void SaveMemorySnapshot(ID3D12Device* device, const std::string& label);
bool FindMemorySnapshot(const std::string& label, MemorySnapshot& out);
double MemoryNowSec();

// The MCP `get_memory` answer, as a JSON object: platform, GPU census, tags (those whose name
// contains `filter`, or every tag when empty), the `top` largest GPU objects (name or tag contains
// `filter`), `diff` against a saved snapshot, then `snapshot` stores the current state under that
// label; `report` also writes the full memory report into the session log.
std::string MemoryJson(ID3D12Device* device, const std::string& filter, int top, const std::string& snapshot,
                       const std::string& diff, bool report);

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

// The adapter's dedicated video memory (DXGI_ADAPTER_DESC), 0 when unavailable. The texture
// streaming pool defaults to a fraction of it (UE GPoolSizeVRAMPercentage).
std::uint64_t DedicatedVideoMemoryBytes(ID3D12Device* device);
} // namespace render
