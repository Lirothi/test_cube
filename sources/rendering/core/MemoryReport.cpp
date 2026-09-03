#include "rendering/core/MemoryReport.h"

#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <mimalloc.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/logging/Log.h"

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

    struct State
    {
        double lastSec = -1.0;
        std::uint64_t prevPrivate = 0;
        std::uint64_t prevLocal = 0;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
        bool adapterTried = false;
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
    if (s.lastSec >= 0.0 && nowSec - s.lastSec < periodSec) { return; }
    const bool first = s.lastSec < 0.0;
    s.lastSec = nowSec;

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

    size_t elapsedMs = 0, userMs = 0, sysMs = 0, rss = 0, peakRss = 0, commit = 0, peakCommit = 0, faults = 0;
    mi_process_info(&elapsedMs, &userMs, &sysMs, &rss, &peakRss, &commit, &peakCommit, &faults);

    char line[512];
    int n = std::snprintf(line, sizeof(line), "mem: private %.0f MB (%+lld) ws %.0f",
                          Mb(privateBytes), first ? 0ll : MbDelta(privateBytes, s.prevPrivate), Mb(workingSet));
    if (haveVram)
    {
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                           " | vram local %.0f/%.0f MB (%+lld) nonlocal %.0f",
                           Mb(local.CurrentUsage), Mb(local.Budget),
                           first ? 0ll : MbDelta(local.CurrentUsage, s.prevLocal), Mb(nonLocal.CurrentUsage));
    }
    n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | mi commit %.0f MB", Mb(commit));
    for (const Provider& p : Providers())
    {
        if (n >= static_cast<int>(sizeof(line)) - 40) { break; }
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " | %s %.1f MB", p.name, Mb(p.fn(p.self)));
    }
    s.prevPrivate = privateBytes;
    s.prevLocal = local.CurrentUsage;
    LOG_INFO(logging::LogCategory::Core, "{}", line);
}
} // namespace render
