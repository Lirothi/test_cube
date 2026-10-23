#pragma once
// The memory trace -- memory profiling plan M4 (docs/memory_profiling_plan.md §4.4).
//
//     --memtrace[=all|cpu|gpu][,stacks=big|all|off|1/<N>][,start=<frame>]
//
// Every allocation and free as an event in traces/memtrace_<stamp>_<pid>.mtrace (the last 10 are
// kept): CPU (operator new/delete, through the M3 pointer map -- a CPU trace turns --memtrack on)
// and GPU (the M1 census). The shape is UE's Memory Insights trace (MemoryTrace.cpp:66-173): an
// event carries no time, a marker does (every 4096 events, and here every frame); a stack is sent
// once and referenced by id (CallstackTracePrivate.h:58-146); modules go with their PDB GUID+age
// (WindowsModuleDiagnostics.cpp:164-203). Two things UE cannot do: the trace may START late -- at
// its start every live object goes out as a synthetic alloc, taken from the census and the map
// under their own locks -- and its END record carries what the process itself counts alive, so a
// replay of the file can be checked against it (tools/memtrace.py verify).
//
// Ordering. Each thread writes into its own 64 KB buffer (VirtualAlloc, never the heap it
// measures) and publishes after each whole event; a writer thread drains the published bytes to the
// file (UE TlsBuffer.cpp). A global serial number orders events across threads. An event is
// emitted under the lock that owns its key -- the map's stripe, the census mutex -- so a free and
// the next alloc of the same address can never swap places, and the start snapshot meets every
// later event in order.

#include <cstdint>

#include "core/memory/MemTags.h"

namespace memtrace
{
// Root heaps (UE EMemoryTraceRootHeap: 0 System, 1 Video); the file's heap table names them.
enum Heap : std::uint8_t
{
    kHeapCpu = 0,
    kHeapGpuLocal = 1,
    kHeapGpuNonLocal = 2,
};

// What an allocation is (the census kind for the GPU). Bit 7: synthetic -- alive when the trace started.
enum Kind : std::uint8_t
{
    kKindCpu = 0,
    kKindBuffer = 1,
    kKindTexture = 2,
    kKindDescriptorHeap = 3,
    kKindQueryHeap = 4,
    kKindSynthetic = 0x80,
};

enum class StackMode : std::uint8_t
{
    Off,     // no stacks at all
    Big,     // CPU allocations of 64 KB and more (GPU: every allocation)
    All,     // every allocation
    Sampled, // one CPU allocation in N (GPU: every allocation)
};

struct Options
{
    bool cpu = true;
    bool gpu = true;
    StackMode stacks = StackMode::Big;
    std::uint32_t sampleEvery = 0; // StackMode::Sampled
    std::uint64_t startFrame = 0;  // 0 = at boot, before the device exists
};

// Parses the switch from the narrow command line. False when it is absent.
bool ParseSwitch(const char* cmdLine, Options& out);
// The answer MemNew needs at the process's FIRST allocation, from the raw wide command line and
// without allocating: does the trace want CPU events (and so the pointer map)?
bool WantsCpuRaw(const wchar_t* cmdLine) noexcept;

// main.cpp: keeps the options and starts at once when startFrame is 0.
void Configure(const Options& options);
// The main loop, once per finished frame: the frame marker, the delayed start.
void OnFrameEnd(std::uint64_t frame);
// After the main loop (idempotent): the End record with the process's own totals, the module and
// symbol tables, the file closed. A process that never gets here leaves a trace without them.
void Stop();

// ---- Emission. Allocation-free; the caller holds the lock that owns `key` (see above). ----
void EmitAlloc(std::uint64_t key, std::uint64_t size, std::uint8_t tag, std::uint8_t heap, std::uint8_t kind,
               std::uint32_t stack, const char* name) noexcept;
// `stack`: who freed it (format version 2, as UE's Free event carries a CallstackId since its version 2).
void EmitFree(std::uint64_t key, std::uint8_t heap, std::uint32_t stack) noexcept;
// A stack id for the caller's allocation or free (0 = none): the CPU policy for `size` / every GPU one.
// Call it OUTSIDE the key's lock -- the capture is the slow part.
std::uint32_t CaptureCpuStack(std::uint64_t size) noexcept;
std::uint32_t CaptureGpuStack() noexcept;

// What the GPU census holds alive when the trace stops, per tag (gpu::CensusTotals' columns).
struct GpuEndTotals
{
    std::uint64_t local[kMemTagCount] = {};
    std::uint64_t nonLocal[kMemTagCount] = {};
    std::uint64_t descriptor[kMemTagCount] = {};
    std::uint32_t resources[kMemTagCount] = {};
    std::uint32_t heaps[kMemTagCount] = {};
};
// The census's side (rendering/core/GpuAlloc.cpp registers it): `start` turns its events on and
// emits every live object as a synthetic alloc; stop turns them off and fills `out`. Both under
// the census lock.
using GpuHook = void (*)(bool start, GpuEndTotals* out);
void SetGpuHook(GpuHook hook) noexcept;
} // namespace memtrace
