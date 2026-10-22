#include "rendering/core/MemGates.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "core/logging/Log.h"
#include "core/memory/MemCounters.h"
#include "core/memory/MemScope.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/MemoryReport.h"

namespace memgate
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr double kSamplePeriodSec = 0.25;

struct SelfTest
{
    bool on = false;
    MemTag tag = MemTag::Untagged;
    std::uint32_t kb = 64;
    bool cpu = true;
    bool gpu = false;
    bool announced = false;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::vector<void*> cpuBlocks;
    std::vector<ComPtr<ID3D12Resource>> gpuBuffers;
    std::string spec;
};
SelfTest g_self;

struct Series
{
    std::string name;
    std::vector<double> mb;
};

struct Readout
{
    bool on = false;
    bool done = false;
    double seconds = 0.0;
    double warmup = 15.0;
    double limit = 1.0;
    double startSec = -1.0;  // the first frame's time
    double nextSample = 0.0;
    std::vector<double> t;   // seconds since the warm-up ended
    std::vector<Series> series;
    int failures = 0;
};
Readout g_read;

bool TagByName(const std::string& name, MemTag& out)
{
    for (std::uint32_t t = 0; t < kMemTagCount; ++t)
    {
        if (name == kMemTagNames[t])
        {
            out = static_cast<MemTag>(t);
            return true;
        }
    }
    return false;
}

// The comma-separated value of `--<flag>=`, up to the next blank; empty when absent.
std::vector<std::string> Tokens(const char* cmdLine, const char* flag)
{
    std::vector<std::string> tokens;
    const char* p = cmdLine ? std::strstr(cmdLine, flag) : nullptr;
    if (p == nullptr) { return tokens; }
    p += std::strlen(flag);
    while (*p != '\0' && *p != ' ' && *p != '\t')
    {
        const char* end = p;
        while (*end != '\0' && *end != ',' && *end != ' ' && *end != '\t') { ++end; }
        tokens.emplace_back(p, end);
        p = *end == ',' ? end + 1 : end;
    }
    return tokens;
}

bool ParseSelfTest(const char* cmdLine)
{
    const std::vector<std::string> tokens = Tokens(cmdLine, "--mem-selftest=");
    if (tokens.empty()) { return false; }
    if (tokens[0].rfind("leak@", 0) != 0 || !TagByName(tokens[0].substr(5), g_self.tag))
    {
        LOG_ERROR(logging::LogCategory::Core, "--mem-selftest: expected leak@<tag> with a tag of MemTags.h, got '{}' -- no self-test", tokens[0]);
        return false;
    }
    for (std::size_t i = 1; i < tokens.size(); ++i)
    {
        const std::string& t = tokens[i];
        if (t.rfind("kb=", 0) == 0) { g_self.kb = static_cast<std::uint32_t>(std::max(1, std::atoi(t.c_str() + 3))); }
        else if (t == "cpu") { g_self.cpu = true; g_self.gpu = false; }
        else if (t == "gpu") { g_self.cpu = false; g_self.gpu = true; }
        else if (t == "both") { g_self.cpu = true; g_self.gpu = true; }
        else { LOG_WARNING(logging::LogCategory::Core, "--mem-selftest: unknown option '{}' (ignored)", t); }
    }
    g_self.on = true;
    g_self.spec = tokens[0];
    return true;
}

bool ParseReadout(const char* cmdLine)
{
    const std::vector<std::string> tokens = Tokens(cmdLine, "--mem-readout=");
    if (tokens.empty()) { return false; }
    g_read.seconds = std::atof(tokens[0].c_str());
    if (g_read.seconds <= 0.0)
    {
        LOG_ERROR(logging::LogCategory::Core, "--mem-readout: expected a window in seconds, got '{}' -- no readout", tokens[0]);
        return false;
    }
    for (std::size_t i = 1; i < tokens.size(); ++i)
    {
        const std::string& t = tokens[i];
        if (t.rfind("warmup=", 0) == 0) { g_read.warmup = std::max(0.0, std::atof(t.c_str() + 7)); }
        else if (t.rfind("limit=", 0) == 0) { g_read.limit = std::max(0.001, std::atof(t.c_str() + 6)); }
        else { LOG_WARNING(logging::LogCategory::Core, "--mem-readout: unknown option '{}' (ignored)", t); }
    }
    g_read.on = true;
    return true;
}

void Inject(ID3D12Device* device)
{
    const std::size_t bytes = static_cast<std::size_t>(g_self.kb) * 1024u;
    if (g_self.cpu)
    {
        void* p = nullptr;
        {
            MEM_SCOPE(g_self.tag);
            p = ::operator new(bytes);
        }
        std::memset(p, 0xA5, bytes); // committed for real, so private bytes see it too
        g_self.cpuBlocks.push_back(p); // the bookkeeping is outside the tag: it is not the leak
        g_self.bytes += bytes;
    }
    if (g_self.gpu && device != nullptr)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        if (SUCCEEDED(gpu::CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   g_self.tag, L"MemSelfTest.Leak", IID_PPV_ARGS(&buffer))))
        {
            g_self.gpuBuffers.push_back(std::move(buffer));
            g_self.bytes += bytes;
        }
    }
    ++g_self.frames;
    if (!g_self.announced)
    {
        g_self.announced = true;
        LOG_INFO(logging::LogCategory::Core, "mem self-test: injected {} KB {} under '{}' every frame ({})", g_self.kb,
                 g_self.cpu && g_self.gpu ? "cpu+gpu" : (g_self.gpu ? "gpu" : "cpu"), MemTagName(g_self.tag), g_self.spec);
    }
}

void Sample(ID3D12Device* device, double t)
{
    const render::MemoryPlatform plat = render::ReadMemoryPlatform(device);
    std::vector<std::int64_t> tags;
    ReadTagBytes(tags);
    if (g_read.series.empty())
    {
        g_read.series.push_back({ "private", {} });
        g_read.series.push_back({ "vram.local", {} });
        for (std::uint32_t i = 0; i < kMemTagCount; ++i) { g_read.series.push_back({ kMemTagNames[i], {} }); }
    }
    constexpr double kMb = 1.0 / (1024.0 * 1024.0);
    g_read.t.push_back(t);
    g_read.series[0].mb.push_back(static_cast<double>(plat.privateBytes) * kMb);
    g_read.series[1].mb.push_back(static_cast<double>(plat.vramLocalBytes) * kMb);
    for (std::uint32_t i = 0; i < kMemTagCount; ++i) { g_read.series[2 + i].mb.push_back(static_cast<double>(tags[i]) * kMb); }
}

// Least-squares slope of y over t on [b, e): (N sum(ty) - sum t sum y) / (N sum(t^2) - (sum t)^2).
double Slope(const std::vector<double>& t, const std::vector<double>& y, std::size_t b, std::size_t e)
{
    const double n = static_cast<double>(e - b);
    if (e <= b + 2) { return 0.0; }
    double st = 0.0, sy = 0.0, stt = 0.0, sty = 0.0;
    for (std::size_t i = b; i < e; ++i)
    {
        st += t[i];
        sy += y[i];
        stt += t[i] * t[i];
        sty += t[i] * y[i];
    }
    const double den = n * stt - st * st;
    return den > 0.0 ? (n * sty - st * sy) / den : 0.0;
}

void Verdict()
{
    const std::vector<double>& t = g_read.t;
    const std::size_t n = t.size();
    struct Row { std::size_t series; double slope; double thirds[3]; bool fail; };
    std::vector<Row> rows;
    // Thirds of the window by time: a sustained leak climbs in every one, a one-off step in one.
    const double span = n > 0 ? t.back() : 0.0;
    const std::size_t c1 = static_cast<std::size_t>(std::lower_bound(t.begin(), t.end(), span / 3.0) - t.begin());
    const std::size_t c2 = static_cast<std::size_t>(std::lower_bound(t.begin(), t.end(), 2.0 * span / 3.0) - t.begin());
    for (std::size_t s = 0; s < g_read.series.size(); ++s)
    {
        const std::vector<double>& y = g_read.series[s].mb;
        Row r{ s, Slope(t, y, 0, n), { Slope(t, y, 0, c1), Slope(t, y, c1, c2), Slope(t, y, c2, n) }, false };
        const double weakest = std::min({ r.thirds[0], r.thirds[1], r.thirds[2] });
        r.fail = r.slope > g_read.limit && weakest > 0.5 * g_read.limit;
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.slope > b.slope; });
    for (std::size_t i = 0; i < rows.size() && i < 8; ++i)
    {
        const Row& r = rows[i];
        const std::vector<double>& y = g_read.series[r.series].mb;
        LOG_INFO(logging::LogCategory::Core, "mem readout:   {} {:.3f} MB/s (thirds {:.3f} / {:.3f} / {:.3f}), {:.1f} -> {:.1f} MB",
                 g_read.series[r.series].name, r.slope, r.thirds[0], r.thirds[1], r.thirds[2], y.empty() ? 0.0 : y.front(),
                 y.empty() ? 0.0 : y.back());
    }
    for (const Row& r : rows)
    {
        if (!r.fail) { continue; }
        ++g_read.failures;
        LOG_ERROR(logging::LogCategory::Core, "mem readout: FAIL {} {:.2f} MB/s", g_read.series[r.series].name, r.slope);
    }
    const Row* steepest = rows.empty() ? nullptr : &rows.front();
    if (g_read.failures == 0)
    {
        LOG_INFO(logging::LogCategory::Core, "mem readout: PASS ({} series, steepest {} {:.3f} MB/s; {} samples over {:.0f} s after a {:.0f} s warm-up, limit {:.2f} MB/s)",
                 rows.size(), steepest ? g_read.series[steepest->series].name : std::string("-"), steepest ? steepest->slope : 0.0,
                 n, span, g_read.warmup, g_read.limit);
    }
    else
    {
        LOG_ERROR(logging::LogCategory::Core, "mem readout: FAIL ({} of {} series; {} samples over {:.0f} s after a {:.0f} s warm-up, limit {:.2f} MB/s)",
                  g_read.failures, rows.size(), n, span, g_read.warmup, g_read.limit);
    }
}
} // namespace

bool ParseAndConfigure(const char* cmdLine)
{
    const bool self = ParseSelfTest(cmdLine);
    const bool read = ParseReadout(cmdLine);
    if (read)
    {
        LOG_INFO(logging::LogCategory::Core, "mem readout: {:.0f} s warm-up, then {:.0f} s of samples at {:.0f} Hz; FAIL above {:.2f} MB/s sustained (cpu tags {})",
                 g_read.warmup, g_read.seconds, 1.0 / kSamplePeriodSec, g_read.limit, mem::MemTrackEnabled() ? "on" : "OFF");
    }
    return self || read;
}

bool OnFrameEnd(ID3D12Device* device, double nowSec)
{
    if (g_self.on) { Inject(device); }
    if (!g_read.on || g_read.done) { return false; }
    if (g_read.startSec < 0.0)
    {
        g_read.startSec = nowSec;
        g_read.nextSample = nowSec + g_read.warmup;
        return false;
    }
    if (nowSec < g_read.nextSample) { return false; }
    g_read.nextSample = std::max(g_read.nextSample + kSamplePeriodSec, nowSec);
    Sample(device, nowSec - g_read.startSec - g_read.warmup);
    if (nowSec - g_read.startSec < g_read.warmup + g_read.seconds) { return false; }
    Verdict();
    g_read.done = true;
    return true;
}

void ReleaseSelfTest()
{
    if (!g_self.on || g_self.frames == 0) { return; }
    for (void* p : g_self.cpuBlocks) { ::operator delete(p); }
    g_self.cpuBlocks.clear();
    g_self.gpuBuffers.clear();
    LOG_INFO(logging::LogCategory::Core, "mem self-test: lost {:.1f} MB over {} frames under '{}' (released at shutdown)",
             static_cast<double>(g_self.bytes) / (1024.0 * 1024.0), g_self.frames, MemTagName(g_self.tag));
    g_self.frames = 0;
}

int ExitCode()
{
    return g_read.failures;
}

void ReadTagBytes(std::vector<std::int64_t>& out)
{
    out.assign(kMemTagCount, 0);
    gpu::CensusTotals census;
    gpu::ReadCensus(census);
    for (std::uint32_t t = 0; t < kMemTagCount; ++t)
    {
        const gpu::TagTotals& tt = census.tags[t];
        out[t] = static_cast<std::int64_t>(tt.bytes[0] + tt.bytes[1] + tt.descriptorBytes);
    }
    if (mem::MemTrackEnabled())
    {
        mem::CpuTagTotals cpu;
        mem::ReadCpuTagTotals(cpu);
        for (std::uint32_t t = 0; t < kMemTagCount; ++t) { out[t] += cpu.bytes[t]; }
    }
}
} // namespace memgate
