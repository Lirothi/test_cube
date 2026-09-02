#include "core/diagnostics/BootProfile.h"

#include "core/logging/Log.h"

#include <cstdarg>
#include <cstdio>
#include <string>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace boot {
namespace {

using Clock = std::chrono::steady_clock;

struct Entry
{
    std::string      label;
    std::string      detail;
    double           startMs = 0.0;
    double           endMs   = -1.0;   // -1 while open
    bool             instant = false; // a Mark, not a phase -- explicit, because a real scope CAN
                                      // measure 0.0 ms and must still print as a duration
    int              depth   = 0;
    std::thread::id  thread;
};

struct WorstItem
{
    std::string item;
    double      ms = 0.0;
};

struct Bucket
{
    long long              count = 0;
    double                 totalMs = 0.0;
    std::vector<WorstItem> worst;   // kept sorted descending, capped
};

constexpr std::size_t kWorstKept = 6;

std::mutex                                  g_mutex;
std::vector<Entry>                          g_entries;
std::unordered_map<std::string, Bucket>     g_buckets;
std::unordered_map<std::string, long long>  g_counts;
int                                         g_dumpCount = 0;

// Per-thread nesting depth. Boot is mostly one thread, but level loading dispatches onto the task
// pool; a worker's scopes must not be indented under whatever the main thread happens to be inside.
thread_local int t_depth = 0;

Clock::time_point Origin()
{
    // Function-local static: the clock starts the first time anything asks, which is the earliest
    // BOOT_SCOPE in main(). No static-init-order question, no separate "call this first" step to
    // forget.
    static const Clock::time_point origin = Clock::now();
    return origin;
}

double NowMs()
{
    return std::chrono::duration<double, std::milli>(Clock::now() - Origin()).count();
}

const char* ConfigTag()
{
#ifdef _DEBUG
    return "Debug";
#else
    return "Release";
#endif
}

} // namespace

double ElapsedMs() { return NowMs(); }

void Mark(const char* label)
{
    const double t = NowMs();
    std::lock_guard<std::mutex> lock(g_mutex);
    Entry e;
    e.label   = label ? label : "?";
    e.startMs = t;
    e.endMs   = t;
    e.instant = true;
    e.depth   = t_depth;
    e.thread  = std::this_thread::get_id();
    g_entries.push_back(std::move(e));
}

Scope::Scope(const char* label) : Scope(label, std::string()) {}

Scope::Scope(const char* label, std::string detail)
{
    const double t = NowMs();
    std::lock_guard<std::mutex> lock(g_mutex);
    Entry e;
    e.label   = label ? label : "?";
    e.detail  = std::move(detail);
    e.startMs = t;
    e.depth   = t_depth++;
    e.thread  = std::this_thread::get_id();
    index_    = g_entries.size();
    g_entries.push_back(std::move(e));
}

Scope::~Scope()
{
    const double t = NowMs();
    std::lock_guard<std::mutex> lock(g_mutex);
    --t_depth;
    if (index_ < g_entries.size()) { g_entries[index_].endMs = t; }
}

void AddBucket(const char* name, double milliseconds, const char* item)
{
    AddBucket(name, milliseconds, std::string(item ? item : ""));
}

void AddBucket(const char* name, double milliseconds, const std::string& item)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Bucket& b = g_buckets[name ? name : "?"];
    ++b.count;
    b.totalMs += milliseconds;
    if (b.worst.size() < kWorstKept || milliseconds > b.worst.back().ms)
    {
        b.worst.push_back(WorstItem{ item, milliseconds });
        std::sort(b.worst.begin(), b.worst.end(),
                  [](const WorstItem& a, const WorstItem& c) { return a.ms > c.ms; });
        if (b.worst.size() > kWorstKept) { b.worst.resize(kWorstKept); }
    }
}

void SetFrameProfiling(bool on) { g_frameProfiling = on; }

void AddCount(const char* name, long long delta)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counts[name ? name : "?"] += delta;
}

// printf into a string, one report section at a time (the report used to be a file).
struct LineSink
{
    std::string* out;
    void Printf(const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        if (length > 0) { out->append(buffer, static_cast<std::size_t>(length) < sizeof(buffer) ? static_cast<std::size_t>(length) : sizeof(buffer) - 1); }
    }
};

void Dump(const char* reason)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    // Into the session log (no separate boot_profile.log, by the owner's decision): the header,
    // the aggregated work and the counters at Info under `profiling`, the per-scope timeline at
    // Debug — a Release session keeps the summary, a Debug session the whole report.
    std::string summary;
    std::string timeline;
    LineSink f{ &summary };
    ++g_dumpCount;

    const double total = NowMs();
    f.Printf("==== boot profile (%s) : %s : %.1f ms since process start ====\n",
                 ConfigTag(), reason ? reason : "?", total);
    f.Printf("  columns: [start ms] wall ms (self ms) label\n");
    f.Printf("  'self' = wall minus the direct children on the SAME thread; a big self time\n"
                    "  is unattributed work, i.e. exactly where the next scope should go.\n\n");

    const std::thread::id mainThread = g_entries.empty() ? std::this_thread::get_id()
                                                         : g_entries.front().thread;

    f.out = &timeline;
    for (std::size_t i = 0; i < g_entries.size(); ++i)
    {
        const Entry& e = g_entries[i];
        const bool   instant = e.instant;
        const double end     = (e.endMs < 0.0) ? total : e.endMs;
        const double wall    = end - e.startMs;

        // Self time: subtract children, which are the following entries at depth+1 on this thread,
        // up to the first entry that leaves this scope.
        double childSum = 0.0;
        if (!instant)
        {
            for (std::size_t k = i + 1; k < g_entries.size(); ++k)
            {
                const Entry& c = g_entries[k];
                if (c.thread != e.thread) { continue; }
                if (c.depth <= e.depth) { break; }
                if (c.depth == e.depth + 1 && !c.instant)
                {
                    childSum += ((c.endMs < 0.0) ? total : c.endMs) - c.startMs;
                }
            }
        }

        f.Printf("  [%9.1f] ", e.startMs);
        for (int d = 0; d < e.depth; ++d) { f.Printf("  "); }

        if (instant)
        {
            f.Printf(". %s", e.label.c_str());
        }
        else
        {
            f.Printf("%9.1f (%8.1f) %s", wall, wall - childSum, e.label.c_str());
        }
        if (!e.detail.empty()) { f.Printf("  [%s]", e.detail.c_str()); }
        if (e.thread != mainThread) { f.Printf("  {worker}"); }
        if (e.endMs < 0.0) { f.Printf("  <<< STILL OPEN"); }
        f.Printf("\n");
    }

    f.out = &summary;
    if (!g_buckets.empty())
    {
        f.Printf("  ---- aggregated work (repeated items) ----\n");
        std::vector<const std::pair<const std::string, Bucket>*> sorted;
        sorted.reserve(g_buckets.size());
        for (const auto& kv : g_buckets) { sorted.push_back(&kv); }
        std::sort(sorted.begin(), sorted.end(),
                  [](auto* a, auto* b) { return a->second.totalMs > b->second.totalMs; });

        for (const auto* kv : sorted)
        {
            const Bucket& b = kv->second;
            f.Printf("  %-28s %6lld items, %10.1f ms total, %8.2f ms avg\n",
                         kv->first.c_str(), b.count, b.totalMs,
                         b.count ? b.totalMs / static_cast<double>(b.count) : 0.0);
            for (const WorstItem& w : b.worst)
            {
                if (w.ms <= 0.0) { continue; }
                f.Printf("        %9.1f ms  %s\n", w.ms, w.item.c_str());
            }
        }
    }

    if (!g_counts.empty())
    {
        f.Printf("  ---- counters ----\n");
        std::vector<const std::pair<const std::string, long long>*> sorted;
        sorted.reserve(g_counts.size());
        for (const auto& kv : g_counts) { sorted.push_back(&kv); }
        std::sort(sorted.begin(), sorted.end(),
                  [](auto* a, auto* b) { return a->first < b->first; });
        for (const auto* kv : sorted)
        {
            f.Printf("  %-28s %lld\n", kv->first.c_str(), kv->second);
        }
    }

    logging::WriteRawLines(logging::LogLevel::Info, logging::LogCategory::Profiling, summary);
    logging::WriteRawLines(logging::LogLevel::Debug, logging::LogCategory::Profiling, timeline);
}

} // namespace boot
