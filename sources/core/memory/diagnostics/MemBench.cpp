#include "core/memory/diagnostics/MemBench.h"

#include "core/logging/Log.h"
#include "core/memory/MemCounters.h"

#include <mimalloc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

namespace
{
    // A mix of the sizes a frame actually asks for (small containers, strings, nodes) with a tail
    // of larger blocks. Fixed, so every round allocates the same sequence.
    constexpr std::size_t kSizes[] = { 16, 24, 32, 48, 64, 96, 128, 256, 32, 16, 64, 512, 24, 40, 2048, 80 };
    constexpr int kBatch = 256;       // live pointers per batch: allocate all, then free all
    constexpr int kBatches = 40000;   // ~10.2 M new/delete pairs per round

    struct Counted
    {
        static void* Alloc(std::size_t n) { return ::operator new(n); }
        static void Free(void* p, std::size_t n) { ::operator delete(p, n); }
    };
    struct Raw
    {
        static void* Alloc(std::size_t n) { return mi_new(n); }
        static void Free(void* p, std::size_t n) { mi_free_size(p, n); }
    };

    // Nanoseconds per new+delete pair for one thread running the fixed sequence.
    template <class Api>
    double RunOne()
    {
        void* ptrs[kBatch];
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < kBatches; ++b)
        {
            for (int i = 0; i < kBatch; ++i)
            {
                const std::size_t n = kSizes[(b + i) % std::size(kSizes)];
                ptrs[i] = Api::Alloc(n);
                static_cast<unsigned char*>(ptrs[i])[0] = static_cast<unsigned char>(i);
            }
            for (int i = 0; i < kBatch; ++i)
            {
                Api::Free(ptrs[i], kSizes[(b + i) % std::size(kSizes)]);
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        return ns / (static_cast<double>(kBatches) * kBatch);
    }

    // The same on `threads` threads started together; the slowest thread's figure.
    template <class Api>
    double RunMany(int threads)
    {
        std::vector<double> perThread(static_cast<std::size_t>(threads), 0.0);
        std::atomic<int> ready{ 0 };
        std::atomic<bool> go{ false };
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));
        for (int t = 0; t < threads; ++t)
        {
            pool.emplace_back([&, t]() {
                ready.fetch_add(1);
                while (!go.load()) { std::this_thread::yield(); }
                perThread[static_cast<std::size_t>(t)] = RunOne<Api>();
            });
        }
        while (ready.load() < threads) { std::this_thread::yield(); }
        go.store(true);
        for (std::thread& th : pool) { th.join(); }
        return *std::max_element(perThread.begin(), perThread.end());
    }
}

int RunMemBench(const char* commandLine)
{
    (void)commandLine;
    const unsigned hw = std::thread::hardware_concurrency();
    const int threads = static_cast<int>(std::clamp(hw / 2u, 2u, 8u));
    constexpr int kRounds = 3;

    // With --memtrack the counted path also tags every pointer (M3): this bench then prices the map.
    LOG_INFO(logging::LogCategory::Core,
             "mem bench: {} rounds A/B alternating, {} pairs per round, sizes 16..2048 B, then {} threads; memtrack {}",
             kRounds, kBatches * kBatch, threads, mem::MemTrackEnabled() ? "ON" : "off");

    // Warm both paths once: page faults and mimalloc's first segments are not what is measured.
    (void)RunOne<Counted>();
    (void)RunOne<Raw>();

    double counted = 1e30, raw = 1e30;
    for (int r = 0; r < kRounds; ++r)
    {
        const double a = RunOne<Counted>();
        const double b = RunOne<Raw>();
        LOG_INFO(logging::LogCategory::Core, "mem bench: round {} 1 thread: counted {:.2f} ns/pair, raw {:.2f} ns/pair",
                 r, a, b);
        counted = std::min(counted, a);
        raw = std::min(raw, b);
    }
    double countedMt = 1e30, rawMt = 1e30;
    for (int r = 0; r < kRounds; ++r)
    {
        const double a = RunMany<Counted>(threads);
        const double b = RunMany<Raw>(threads);
        LOG_INFO(logging::LogCategory::Core, "mem bench: round {} {} threads: counted {:.2f} ns/pair, raw {:.2f} ns/pair (slowest thread)",
                 r, threads, a, b);
        countedMt = std::min(countedMt, a);
        rawMt = std::min(rawMt, b);
    }
    LOG_INFO(logging::LogCategory::Core,
             "mem bench: best 1 thread counted {:.2f} raw {:.2f} -> counters {:.2f} ns/pair; best {} threads counted {:.2f} raw {:.2f} -> counters {:.2f} ns/pair",
             counted, raw, counted - raw, threads, countedMt, rawMt, countedMt - rawMt);
    return 0;
}
