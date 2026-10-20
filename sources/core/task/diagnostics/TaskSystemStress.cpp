#include "core/task/diagnostics/TaskSystemStress.h"
#include "core/diagnostics/ArtifactWriter.h"
#include "core/task/TaskSystem.h"
#include "core/logging/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <stdexcept>
#include <unordered_set>
#include <thread>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace {

diag::ArtifactFile gLog;
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
    gLog.VPrintf(fmt, args);
    va_end(args);
}

void Check(bool ok, const char* what)
{
    if (!ok) {
        ++gFailures;
    }
    Log("%s: %s\n", ok ? "ok  " : "FAIL", what);
}

// Concurrent create/submit/recycle churn from external threads. Exercises the
// task caches (acquire/recycle race from many threads), detached lambda
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
// shutdown path (WaitForAll + queued-reference drain) that the lost-wakeup fix touched.
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

// Each deterministic regression has a deadline so a scheduling regression fails
// the gate instead of leaving a hung harness behind. Diagnostics use the session.
class ScenarioDeadline {
public:
    explicit ScenarioDeadline(const char* name) : thread_([this, name]() {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(30), [this] { return done_; })) {
            LOG_FATAL(logging::LogCategory::Task, "task regression timed out: {}", name);
            std::_Exit(98);
        }
    }) {}
    ~ScenarioDeadline()
    {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

void RegressionCheck(bool ok, const char* name)
{
    Check(ok, name); // retain the existing stress artifact's summary
    if (ok) {
        LOG_INFO(logging::LogCategory::Task, "task regression PASS: {}", name);
    } else {
        LOG_ERROR(logging::LogCategory::Task, "task regression FAIL: {}", name);
    }
}

void ScenarioSaturatedProducers(unsigned workerCount)
{
    ScenarioDeadline deadline("saturated producers");
    auto& ts = TaskSystem::Get();
    ts.Start(workerCount);
    constexpr std::size_t kChildren = 768;
    std::vector<std::atomic<unsigned>> hits(workerCount * kChildren);
    for (auto& hit : hits) { hit.store(0); }
    std::latch started(workerCount), go(1);
    for (unsigned producer = 0; producer < workerCount; ++producer) {
        ts.SubmitDetach([&, producer] {
            started.count_down();
            go.wait();
            // Every worker is a producer, and none can consume until its Submit
            // loop returns. Overflow must neither spin nor execute arbitrary work.
            for (std::size_t i = 0; i < kChildren; ++i) {
                ts.SubmitDetach([&, index = producer * kChildren + i] { ++hits[index]; });
            }
        });
    }
    started.wait();
    go.count_down();
    ts.WaitForAll(); // deliberately no helping from the main thread
    ts.Stop();
    const bool once = std::all_of(hits.begin(), hits.end(), [](const auto& h) { return h.load() == 1; });
    RegressionCheck(once, workerCount == 1 ? "single-worker queue saturation" : "all-worker queue saturation");
}

void ScenarioLargeRange()
{
    ScenarioDeadline deadline("large range");
    auto& ts = TaskSystem::Get();
    ts.Start(1);
    std::atomic<unsigned> ran{0};
    auto range = ts.Dispatch(300, [&](std::size_t) { ++ran; }, 1);
    ts.WaitForAll();
    ts.Release(range);
    ts.Stop();
    RegressionCheck(ran.load() == 300, "range larger than ready queue");
}

void ScenarioRangeExceptions()
{
    ScenarioDeadline deadline("range exceptions");
    auto& ts = TaskSystem::Get();
    ts.Start(2);
    LOG_INFO(logging::LogCategory::Task, "task regression: expecting two intentional callback exceptions");
    std::atomic<unsigned> ran{0};
    ts.DispatchWait(16, [&](std::size_t i) {
        if (i == 7) { throw std::runtime_error("task regression: intentional child exception"); }
        ++ran;
    }, 1);
    RegressionCheck(ran.load() == 15, "throwing child still completes and joins");

    std::latch first(1), releaseChild(1);
    std::atomic<bool> childDone{false};
    std::thread release([&] {
        first.wait();
        // Hold the child beyond the throw so an early completion is observable.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        releaseChild.count_down();
    });
    ts.DispatchWait(2, [&](std::size_t i) {
        if (i == 0) {
            first.count_down();
            throw std::runtime_error("task regression: intentional first-chunk exception");
        }
        releaseChild.wait();
        childDone.store(true);
    }, 1);
    RegressionCheck(childDone.load(), "first-chunk exception joins outstanding children");
    release.join();
    ts.Stop();
}

void ScenarioNestedTracking()
{
    ScenarioDeadline deadline("nested tracking");
    auto& ts = TaskSystem::Get();
    ts.Start(1);
    std::latch started(1), go(1);
    std::atomic<unsigned> ran{0};
    auto parent = ts.Submit([&] {
        started.count_down();
        go.wait();
        ts.DispatchTrack(8, [&](std::size_t) {
            ts.DispatchTrack(4, [&](std::size_t) { ++ran; }, 1);
        }, 1);
    });
    started.wait();
    ts.TrackFrameTask(parent);
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        go.count_down();
    });
    ts.WaitForTrackedAsyncTasks();
    RegressionCheck(ran.load() == 32, "tracked wait drains recursively registered work");
    release.join();
    ts.Stop();
}

void ScenarioTargetedWait()
{
    ScenarioDeadline deadline("targeted wait");
    auto& ts = TaskSystem::Get();
    ts.Start(1);
    std::latch started(1), go(1);
    std::atomic<unsigned> ran{0};
    auto a = ts.CreateTask([&] {
        started.count_down();
        go.wait();
        ts.DispatchWait(1, [&](std::size_t) { ++ran; }, 1);
    });
    auto b = ts.CreateTask([&] { ++ran; });
    ts.SetDependencies(b, &a, 1);
    auto c = ts.CreateTask([&] { ts.Wait(b); ++ran; });
    ts.Submit(a);
    started.wait();
    ts.Submit(c); // FIFO places unrelated C before A's newly-created child
    go.count_down();
    ts.WaitForAll();
    RegressionCheck(ran.load() == 3, "nested wait never executes a dependent of its ancestor");
    ts.Release(a); ts.Release(b); ts.Release(c);
    ts.Stop();
}

void ScenarioPrerequisiteLifetime()
{
    ScenarioDeadline deadline("prerequisite lifetime");
    auto& ts = TaskSystem::Get();
    ts.Start(1);
    std::latch started(1), go(1);
    ts.SubmitDetach([&] { started.count_down(); go.wait(); });
    started.wait(); // all progress below must come from targeted main-thread help
    std::atomic<unsigned> ran{0};
    auto root = ts.CreateTask([&] { ++ran; });
    auto middle = ts.CreateTask([&] { ++ran; });
    auto leaf = ts.CreateTask([&] { ++ran; });
    ts.SetDependencies(middle, &root, 1);
    ts.SetDependencies(leaf, &middle, 1);
    ts.Submit(root);
    ts.Release(root); ts.Release(middle);
    ts.Wait(leaf);
    RegressionCheck(ran.load() == 3, "targeted help retains and traverses prerequisites");
    ts.Release(leaf);
    go.count_down();
    ts.Stop(); // must release the already-completed entries still in the ring
}

void ScenarioPoolOwnership()
{
    ScenarioDeadline deadline("pool ownership");
    auto& ts = TaskSystem::Get();
    std::unordered_set<TaskSystem::TaskHandle> live;
    std::mutex liveMutex;
    std::atomic<bool> duplicate{false};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (unsigned i = 0; i < 70000; ++i) {
                TaskSystem::TaskHandle handles[] = {
                    ts.CreateTask([] {}), ts.CreateTask([] {}),
                    ts.CreateRangeTask(1, [](std::size_t) {}), ts.CreateRangeTask(1, [](std::size_t) {})
                };
                {
                    std::lock_guard lock(liveMutex);
                    for (auto handle : handles) {
                        if (!live.insert(handle).second) { duplicate.store(true); }
                    }
                }
                std::this_thread::yield();
                {
                    std::lock_guard lock(liveMutex);
                    for (auto handle : handles) { live.erase(handle); }
                }
                for (auto& handle : handles) { ts.Release(handle); }
            }
        });
    }
    for (auto& thread : threads) { thread.join(); }
    RegressionCheck(!duplicate.load(), "concurrent pool ownership beyond 65536 reuse cycles");
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
    gLog.Open("tasksystem_stress.log", diag::ArtifactMode::PerRunTruncate);
    gFailures = 0;

    if (overflowDeathTest) {
        RunOverflowDeathTest();
        gLog.Close();
        return 100; // surviving the death test is a failure
    }

    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const auto t0 = std::chrono::steady_clock::now();
    Log("task system stress, hardware threads: %u\n", hw);

    ScenarioSaturatedProducers(1);
    ScenarioSaturatedProducers(4);
    ScenarioLargeRange();
    ScenarioRangeExceptions();
    ScenarioNestedTracking();
    ScenarioTargetedWait();
    ScenarioPrerequisiteLifetime();
    ScenarioPoolOwnership();

    // One round is a few seconds; races need wall-clock, so run many.
    constexpr int kRounds = 50;
    for (int round = 0; round < kRounds; ++round) {
        ScenarioDeadline deadline("stress round");
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
    LOG_INFO(logging::LogCategory::Task, "task system stress: {} ({} failures)",
        gFailures == 0 ? "PASS" : "FAIL", gFailures);
    gLog.Close();
    return gFailures;
}
