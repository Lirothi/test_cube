#include "rendering/core/MemoryReport.h"

#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/logging/Log.h"
#include "core/memory/MemCounters.h"

#pragma comment(lib, "dxgi.lib")

namespace render
{
namespace
{
    struct Provider
    {
        const char* name;
        MemoryProviderFn fn;
        const void* self;
    };

    std::vector<Provider>& Providers()
    {
        static std::vector<Provider> providers;
        return providers;
    }

    // Per counter block (core/memory/MemCounters.h), the cumulative values at the previous line.
    struct ThreadPrev
    {
        std::uint64_t allocs = 0;
        std::uint64_t bytes = 0;
    };

    struct State
    {
        double lastSec = -1.0;
        std::uint64_t prevPrivate = 0;
        std::uint64_t prevLocal = 0;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
        bool adapterTried = false;
        // TickMemoryReport runs once per frame, so counting its calls counts frames.
        std::uint64_t frames = 0;
        std::uint64_t framesAtLine = 0;
        std::uint64_t lines = 0;
        ThreadPrev threadPrev[mem::kMaxThreadCounterBlocks + 1];
    };
    State& St()
    {
        static State s;
        return s;
    }

    // The device's own adapter, by LUID: no plumbing through the renderer, and the right one
    // on a laptop with two.
    IDXGIAdapter3* AdapterFor(ID3D12Device* device)
    {
        State& s = St();
        if (s.adapterTried || !device) { return s.adapter.Get(); }
        s.adapterTried = true;
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { return nullptr; }
        const LUID luid = device->GetAdapterLuid();
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) { return nullptr; }
        adapter.As(&s.adapter);
        return s.adapter.Get();
    }

    double Mb(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }
    long long MbDelta(std::uint64_t now, std::uint64_t prev)
    {
        const double d = Mb(now) - Mb(prev);
        return static_cast<long long>(d < 0.0 ? d - 0.5 : d + 0.5);
    }

    // The NT heaps of the process: the CRT's (C `malloc` -- WIC, parts of the engine), the D3D12
    // runtime's, the driver's. mimalloc takes its pages from VirtualAlloc directly, so these and
    // `mi commit` do not overlap. A heap destroyed between the two calls fails HeapSummary and is
    // skipped; driver and runtime heaps live as long as the device.
    struct HeapTotals
    {
        std::uint64_t committed = 0;
        std::uint32_t count = 0;
    };
    HeapTotals ProcessHeapTotals()
    {
        HANDLE heaps[64];
        HeapTotals t;
        const DWORD total = GetProcessHeaps(static_cast<DWORD>(std::size(heaps)), heaps);
        t.count = total;
        const DWORD visit = total < std::size(heaps) ? total : static_cast<DWORD>(std::size(heaps));
        for (DWORD i = 0; i < visit; ++i)
        {
            HEAP_SUMMARY summary{};
            summary.cb = sizeof(summary);
            if (HeapSummary(heaps[i], 0, &summary)) { t.committed += summary.cbCommitted; }
        }
        return t;
    }

    // One walk of the address space: the committed PRIVATE pages -- what `private` counts -- split
    // by what can be told about their owner without hooking anybody. mimalloc's own regions
    // (mi_is_in_heap_region), write-combined pages (the CPU side of D3D12 upload/readback
    // memory), executable pages, and the largest reservations by committed size: a remainder
    // that is one or two big reservations names a different owner than one spread over hundreds.
    // Debug only, and not every line: the walk costs milliseconds on the main thread.
    struct RegionTotals
    {
        std::uint64_t committed = 0;
        std::uint64_t mimalloc = 0;
        std::uint64_t writeCombine = 0;
        std::uint64_t exec = 0;
        std::uint32_t reservations = 0;
        struct Big
        {
            std::uintptr_t base = 0;
            std::uint64_t committed = 0;
            bool writeCombine = false;
            bool mimalloc = false;
        };
        static constexpr int kTop = 6;
        Big top[kTop];
        int topCount = 0;
        double ms = 0.0;
    };

    void PushBig(RegionTotals& t, const RegionTotals::Big& b)
    {
        if (b.committed == 0) { return; }
        int at = t.topCount < RegionTotals::kTop ? t.topCount : RegionTotals::kTop;
        while (at > 0 && t.top[at - 1].committed < b.committed) { --at; }
        if (at >= RegionTotals::kTop) { return; }
        const int last = t.topCount < RegionTotals::kTop ? t.topCount : RegionTotals::kTop - 1;
        for (int i = last; i > at; --i) { t.top[i] = t.top[i - 1]; }
        t.top[at] = b;
        if (t.topCount < RegionTotals::kTop) { ++t.topCount; }
    }

    RegionTotals WalkPrivateRegions()
    {
        RegionTotals t;
        LARGE_INTEGER f{}, t0{}, t1{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        auto* addr = static_cast<std::uint8_t*>(si.lpMinimumApplicationAddress);
        auto* const end = static_cast<std::uint8_t*>(si.lpMaximumApplicationAddress);
        RegionTotals::Big cur;
        const void* curBase = nullptr;
        MEMORY_BASIC_INFORMATION mbi{};
        while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            // Regions of one reservation are contiguous and share AllocationBase.
            if (mbi.AllocationBase != curBase)
            {
                PushBig(t, cur);
                cur = RegionTotals::Big{};
                cur.base = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
                curBase = mbi.AllocationBase;
                if (mbi.State != MEM_FREE) { ++t.reservations; }
            }
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE)
            {
                const std::uint64_t size = mbi.RegionSize;
                const bool wc = (mbi.Protect & PAGE_WRITECOMBINE) != 0;
                const bool mi = mi_is_in_heap_region(mbi.BaseAddress);
                t.committed += size;
                if (mi) { t.mimalloc += size; }
                else if (wc) { t.writeCombine += size; }
                else if ((mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                         PAGE_EXECUTE_WRITECOPY)) != 0) { t.exec += size; }
                cur.committed += size;
                cur.writeCombine = cur.writeCombine || wc;
                cur.mimalloc = cur.mimalloc || mi;
            }
            addr = static_cast<std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }
        PushBig(t, cur);
        QueryPerformanceCounter(&t1);
        t.ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
        return t;
    }

    // Allocation counters since the previous line: totals, the calling (main) thread's share, and
    // the busiest threads for the Debug breakdown.
    struct AllocPeriod
    {
        State* state = nullptr;
        std::uint32_t mainIndex = 0;
        std::uint64_t allocs = 0;     // cumulative, all threads
        std::uint64_t frees = 0;      // cumulative, all threads
        std::uint64_t periodAllocs = 0;
        std::uint64_t periodBytes = 0;
        std::uint64_t mainAllocs = 0;
        std::uint32_t threadsSeen = 0;
        struct Top
        {
            std::uint32_t threadId = 0;
            std::uint64_t allocs = 0;
            std::uint64_t bytes = 0;
        };
        static constexpr int kTop = 6;
        Top top[kTop];
        int topCount = 0;
    };

    void GatherThread(std::uint32_t index, const mem::ThreadAllocCounters& c, void* user)
    {
        AllocPeriod& p = *static_cast<AllocPeriod*>(user);
        ThreadPrev& prev = p.state->threadPrev[index];
        const std::uint64_t dAllocs = c.allocs - prev.allocs;
        const std::uint64_t dBytes = c.allocBytes - prev.bytes;
        prev.allocs = c.allocs;
        prev.bytes = c.allocBytes;
        p.allocs += c.allocs;
        p.frees += c.frees;
        p.periodAllocs += dAllocs;
        p.periodBytes += dBytes;
        ++p.threadsSeen;
        if (index == p.mainIndex) { p.mainAllocs = dAllocs; }
        if (dAllocs == 0) { return; }
        // Insertion into a short descending list.
        int at = p.topCount < AllocPeriod::kTop ? p.topCount : AllocPeriod::kTop;
        while (at > 0 && p.top[at - 1].allocs < dAllocs) { --at; }
        if (at >= AllocPeriod::kTop) { return; }
        const int last = p.topCount < AllocPeriod::kTop ? p.topCount : AllocPeriod::kTop - 1;
        for (int i = last; i > at; --i) { p.top[i] = p.top[i - 1]; }
        p.top[at] = AllocPeriod::Top{ c.threadId, dAllocs, dBytes };
        if (p.topCount < AllocPeriod::kTop) { ++p.topCount; }
    }
}

void GpuMemoryTotals(std::uint64_t& outUsedBytes, std::uint64_t& outTotalBytes)
{
    outUsedBytes = 0;
    outTotalBytes = 0;

    // Resolved once. nvml.dll lives beside the driver, so it is present on an NVIDIA box and
    // absent everywhere else; the absence is not an error, it is a machine without NVML.
    struct Nvml
    {
        // The v2 memory struct is what current drivers want; the v1 entry point is kept
        // because it is the one that exists on older ones and the fields we read are the
        // same two.
        struct MemoryV2 { unsigned int version; unsigned long long total, reserved, free, used; };
        using InitFn = int (*)();
        using HandleFn = int (*)(unsigned int, void**);
        using MemFn = int (*)(void*, MemoryV2*);
        using MemV1Fn = int (*)(void*, unsigned long long*);

        HMODULE dll = nullptr;
        HandleFn handleByIndex = nullptr;
        MemFn memoryV2 = nullptr;
        MemV1Fn memoryV1 = nullptr;
        void* device = nullptr;
        bool ready = false;

        Nvml()
        {
            dll = LoadLibraryA("nvml.dll");
            if (!dll)
            {
                // The driver's own directory, for installs that do not put it on the path.
                dll = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
            }
            if (!dll)
            {
                return;
            }
            auto init = reinterpret_cast<InitFn>(GetProcAddress(dll, "nvmlInit_v2"));
            handleByIndex = reinterpret_cast<HandleFn>(
                GetProcAddress(dll, "nvmlDeviceGetHandleByIndex_v2"));
            memoryV2 = reinterpret_cast<MemFn>(GetProcAddress(dll, "nvmlDeviceGetMemoryInfo_v2"));
            memoryV1 = reinterpret_cast<MemV1Fn>(GetProcAddress(dll, "nvmlDeviceGetMemoryInfo"));
            if (!init || !handleByIndex || (!memoryV2 && !memoryV1) || init() != 0)
            {
                return;
            }
            ready = handleByIndex(0, &device) == 0;
        }
    };
    static Nvml nvml;
    if (!nvml.ready)
    {
        return;
    }

    if (nvml.memoryV2)
    {
        Nvml::MemoryV2 info{};
        // NVML's versioned structs carry their size and a version number in the top bits.
        info.version = static_cast<unsigned int>(sizeof(info)) | (2u << 24);
        if (nvml.memoryV2(nvml.device, &info) == 0 && info.total > 0)
        {
            outUsedBytes = info.used;
            outTotalBytes = info.total;
            return;
        }
    }
    if (nvml.memoryV1)
    {
        // v1 lays out total, free, used as three consecutive 64-bit fields.
        unsigned long long v1[3] = { 0, 0, 0 };
        if (nvml.memoryV1(nvml.device, v1) == 0 && v1[0] > 0)
        {
            outTotalBytes = v1[0];
            outUsedBytes = v1[2];
        }
    }
}

void RegisterMemoryProvider(const char* name, MemoryProviderFn fn, const void* self)
{
    if (!name || !fn) { return; }
    Providers().push_back(Provider{ name, fn, self });
}

void UnregisterMemoryProvider(const void* self)
{
    std::vector<Provider>& p = Providers();
    for (size_t i = 0; i < p.size();)
    {
        if (p[i].self == self) { p.erase(p.begin() + static_cast<std::ptrdiff_t>(i)); }
        else { ++i; }
    }
}

void TickMemoryReport(ID3D12Device* device, double nowSec, double periodSec)
{
    State& s = St();
    ++s.frames;
    if (s.lastSec >= 0.0 && nowSec - s.lastSec < periodSec) { return; }
    const bool first = s.lastSec < 0.0;
    s.lastSec = nowSec;
    const std::uint64_t periodFrames = s.frames - s.framesAtLine;
    s.framesAtLine = s.frames;

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    const std::uint64_t privateBytes = pmc.PrivateUsage;
    const std::uint64_t workingSet = pmc.WorkingSetSize;

    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
    bool haveVram = false;
    if (IDXGIAdapter3* adapter = AdapterFor(device))
    {
        haveVram = SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
                   SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal));
    }

    // mimalloc's OWN committed bytes. Not mi_process_info: its "commit" is the whole process's
    // PagefileUsage on Windows -- the same number as `private` above, byte for byte (measured
    // 2026-10-22, M0) -- so until then this column was private twice over, and "almost all the
    // process lives in the mimalloc heap" (docs/bug_rt_retire_bin_leak.md) was read off it.
    mi_stats_t miStats{};
    mi_stats_get(sizeof(miStats), &miStats);
    const std::uint64_t miCommit = miStats.committed.current > 0 ? static_cast<std::uint64_t>(miStats.committed.current) : 0;

    const HeapTotals heaps = ProcessHeapTotals();
    // What neither mimalloc nor an NT heap accounts for: module images' writable pages, thread
    // stacks, the driver's own VirtualAllocs and mappings. Signed -- it is a remainder.
    const double otherMb = Mb(privateBytes) - Mb(miCommit) - Mb(heaps.committed);

    AllocPeriod alloc;
    alloc.state = &s;
    alloc.mainIndex = mem::CurrentThreadCounterIndex();
    mem::ForEachThreadCounters(&GatherThread, &alloc);

    char line[768];
    int n = std::snprintf(line, sizeof(line), "mem: private %.0f MB (%+lld) ws %.0f",
                          Mb(privateBytes), first ? 0ll : MbDelta(privateBytes, s.prevPrivate), Mb(workingSet));
    if (haveVram)
    {
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                           " | vram local %.0f/%.0f MB (%+lld) nonlocal %.0f",
                           Mb(local.CurrentUsage), Mb(local.Budget),
                           first ? 0ll : MbDelta(local.CurrentUsage, s.prevLocal), Mb(nonLocal.CurrentUsage));
    }
    if (miCommit > 0)
    {
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | mi commit %.0f MB", Mb(miCommit));
    }
    else
    {
        // A mimalloc built without OS statistics reports 0: say unknown, never "0 MB".
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | mi commit n/a");
    }
    n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | heaps %.0f MB (%u) | other %.0f MB",
                       Mb(heaps.committed), heaps.count, otherMb);
    // The first line covers static initialisation and boot, not frames: it states the totals.
    if (first || periodFrames == 0)
    {
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | new %llu live %llu",
                           static_cast<unsigned long long>(alloc.allocs),
                           static_cast<unsigned long long>(alloc.allocs - alloc.frees));
    }
    else
    {
        const double f = static_cast<double>(periodFrames);
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                           " | new/frame %.0f (main %.0f) %.2f MB live %llu",
                           static_cast<double>(alloc.periodAllocs) / f, static_cast<double>(alloc.mainAllocs) / f,
                           Mb(alloc.periodBytes) / f, static_cast<unsigned long long>(alloc.allocs - alloc.frees));
    }
    for (const Provider& p : Providers())
    {
        if (n >= static_cast<int>(sizeof(line)) - 40) { break; }
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | %s %.1f MB", p.name, Mb(p.fn(p.self)));
    }
    s.prevPrivate = privateBytes;
    s.prevLocal = local.CurrentUsage;
    LOG_INFO(logging::LogCategory::Core, "{}", line);

    // Which threads allocate: the busiest few since the previous line, per frame.
    if (!first && periodFrames > 0 && alloc.topCount > 0)
    {
        char threads[512];
        int t = std::snprintf(threads, sizeof(threads), "mem threads (%u):", alloc.threadsSeen);
        const double f = static_cast<double>(periodFrames);
        for (int i = 0; i < alloc.topCount && t < static_cast<int>(sizeof(threads)) - 80; ++i)
        {
            const AllocPeriod::Top& top = alloc.top[i];
            char name[64] = {};
            if (top.threadId == 0 || logging::GetThreadName(top.threadId, name, sizeof(name)) == 0)
            {
                std::snprintf(name, sizeof(name), "tid %u", top.threadId);
            }
            t += std::snprintf(threads + t, sizeof(threads) - static_cast<size_t>(t), " %s %.0f/f %.1f KB/f%s",
                               name, static_cast<double>(top.allocs) / f,
                               static_cast<double>(top.bytes) / 1024.0 / f,
                               i + 1 < alloc.topCount ? "," : "");
        }
        LOG_DEBUG(logging::LogCategory::Core, "{}", threads);
    }

    // The address-space breakdown: the second line (boot settled) and then once a minute.
    const std::uint64_t lineIndex = s.lines++;
    if ((lineIndex == 1 || (lineIndex > 1 && lineIndex % 12 == 1)) &&
        logging::ShouldLog(logging::LogLevel::Debug, logging::LogCategory::Core))
    {
        const RegionTotals r = WalkPrivateRegions();
        const std::uint64_t rest = r.committed - r.mimalloc - r.writeCombine - r.exec;
        char regions[640];
        int k = std::snprintf(regions, sizeof(regions),
                              "mem regions: private commit %.0f MB in %u reservations (walk %.1f ms) | mimalloc %.0f | "
                              "write-combined %.0f | exec %.0f | rest %.0f MB (NT heaps %.0f of it) | biggest:",
                              Mb(r.committed), r.reservations, r.ms, Mb(r.mimalloc), Mb(r.writeCombine), Mb(r.exec),
                              Mb(rest), Mb(heaps.committed));
        for (int i = 0; i < r.topCount && k < static_cast<int>(sizeof(regions)) - 64; ++i)
        {
            const RegionTotals::Big& b = r.top[i];
            k += std::snprintf(regions + k, sizeof(regions) - static_cast<size_t>(k), " %.0f MB @%llx%s%s",
                               Mb(b.committed), static_cast<unsigned long long>(b.base),
                               b.mimalloc ? " mi" : "", b.writeCombine ? " wc" : "");
        }
        LOG_DEBUG(logging::LogCategory::Core, "{}", regions);
    }
}
std::uint64_t DedicatedVideoMemoryBytes(ID3D12Device* device)
{
    IDXGIAdapter3* adapter = AdapterFor(device);
    if (!adapter) { return 0; }
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) { return 0; }
    return static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
}

} // namespace render
