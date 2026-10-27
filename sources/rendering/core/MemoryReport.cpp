#include "rendering/core/MemoryReport.h"

#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/logging/Log.h"
#include "core/memory/MemCounters.h"
#include "core/profiling/Profiler.h"
#include "rendering/core/GpuAlloc.h"
#include "third_party/json/json.hpp"

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
        // The memory report: scheduled relative to the first tick, or requested for the next one.
        double firstTickSec = -1.0;
        double reportAfterSec = -1.0;
        bool reportRequested = false;
        // The latest line's allocation rates, for ReadMemoryPlatform (they are per-period numbers).
        double newPerFrame = 0.0;
        double mainNewPerFrame = 0.0;
        double newBytesPerFrame = 0.0;
        std::uint64_t liveAllocations = 0;
        // Named snapshots (get_memory snapshot/diff, the Memory tab).
        std::unordered_map<std::string, MemorySnapshot> snapshots;
        ThreadPrev threadPrev[mem::kMaxThreadCounterBlocks + 1];
        // NoteDescriptorRingUse: the period's peak of the frame descriptor ring, and its size.
        std::atomic<std::uint32_t> ringPeak{ 0 };
        std::atomic<std::uint32_t> ringCapacity{ 0 };
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

MemoryPlatform ReadMemoryPlatform(ID3D12Device* device)
{
    State& s = St();
    MemoryPlatform p;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    p.privateBytes = pmc.PrivateUsage;
    p.workingSetBytes = pmc.WorkingSetSize;

    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
    if (IDXGIAdapter3* adapter = AdapterFor(device))
    {
        p.haveVram = SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
                     SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal));
    }
    p.vramLocalBytes = local.CurrentUsage;
    p.vramBudgetBytes = local.Budget;
    p.vramNonLocalBytes = nonLocal.CurrentUsage;

    // mimalloc's OWN committed bytes. Not mi_process_info: its "commit" is the whole process's
    // PagefileUsage on Windows -- the same number as `private` above, byte for byte (measured
    // 2026-10-22, M0) -- so until then this column was private twice over, and "almost all the
    // process lives in the mimalloc heap" (docs/bug_rt_retire_bin_leak.md) was read off it.
    mi_stats_t miStats{};
    mi_stats_get(sizeof(miStats), &miStats);
    p.mimallocCommitBytes = miStats.committed.current > 0 ? static_cast<std::uint64_t>(miStats.committed.current) : 0;

    const HeapTotals heaps = ProcessHeapTotals();
    p.ntHeapsBytes = heaps.committed;
    p.ntHeapCount = heaps.count;

    p.newPerFrame = s.newPerFrame;
    p.mainNewPerFrame = s.mainNewPerFrame;
    p.newBytesPerFrame = s.newBytesPerFrame;
    p.liveAllocations = s.liveAllocations;
    p.frame = s.frames;
    return p;
}

double MemoryNowSec()
{
    LARGE_INTEGER f{}, t{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

void SaveMemorySnapshot(ID3D12Device* device, const std::string& label)
{
    MemorySnapshot snap;
    snap.platform = ReadMemoryPlatform(device);
    gpu::ReadCensus(snap.census);
    mem::ReadCpuTagTotals(snap.cpu);
    snap.timeSec = MemoryNowSec();
    St().snapshots[label] = snap;
}

bool FindMemorySnapshot(const std::string& label, MemorySnapshot& out)
{
    const State& s = St();
    const auto it = s.snapshots.find(label);
    if (it == s.snapshots.end()) { return false; }
    out = it->second;
    return true;
}

std::string MemoryJson(ID3D12Device* device, const std::string& filter, int top, const std::string& snapshot,
                       const std::string& diff, bool report)
{
    using nlohmann::json;
    const auto lower = [](std::string text) {
        for (char& c : text) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return text;
    };
    // Two decimals: what a person reads, and stable enough to diff by eye.
    const auto mb = [](double bytes) { return std::round(bytes / (1024.0 * 1024.0) * 100.0) / 100.0; };
    const auto dmb = [&](std::uint64_t now, std::uint64_t then) {
        return mb(static_cast<double>(now) - static_cast<double>(then));
    };
    const std::string want = lower(filter);
    const MemoryPlatform plat = ReadMemoryPlatform(device);
    gpu::CensusTotals census;
    gpu::ReadCensus(census);
    const gpu::TagTotals& all = census.all;
    const int kLocal = static_cast<int>(gpu::MemPool::Local);
    const int kNonLocal = static_cast<int>(gpu::MemPool::NonLocal);

    json out;
    out["frame"] = plat.frame;
    json platform = {
        { "privateMB", mb(static_cast<double>(plat.privateBytes)) },
        { "workingSetMB", mb(static_cast<double>(plat.workingSetBytes)) },
        { "mimallocCommitMB", plat.mimallocCommitBytes > 0 ? json(mb(static_cast<double>(plat.mimallocCommitBytes))) : json(nullptr) },
        { "ntHeapsMB", mb(static_cast<double>(plat.ntHeapsBytes)) },
        { "ntHeaps", plat.ntHeapCount },
        { "newPerFrame", std::round(plat.newPerFrame) },
        { "mainNewPerFrame", std::round(plat.mainNewPerFrame) },
        { "newMBPerFrame", mb(plat.newBytesPerFrame) },
        { "liveAllocations", plat.liveAllocations },
    };
    if (plat.haveVram)
    {
        platform["vramLocalMB"] = mb(static_cast<double>(plat.vramLocalBytes));
        platform["vramBudgetMB"] = mb(static_cast<double>(plat.vramBudgetBytes));
        platform["vramNonLocalMB"] = mb(static_cast<double>(plat.vramNonLocalBytes));
    }
    out["platform"] = std::move(platform);

    json gpuJson = {
        { "localMB", mb(static_cast<double>(all.bytes[kLocal])) },
        { "nonLocalMB", mb(static_cast<double>(all.bytes[kNonLocal])) },
        { "resources", all.resources },
        { "heaps", all.heaps },
        { "descriptorMB", mb(static_cast<double>(all.descriptorBytes)) },
        { "bufferSlackMB", mb(static_cast<double>(all.bufferSlack)) },
        { "created", census.created },
        { "released", census.released },
    };
    if (plat.haveVram)
    {
        gpuJson["untrackedLocalMB"] = dmb(plat.vramLocalBytes, all.bytes[kLocal]);
        gpuJson["untrackedNonLocalMB"] = dmb(plat.vramNonLocalBytes, all.bytes[kNonLocal]);
    }
    out["gpu"] = std::move(gpuJson);

    // The CPU tags (--memtrack, M3): null columns without it, so a reader can tell "off" from "zero".
    const bool tracking = mem::MemTrackEnabled();
    mem::CpuTagTotals cpu;
    mem::ReadCpuTagTotals(cpu);
    if (tracking)
    {
        std::int64_t live = 0;
        for (std::uint32_t t = 0; t < kMemTagCount; ++t) { live += cpu.bytes[t]; }
        out["cpu"] = {
            { "liveMB", mb(static_cast<double>(live)) },
            { "untaggedMB", mb(static_cast<double>(cpu.bytes[static_cast<std::uint32_t>(MemTag::Untagged)])) },
            { "slackMB", mb(static_cast<double>(plat.mimallocCommitBytes) - static_cast<double>(live)) },
            { "mapMB", mb(static_cast<double>(cpu.mapBytes)) },
            { "mapEntries", cpu.mapEntries },
            { "unknownFrees", cpu.unknownFrees },
        };
    }
    else
    {
        out["cpu"] = nullptr;
    }

    // Tags with anything in them (each its own row; a parent row holds only what was tagged with
    // the parent itself), biggest first.
    struct TagRow { std::uint32_t tag; std::int64_t bytes; };
    std::vector<TagRow> tagRows;
    for (std::uint32_t t = 0; t < kMemTagCount; ++t)
    {
        const gpu::TagTotals& tt = census.tags[t];
        if (tt.resources == 0 && tt.heaps == 0 && cpu.count[t] == 0 && cpu.bytes[t] == 0) { continue; }
        if (!want.empty() && lower(kMemTagNames[t]).find(want) == std::string::npos) { continue; }
        tagRows.push_back(TagRow{ t, static_cast<std::int64_t>(tt.bytes[kLocal] + tt.bytes[kNonLocal]) + cpu.bytes[t] });
    }
    std::sort(tagRows.begin(), tagRows.end(), [](const TagRow& a, const TagRow& b) { return a.bytes > b.bytes; });
    out["tagColumns"] = json::array({ "tag", "localMB", "nonLocalMB", "resources", "heaps", "cpuMB", "cpuAllocations" });
    json tags = json::array();
    for (const TagRow& r : tagRows)
    {
        const gpu::TagTotals& tt = census.tags[r.tag];
        tags.push_back(json::array({ kMemTagNames[r.tag], mb(static_cast<double>(tt.bytes[kLocal])),
                                     mb(static_cast<double>(tt.bytes[kNonLocal])), tt.resources, tt.heaps,
                                     tracking ? json(mb(static_cast<double>(cpu.bytes[r.tag]))) : json(nullptr),
                                     tracking ? json(cpu.count[r.tag]) : json(nullptr) }));
    }
    out["tags"] = std::move(tags);

    if (top > 0)
    {
        std::vector<gpu::RecordInfo> records;
        gpu::SnapshotRecords(records);
        if (!want.empty())
        {
            records.erase(std::remove_if(records.begin(), records.end(), [&](const gpu::RecordInfo& r) {
                return lower(r.name).find(want) == std::string::npos &&
                       lower(MemTagName(r.tag)).find(want) == std::string::npos;
            }), records.end());
        }
        std::sort(records.begin(), records.end(),
                  [](const gpu::RecordInfo& a, const gpu::RecordInfo& b) { return a.bytes > b.bytes; });
        out["objectColumns"] = json::array({ "name", "tag", "pool", "kind", "MB" });
        json objects = json::array();
        const std::size_t n = std::min<std::size_t>(records.size(), static_cast<std::size_t>(top));
        for (std::size_t i = 0; i < n; ++i)
        {
            const gpu::RecordInfo& r = records[i];
            objects.push_back(json::array({ r.name, MemTagName(r.tag), r.pool == gpu::MemPool::Local ? "local" : "nonlocal",
                                            r.kind, mb(static_cast<double>(r.bytes)) }));
        }
        out["objects"] = std::move(objects);
        out["objectsMatched"] = records.size();
    }

    // The diff comes before the snapshot, so {diff: "A", snapshot: "A"} reads "since the last call".
    if (!diff.empty())
    {
        MemorySnapshot then;
        if (!FindMemorySnapshot(diff, then))
        {
            out["diff"] = { { "against", diff }, { "error", "no snapshot with this label -- take one with snapshot first" } };
        }
        else
        {
            json tagDiff = json::array();
            for (std::uint32_t t = 0; t < kMemTagCount; ++t)
            {
                const gpu::TagTotals& a = then.census.tags[t];
                const gpu::TagTotals& b = census.tags[t];
                const std::int64_t dCpu = cpu.bytes[t] - then.cpu.bytes[t];
                const std::int64_t dCpuCount = cpu.count[t] - then.cpu.count[t];
                if (a.bytes[kLocal] == b.bytes[kLocal] && a.bytes[kNonLocal] == b.bytes[kNonLocal] &&
                    a.resources == b.resources && a.heaps == b.heaps && dCpu == 0 && dCpuCount == 0)
                {
                    continue;
                }
                tagDiff.push_back(json::array({ kMemTagNames[t], dmb(b.bytes[kLocal], a.bytes[kLocal]),
                                                dmb(b.bytes[kNonLocal], a.bytes[kNonLocal]),
                                                static_cast<long long>(b.resources) - static_cast<long long>(a.resources),
                                                mb(static_cast<double>(dCpu)), dCpuCount }));
            }
            json d = {
                { "against", diff },
                { "seconds", std::round((MemoryNowSec() - then.timeSec) * 10.0) / 10.0 },
                { "frames", static_cast<long long>(plat.frame) - static_cast<long long>(then.platform.frame) },
                { "privateMB", dmb(plat.privateBytes, then.platform.privateBytes) },
                { "mimallocCommitMB", dmb(plat.mimallocCommitBytes, then.platform.mimallocCommitBytes) },
                { "ntHeapsMB", dmb(plat.ntHeapsBytes, then.platform.ntHeapsBytes) },
                { "gpuLocalMB", dmb(all.bytes[kLocal], then.census.all.bytes[kLocal]) },
                { "gpuNonLocalMB", dmb(all.bytes[kNonLocal], then.census.all.bytes[kNonLocal]) },
                { "resources", static_cast<long long>(all.resources) - static_cast<long long>(then.census.all.resources) },
                { "liveAllocations", static_cast<long long>(plat.liveAllocations) - static_cast<long long>(then.platform.liveAllocations) },
                { "tagColumns", json::array({ "tag", "dLocalMB", "dNonLocalMB", "dResources", "dCpuMB", "dCpuAllocations" }) },
                { "tags", std::move(tagDiff) },
            };
            if (plat.haveVram && then.platform.haveVram)
            {
                d["vramLocalMB"] = dmb(plat.vramLocalBytes, then.platform.vramLocalBytes);
            }
            out["diff"] = std::move(d);
        }
    }
    if (!snapshot.empty())
    {
        SaveMemorySnapshot(device, snapshot);
        out["snapshot"] = snapshot;
    }
    if (report)
    {
        RequestMemoryReport();
        out["report"] = "the full memory report goes into the session log on the next frame (memreport: lines)";
    }
    return out.dump();
}

namespace
{
    // While a profiler trace records, the process, the adapter and the GPU census ride along as
    // counter tracks ("ph":"C") beside the CPU/GPU scopes -- a stall next to the allocation that
    // caused it. Two DXGI reads and one census copy per frame, and only during a capture.
    void SampleTraceCounters(ID3D12Device* device)
    {
        Profiler& profiler = Profiler::Get();
        if (!profiler.IsTraceCapturing()) { return; }
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
        if (IDXGIAdapter3* adapter = AdapterFor(device))
        {
            adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
        }
        gpu::CensusTotals census;
        gpu::ReadCensus(census);
        const int kLocal = static_cast<int>(gpu::MemPool::Local);
        const int kNonLocal = static_cast<int>(gpu::MemPool::NonLocal);

        static const char* const kProcessNames[] = { "private", "vram local", "vram nonlocal", "gpu census local",
                                                     "gpu census nonlocal" };
        const double process[] = { Mb(pmc.PrivateUsage), Mb(local.CurrentUsage), Mb(nonLocal.CurrentUsage),
                                   Mb(census.all.bytes[kLocal]), Mb(census.all.bytes[kNonLocal]) };
        profiler.RecordTraceCounters("memory MB", kProcessNames, process, 5);

        // The census by top-level tag (children summed into their parent): a stacked track.
        const char* names[Profiler::kMaxTraceCounterSeries];
        double values[Profiler::kMaxTraceCounterSeries];
        int n = 0;
        for (std::uint32_t t = 0; t < kMemTagCount && n < Profiler::kMaxTraceCounterSeries; ++t)
        {
            if (kMemTagParents[t] != MemTag::Root) { continue; }
            std::uint64_t bytes = census.tags[t].bytes[kLocal] + census.tags[t].bytes[kNonLocal];
            for (std::uint32_t c = 0; c < kMemTagCount; ++c)
            {
                if (static_cast<std::uint32_t>(kMemTagParents[c]) == t)
                {
                    bytes += census.tags[c].bytes[kLocal] + census.tags[c].bytes[kNonLocal];
                }
            }
            if (bytes == 0) { continue; }
            names[n] = kMemTagNames[t];
            values[n] = Mb(bytes);
            ++n;
        }
        profiler.RecordTraceCounters("gpu tags MB", names, values, n);

        // The CPU tags the same way (--memtrack, M3).
        if (mem::MemTrackEnabled())
        {
            mem::CpuTagTotals cpu;
            mem::ReadCpuTagTotals(cpu);
            n = 0;
            for (std::uint32_t t = 0; t < kMemTagCount && n < Profiler::kMaxTraceCounterSeries; ++t)
            {
                if (kMemTagParents[t] != MemTag::Root) { continue; }
                std::int64_t bytes = cpu.bytes[t];
                for (std::uint32_t c = 0; c < kMemTagCount; ++c)
                {
                    if (static_cast<std::uint32_t>(kMemTagParents[c]) == t) { bytes += cpu.bytes[c]; }
                }
                if (bytes == 0) { continue; }
                names[n] = kMemTagNames[t];
                values[n] = static_cast<double>(bytes) / (1024.0 * 1024.0);
                ++n;
            }
            profiler.RecordTraceCounters("cpu tags MB", names, values, n);
        }
    }

    void AppendF(std::string& out, const char* fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n > 0) { out.append(buf, static_cast<size_t>(n < static_cast<int>(sizeof(buf)) ? n : sizeof(buf) - 1)); }
    }

    void DumpMemoryReport(ID3D12Device* device, std::uint64_t frame)
    {
        const MemoryPlatform plat = ReadMemoryPlatform(device);
        const bool haveVram = plat.haveVram;
        const double miCommit = Mb(plat.mimallocCommitBytes);
        const HeapTotals heaps{ plat.ntHeapsBytes, plat.ntHeapCount };
        const RegionTotals regions = WalkPrivateRegions();
        gpu::CensusTotals census;
        gpu::ReadCensus(census);
        std::vector<gpu::RecordInfo> records;
        gpu::SnapshotRecords(records);
        std::sort(records.begin(), records.end(),
                  [](const gpu::RecordInfo& a, const gpu::RecordInfo& b) { return a.bytes > b.bytes; });

        const auto& all = census.all;
        const double gpuLocal = Mb(all.bytes[static_cast<int>(gpu::MemPool::Local)]);
        const double gpuNonLocal = Mb(all.bytes[static_cast<int>(gpu::MemPool::NonLocal)]);

        std::string text;
        text.reserve(64 * 1024);
        AppendF(text, "memreport: begin (frame %llu)\n", static_cast<unsigned long long>(frame));
        AppendF(text, "memreport: process private %.1f MB, working set %.1f MB\n", Mb(plat.privateBytes), Mb(plat.workingSetBytes));
        AppendF(text, "memreport:   mimalloc commit %.1f MB | NT heaps %.1f MB (%u) | write-combined %.1f MB | exec %.1f MB | other private %.1f MB (walk %.1f ms, %u reservations)\n",
                miCommit, Mb(heaps.committed), heaps.count, Mb(regions.writeCombine), Mb(regions.exec),
                Mb(regions.committed) - Mb(regions.mimalloc) - Mb(regions.writeCombine) - Mb(regions.exec) - Mb(heaps.committed),
                regions.ms, regions.reservations);
        if (haveVram)
        {
            AppendF(text, "memreport: vram local %.1f / budget %.1f MB, non-local %.1f MB (DXGI, this process)\n",
                    Mb(plat.vramLocalBytes), Mb(plat.vramBudgetBytes), Mb(plat.vramNonLocalBytes));
        }
        AppendF(text, "memreport: gpu census local %.1f MB, non-local %.1f MB, %u resources + %u heaps (descriptors ~%.1f MB), buffer slack %.1f MB",
                gpuLocal, gpuNonLocal, all.resources, all.heaps, Mb(all.descriptorBytes), Mb(all.bufferSlack));
        if (haveVram)
        {
            AppendF(text, " | untracked local %.1f MB, non-local %.1f MB",
                    Mb(plat.vramLocalBytes) - gpuLocal, Mb(plat.vramNonLocalBytes) - gpuNonLocal);
        }
        text += "\n";

        // The CPU tags (--memtrack, M3) beside the GPU census, one tree.
        const bool tracking = mem::MemTrackEnabled();
        mem::CpuTagTotals cpu;
        mem::ReadCpuTagTotals(cpu);
        if (tracking)
        {
            std::int64_t live = 0;
            std::int64_t allocations = 0;
            for (std::uint32_t t = 0; t < kMemTagCount; ++t) { live += cpu.bytes[t]; allocations += cpu.count[t]; }
            AppendF(text, "memreport: cpu (--memtrack) live %.1f MB in %lld allocations, untagged %.1f MB, mimalloc slack %.1f MB, "
                    "map %.1f MB (%llu entries), unknown frees %llu\n",
                    static_cast<double>(live) / (1024.0 * 1024.0), static_cast<long long>(allocations),
                    static_cast<double>(cpu.bytes[static_cast<std::uint32_t>(MemTag::Untagged)]) / (1024.0 * 1024.0),
                    miCommit - static_cast<double>(live) / (1024.0 * 1024.0), Mb(cpu.mapBytes),
                    static_cast<unsigned long long>(cpu.mapEntries), static_cast<unsigned long long>(cpu.unknownFrees));
        }

        // The tag tree: each top-level row is its own bytes plus its children's.
        AppendF(text, tracking ? "memreport: tags (gpu local / non-local MB, resources, heaps | cpu MB, allocations):\n"
                               : "memreport: tags (local / non-local MB, resources, heaps):\n");
        const auto has = [&](std::uint32_t t) {
            return census.tags[t].resources > 0 || census.tags[t].heaps > 0 || cpu.count[t] != 0 || cpu.bytes[t] != 0;
        };
        const auto row = [&](const char* indent, int width, std::uint32_t tag, const gpu::TagTotals& g, std::int64_t cpuBytes,
                             std::int64_t cpuCount) {
            AppendF(text, "memreport: %s%-*s %9.1f %9.1f %5u %4u", indent, width, kMemTagNames[tag], Mb(g.bytes[0]), Mb(g.bytes[1]),
                    g.resources, g.heaps);
            if (tracking)
            {
                AppendF(text, " | %9.1f %7lld", static_cast<double>(cpuBytes) / (1024.0 * 1024.0), static_cast<long long>(cpuCount));
            }
            text += "\n";
        };
        for (std::uint32_t top = 0; top < kMemTagCount; ++top)
        {
            if (kMemTagParents[top] != MemTag::Root) { continue; }
            gpu::TagTotals sum = census.tags[top];
            std::int64_t cpuSum = cpu.bytes[top];
            std::int64_t cpuSumCount = cpu.count[top];
            bool hasChildren = false;
            bool any = has(top);
            for (std::uint32_t c = 0; c < kMemTagCount; ++c)
            {
                if (static_cast<std::uint32_t>(kMemTagParents[c]) != top) { continue; }
                const gpu::TagTotals& ct = census.tags[c];
                sum.bytes[0] += ct.bytes[0];
                sum.bytes[1] += ct.bytes[1];
                sum.resources += ct.resources;
                sum.heaps += ct.heaps;
                cpuSum += cpu.bytes[c];
                cpuSumCount += cpu.count[c];
                hasChildren = hasChildren || has(c);
                any = any || has(c);
            }
            if (!any) { continue; }
            row("  ", 16, top, sum, cpuSum, cpuSumCount);
            if (!hasChildren) { continue; }
            for (std::uint32_t c = 0; c < kMemTagCount; ++c)
            {
                if (static_cast<std::uint32_t>(kMemTagParents[c]) != top || !has(c)) { continue; }
                row("    ", 14, c, census.tags[c], cpu.bytes[c], cpu.count[c]);
            }
        }

        AppendF(text, "memreport: objects by size (MB, pool, kind, tag, name):\n");
        for (const gpu::RecordInfo& r : records)
        {
            AppendF(text, "memreport:   %9.3f %-8s %-9s %-16s %s\n", Mb(r.bytes),
                    r.pool == gpu::MemPool::Local ? "local" : "nonlocal", r.kind, MemTagName(r.tag), r.name.c_str());
        }
        AppendF(text, "memreport: end (%zu objects)\n", records.size());
        logging::WriteRawLines(logging::LogLevel::Info, logging::LogCategory::Core, text);
    }
}

void ScheduleMemoryReport(double afterSec)
{
    St().reportAfterSec = afterSec >= 0.0 ? afterSec : 10.0;
}

void NoteDescriptorRingUse(std::uint32_t used, std::uint32_t capacity)
{
    State& s = St();
    s.ringCapacity.store(capacity, std::memory_order_relaxed);
    std::uint32_t peak = s.ringPeak.load(std::memory_order_relaxed);
    while (used > peak && !s.ringPeak.compare_exchange_weak(peak, used, std::memory_order_relaxed)) {}
}

void RequestMemoryReport()
{
    St().reportRequested = true;
}

void TickMemoryReport(ID3D12Device* device, double nowSec, double periodSec)
{
    State& s = St();
    ++s.frames;
    SampleTraceCounters(device);
    if (s.firstTickSec < 0.0) { s.firstTickSec = nowSec; }
    if (s.reportRequested || (s.reportAfterSec >= 0.0 && nowSec - s.firstTickSec >= s.reportAfterSec))
    {
        s.reportRequested = false;
        s.reportAfterSec = -1.0;
        DumpMemoryReport(device, s.frames);
    }
    if (s.lastSec >= 0.0 && nowSec - s.lastSec < periodSec) { return; }
    const bool first = s.lastSec < 0.0;
    s.lastSec = nowSec;
    const std::uint64_t periodFrames = s.frames - s.framesAtLine;
    s.framesAtLine = s.frames;

    const MemoryPlatform plat = ReadMemoryPlatform(device);
    const std::uint64_t privateBytes = plat.privateBytes;
    const std::uint64_t workingSet = plat.workingSetBytes;
    const bool haveVram = plat.haveVram;
    const std::uint64_t miCommit = plat.mimallocCommitBytes;
    const HeapTotals heaps{ plat.ntHeapsBytes, plat.ntHeapCount };
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
                           Mb(plat.vramLocalBytes), Mb(plat.vramBudgetBytes),
                           first ? 0ll : MbDelta(plat.vramLocalBytes, s.prevLocal), Mb(plat.vramNonLocalBytes));
    }
    // The GPU census (rendering/core/GpuAlloc.h): what the engine's own resources hold, by pool.
    // `untracked` = the adapter's local usage minus ours -- the driver, DLSS/Streamline, the ImGui
    // backend. Signed: a remainder.
    gpu::CensusTotals census;
    gpu::ReadCensus(census);
    const std::uint64_t gpuLocal = census.all.bytes[static_cast<int>(gpu::MemPool::Local)];
    const std::uint64_t gpuNonLocal = census.all.bytes[static_cast<int>(gpu::MemPool::NonLocal)];
    n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | gpu local %.0f nonlocal %.0f MB (%u res)",
                       Mb(gpuLocal), Mb(gpuNonLocal), census.all.resources);
    if (haveVram)
    {
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " untracked %.0f",
                           Mb(plat.vramLocalBytes) - Mb(gpuLocal));
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
    if (const std::uint32_t ringCapacity = s.ringCapacity.load(std::memory_order_relaxed); ringCapacity > 0)
    {
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | desc ring %u/%u",
                           s.ringPeak.exchange(0, std::memory_order_relaxed), ringCapacity);
    }
    // CPU tags (--memtrack, M3): what operator new holds live, how much of it no scope claimed, the
    // allocator's own slack (mi commit - live) and the tag map's pages.
    mem::CpuTagTotals cpuTags;
    if (mem::MemTrackEnabled())
    {
        mem::ReadCpuTagTotals(cpuTags);
        std::int64_t live = 0;
        for (std::uint32_t t = 0; t < kMemTagCount; ++t) { live += cpuTags.bytes[t]; }
        const std::int64_t untagged = cpuTags.bytes[static_cast<std::uint32_t>(MemTag::Untagged)];
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                           " | cpu %.0f MB untagged %.1f MB (%.0f%%) slack %.0f MB map %.1f MB",
                           static_cast<double>(live) / (1024.0 * 1024.0), static_cast<double>(untagged) / (1024.0 * 1024.0),
                           live > 0 ? 100.0 * static_cast<double>(untagged) / static_cast<double>(live) : 0.0,
                           (static_cast<double>(miCommit) - static_cast<double>(live)) / (1024.0 * 1024.0),
                           Mb(cpuTags.mapBytes));
        if (cpuTags.unknownFrees > 0)
        {
            n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " unknown frees %llu",
                               static_cast<unsigned long long>(cpuTags.unknownFrees));
        }
    }
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
        s.newPerFrame = static_cast<double>(alloc.periodAllocs) / f;
        s.mainNewPerFrame = static_cast<double>(alloc.mainAllocs) / f;
        s.newBytesPerFrame = static_cast<double>(alloc.periodBytes) / f;
    }
    s.liveAllocations = alloc.allocs - alloc.frees;
    for (const Provider& p : Providers())
    {
        if (n >= static_cast<int>(sizeof(line)) - 40) { break; }
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | %s %.1f MB", p.name, Mb(p.fn(p.self)));
    }
    s.prevPrivate = privateBytes;
    s.prevLocal = plat.vramLocalBytes;
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

    // The address-space breakdown: once, on the second line (boot settled). Not periodically: the walk is 25-55 ms
    // on the main thread, and a Debug session logs Debug -- a hitch a minute there. --memreport walks on demand.
    const std::uint64_t lineIndex = s.lines++;
    if (lineIndex == 1 && logging::ShouldLog(logging::LogLevel::Debug, logging::LogCategory::Core))
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

        // The census by tag, biggest first: local/non-local MB and the resource count. Leaf rows
        // only (a parent is the sum of its children and adds nothing to a one-line view).
        struct Row
        {
            std::uint32_t tag = 0;
            std::uint64_t bytes = 0;
        };
        Row rows[kMemTagCount];
        int rowCount = 0;
        for (std::uint32_t t = 0; t < kMemTagCount; ++t)
        {
            const gpu::TagTotals& tt = census.tags[t];
            const std::uint64_t b = tt.bytes[0] + tt.bytes[1];
            if (b == 0) { continue; }
            int at = rowCount++;
            while (at > 0 && rows[at - 1].bytes < b) { rows[at] = rows[at - 1]; --at; }
            rows[at] = Row{ t, b };
        }
        char tags[1024];
        int g = std::snprintf(tags, sizeof(tags),
                              "mem gpu: %u res + %u heaps (descriptors ~%.1f MB), buffer slack %.1f MB, created %llu released %llu |",
                              census.all.resources, census.all.heaps, Mb(census.all.descriptorBytes),
                              Mb(census.all.bufferSlack), static_cast<unsigned long long>(census.created),
                              static_cast<unsigned long long>(census.released));
        for (int i = 0; i < rowCount && g < static_cast<int>(sizeof(tags)) - 64; ++i)
        {
            const gpu::TagTotals& tt = census.tags[rows[i].tag];
            g += std::snprintf(tags + g, sizeof(tags) - static_cast<size_t>(g), " %s %.1f/%.1f (%u)",
                               MemTagName(static_cast<MemTag>(rows[i].tag)), Mb(tt.bytes[0]), Mb(tt.bytes[1]),
                               tt.resources);
        }
        LOG_DEBUG(logging::LogCategory::Core, "{}", tags);

        // The CPU tags, biggest first (--memtrack): MB and live allocations.
        if (mem::MemTrackEnabled())
        {
            struct CpuRow
            {
                std::uint32_t tag = 0;
                std::int64_t bytes = 0;
            };
            CpuRow cpuRows[kMemTagCount];
            int cpuCount = 0;
            for (std::uint32_t t = 0; t < kMemTagCount; ++t)
            {
                const std::int64_t b = cpuTags.bytes[t];
                if (b == 0 && cpuTags.count[t] == 0) { continue; }
                int at = cpuCount++;
                while (at > 0 && cpuRows[at - 1].bytes < b) { cpuRows[at] = cpuRows[at - 1]; --at; }
                cpuRows[at] = CpuRow{ t, b };
            }
            char cpu[1024];
            int c = std::snprintf(cpu, sizeof(cpu), "mem cpu: map %llu entries %.1f MB, unknown frees %llu |",
                                  static_cast<unsigned long long>(cpuTags.mapEntries), Mb(cpuTags.mapBytes),
                                  static_cast<unsigned long long>(cpuTags.unknownFrees));
            for (int i = 0; i < cpuCount && c < static_cast<int>(sizeof(cpu)) - 64; ++i)
            {
                c += std::snprintf(cpu + c, sizeof(cpu) - static_cast<size_t>(c), " %s %.1f (%lld)",
                                   MemTagName(static_cast<MemTag>(cpuRows[i].tag)),
                                   static_cast<double>(cpuRows[i].bytes) / (1024.0 * 1024.0),
                                   static_cast<long long>(cpuTags.count[cpuRows[i].tag]));
            }
            LOG_DEBUG(logging::LogCategory::Core, "{}", cpu);
        }
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
