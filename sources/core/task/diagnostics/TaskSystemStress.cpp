#include "core/task/diagnostics/TaskSystemStress.h"
#include "core/diagnostics/DiagPaths.h"
#include "core/task/TaskSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace {

FILE* gLog = nullptr;
int gFailures = 0;

// Cumulative per-scenario wall time across all rounds, for benchmarking
// contention changes (totals are less noisy than the overall run time, which
// is dominated by Start/Stop thread spawning).
double gChurnSeconds = 0.0;
double gNestedSeconds = 0.0;
double gFanOutSeconds = 0.0;
double gStartStopSeconds = 0.0;

struct ScopedSeconds {
    explicit ScopedSeconds(double& target)
        : target_(target), start_(std::chrono::steady_clock::now()) {}
    ~ScopedSeconds()
    {
        target_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    double& target_;
    std::chrono::steady_clock::time_point start_;
};

void Log(const char* fmt, ...)
{
    if (!gLog) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(gLog, fmt, args);
    va_end(args);
    fflush(gLog);
}

void Check(bool ok, const char* what)
{
    if (!ok) {
        ++gFailures;
    }
    Log("%s: %s\n", ok ? "ok  " : "FAIL", what);
}

// Concurrent create/submit/recycle churn from external threads. Exercises the
// tagged freelists (acquire/recycle race from many threads), detached lambda
// tasks, range tasks, and Wait() on non-worker threads.
void ScenarioConcurrentChurn(unsigned workerCount, unsigned externalThreads, int iterations)
{
    ScopedSeconds timing(gChurnSeconds);
    auto& ts = TaskSystem::Get();
    ts.Start(workerCount);

    std::atomic<long long> sum{0};
    std::vector<std::thread> threads;
    threads.reserve(externalThreads);
    for (unsigned t = 0; t < externalThreads; ++t) {
        threads.emplace_back([&ts, &sum, iterations]() {
            for (int i = 0; i < iterations; ++i) {
                ts.SubmitDetach([&sum]() { sum.fetch_add(1, std::memory_order_relaxed); });
                ts.DispatchWait(4, [&sum](std::size_t) { sum.fetch_add(1, std::memory_order_relaxed); }, 1);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    ts.Stop(); // WaitForAll inside — detached tasks must all land

    const long long expected = static_cast<long long>(externalThreads) * iterations * (1 + 4);
    char what[128];
    std::snprintf(what, sizeof(what), "churn workers=%u threads=%u iters=%d (sum %lld == %lld)",
        workerCount, externalThreads, iterations, sum.load(), expected);
    Check(sum.load() == expected, what);
}

// Range tasks dispatched from inside range tasks: the inner DispatchWait runs
// Wait() in worker context (the never-park path).
void ScenarioNestedWaits(unsigned workerCount, int iterations)
{
    ScopedSeconds timing(gNestedSeconds);
    auto& ts = TaskSystem::Get();
    ts.Start(workerCount);

    std::atomic<long long> sum{0};
    for (int it = 0; it < iterations; ++it) {
        ts.DispatchWait(8, [&sum](std::size_t) {
            TaskSystem::Get().DispatchWait(16, [&sum](std::size_t) {
                sum.fetch_add(1, std::memory_order_relaxed);
            }, 1);
        }, 1);
    }
    ts.Stop();

    const long long expected = static_cast<long long>(iterations) * 8 * 16;
    char what[128];
    std::snprintf(what, sizeof(what), "nested waits workers=%u iters=%d (sum %lld == %lld)",
        workerCount, iterations, sum.load(), expected);
    Check(sum.load() == expected, what);
}

// One producer with dependents_ filled exactly to capacity (4). Validates the
// dependency protocol (deps run strictly after the producer) and that the
// capacity boundary itself does not trip the fail-fast.
void ScenarioFanOutAtCapacity(unsigned workerCount, int iterations)
{
    ScopedSeconds timing(gFanOutSeconds);
    auto& ts = TaskSystem::Get();
    ts.Start(workerCount);

    int violations = 0;
    int missing = 0;
    for (int it = 0; it < iterations; ++it) {
        std::atomic<int> producerDone{0};
        std::atomic<int> orderErrors{0};
        std::atomic<int> ran{0};

        TaskSystem::TaskHandle producer = ts.CreateTask([&producerDone]() {
            producerDone.store(1, std::memory_order_release);
        });

        TaskSystem::TaskHandle dependents[4] = {};
        for (int i = 0; i < 4; ++i) {
            dependents[i] = ts.CreateTask([&producerDone, &orderErrors, &ran]() {
                if (producerDone.load(std::memory_order_acquire) == 0) {
                    orderErrors.fetch_add(1, std::memory_order_relaxed);
                }
                ran.fetch_add(1, std::memory_order_relaxed);
            }, 1);
            ts.SetDependencies(dependents[i], &producer, 1);
        }

        ts.Submit(producer);
        for (int i = 0; i < 4; ++i) {
            ts.Wait(dependents[i]);
            ts.Release(dependents[i]);
        }
        ts.Release(producer);

        violations += orderErrors.load();
        if (ran.load() != 4) {
            ++missing;
        }
    }
    ts.Stop();

    char what[128];
    std::snprintf(what, sizeof(what), "fan-out at capacity workers=%u iters=%d (order errors %d, incomplete %d)",
        workerCount, iterations, violations, missing);
    Check(violations == 0 && missing == 0, what);
}

// Repeated Start/Stop with in-flight detached and tracked work. Exercises the
// shutdown path (WaitForAll + sentinel drain) that the lost-wakeup fix touched.
void ScenarioStartStopCycles(unsigned workerCount, int cycles)
{
    ScopedSeconds timing(gStartStopSeconds);
    auto& ts = TaskSystem::Get();
    std::atomic<long long> sum{0};
    long long expected = 0;

    for (int c = 0; c < cycles; ++c) {
        ts.Start(workerCount);
        for (int i = 0; i < 32; ++i) {
            ts.SubmitDetach([&sum]() { sum.fetch_add(1, std::memory_order_relaxed); });
        }
        expected += 32;
        ts.DispatchTrack(16, [&sum](std::size_t) { sum.fetch_add(1, std::memory_order_relaxed); }, 1);
        expected += 16;
        ts.WaitForTrackedAsyncTasks();
        ts.Stop();
    }

    char what[128];
    std::snprintf(what, sizeof(what), "start/stop cycles workers=%u cycles=%d (sum %lld == %lld)",
        workerCount, cycles, sum.load(), expected);
    Check(sum.load() == expected, what);
}

// Death test: a fifth dependent must abort the process (fail-fast instead of
// out-of-bounds write / silent hang). The harness runner treats abnormal exit
// as the PASSING outcome for this mode.
void RunOverflowDeathTest()
{
#ifdef _DEBUG
    // The standard assert macro in a GUI app pops a message box and blocks; route
    // it to stderr instead (_set_error_mode governs assert; _CrtSetReportMode
    // only governs _CrtDbgReport-based macros). Both are set so the intentional
    // failure aborts immediately instead of waiting on a dialog.
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    auto& ts = TaskSystem::Get();
    ts.Start(2);

    TaskSystem::TaskHandle producer = ts.CreateTask([]() {});
    TaskSystem::TaskHandle dependents[5] = {};
    Log("overflow death test: registering 5 dependents (capacity 4), expecting abort...\n");
    for (int i = 0; i < 5; ++i) {
        dependents[i] = ts.CreateTask([]() {}, 1);
        ts.SetDependencies(dependents[i], &producer, 1); // 5th call must abort
    }

    // Not reached when the fail-fast works.
    Log("FAIL: overflow death test survived 5 dependents\n");
}

} // namespace

int RunTaskSystemStress(bool overflowDeathTest)
{
    gLog = nullptr;
    fopen_s(&gLog, diag::LogPath("tasksystem_stress.log").c_str(), "w");
    gFailures = 0;

    if (overflowDeathTest) {
        RunOverflowDeathTest();
        if (gLog) {
            fclose(gLog);
        }
        return 100; // surviving the death test is a failure
    }

    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const auto t0 = std::chrono::steady_clock::now();
    Log("task system stress, hardware threads: %u\n", hw);

    // One round is a few seconds; races need wall-clock, so run many.
    constexpr int kRounds = 50;
    for (int round = 0; round < kRounds; ++round) {
        Log("--- round %d/%d ---\n", round + 1, kRounds);

        ScenarioConcurrentChurn(hw, 8, 4000);
        ScenarioConcurrentChurn(2, 8, 1500);
        ScenarioConcurrentChurn(1, 4, 1000);

        ScenarioNestedWaits(hw, 1500);
        ScenarioNestedWaits(2, 500);
        ScenarioNestedWaits(1, 250);

        ScenarioFanOutAtCapacity(hw, 2000);
        ScenarioFanOutAtCapacity(1, 500);

        ScenarioStartStopCycles(hw, 300);
        ScenarioStartStopCycles(1, 150);

        if (gFailures != 0) {
            Log("aborting after round %d: %d failures\n", round + 1, gFailures);
            break;
        }
    }

    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Log("scenario totals: churn %.2fs, nested %.2fs, fanout %.2fs, startstop %.2fs\n",
        gChurnSeconds, gNestedSeconds, gFanOutSeconds, gStartStopSeconds);
    Log("done in %.1fs, failures: %d\n", seconds, gFailures);
    if (gLog) {
        fclose(gLog);
        gLog = nullptr;
    }
    return gFailures;
}
