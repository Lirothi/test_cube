#include "rendering/core/RendererSubmissionStress.h"
#include "rendering/core/SubmitTimeline.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Returned by a death-test child that survived the violation it was asked to
// commit; any other non-zero exit (abort) is the passing outcome.
constexpr int kSurvivedExitCode = 100;

constexpr const char* kFlagNullCl = "--stress-null-cl";
constexpr const char* kFlagInvalidBatch = "--stress-invalid-batch";
constexpr const char* kFlagDuplicateCl = "--stress-duplicate-cl";

FILE* gLog = nullptr;
int gFailures = 0;

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

// Fake command-list pointer values: the registration layer only stores and
// compares the pointers, it never dereferences them. Gathering executes
// bundles through the driver, so scenarios that gather keep their batches
// bundle-free.
ID3D12CommandList* FakeCL(size_t id)
{
    return reinterpret_cast<ID3D12CommandList*>((id + 1) << 4);
}

ID3D12GraphicsCommandList* FakeGfxCL(size_t id)
{
    return reinterpret_cast<ID3D12GraphicsCommandList*>(((id + 1) << 4) + 8);
}

// Fallback driver for gathering scenarios where no batch should need one
// (it is only requested for batches that hold bundles).
int gUnexpectedFallbacks = 0;
ID3D12GraphicsCommandList* UnexpectedFallback()
{
    ++gUnexpectedFallbacks;
    return FakeGfxCL(0xDEAD);
}

// Registration-order retention across batches, including a batch far beyond
// the 8-entry inline capacity of the container this layer replaced (which
// went out of bounds in Release).
void ScenarioOrderRetention(int round)
{
    SubmitTimeline tl;
    std::vector<ID3D12CommandList*> expected;
    size_t id = static_cast<size_t>(round) * 1000;

    tl.BeginTimeline();
    const size_t bDriverOnly = tl.BeginBatch();
    const size_t bMixed = tl.BeginBatch();
    const size_t bBig = tl.BeginBatch();
    const size_t bEmpty = tl.BeginBatch();
    const size_t bSmall = tl.BeginBatch();
    (void)bEmpty; // an activated batch nothing registers into must gather to nothing

    ID3D12GraphicsCommandList* driverOnly = FakeGfxCL(id++);
    tl.RegisterDriver(driverOnly, bDriverOnly);
    expected.push_back(driverOnly);

    ID3D12GraphicsCommandList* mixedDriver = FakeGfxCL(id++);
    tl.RegisterDriver(mixedDriver, bMixed);
    expected.push_back(mixedDriver); // driver gathers before the batch's directs
    for (int i = 0; i < 3; ++i) {
        ID3D12CommandList* cl = FakeCL(id++);
        tl.RegisterDirect(cl, bMixed);
        expected.push_back(cl);
    }

    for (int i = 0; i < 40; ++i) {
        ID3D12CommandList* cl = FakeCL(id++);
        tl.RegisterDirect(cl, bBig);
        expected.push_back(cl);
    }

    for (int i = 0; i < 11; ++i) {
        ID3D12CommandList* cl = FakeCL(id++);
        tl.RegisterDirect(cl, bSmall);
        expected.push_back(cl);
    }

    std::vector<ID3D12CommandList*> out;
    tl.GatherFrameLists(out, &UnexpectedFallback);

    const bool ok = (out == expected) && tl.ActiveBatchCount() == 0;
    char what[128];
    std::snprintf(what, sizeof(what), "order retention round=%d (%zu lists, one batch >8)", round, expected.size());
    Check(ok, what);
}

// Concurrent registration from many threads, one batch per thread (matching
// the engine: one pass's chunk jobs all land in that pass's batch, so order
// within a batch is the registering thread's program order). Later batches
// are activated while earlier threads are still registering, like the
// render-graph build thread does.
void ScenarioConcurrentRegistration(unsigned threadCount, int clsPerThread, int frames)
{
    SubmitTimeline tl;
    bool ok = true;

    for (int f = 0; f < frames && ok; ++f) {
        tl.BeginTimeline();

        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (unsigned t = 0; t < threadCount; ++t) {
            const size_t batch = tl.BeginBatch(); // interleaves with running threads' registrations
            const size_t base = (static_cast<size_t>(f) * threadCount + t) * clsPerThread;
            threads.emplace_back([&tl, batch, base, clsPerThread]() {
                for (int i = 0; i < clsPerThread; ++i) {
                    tl.RegisterDirect(FakeCL(base + i), batch);
                }
            });
        }
        for (auto& th : threads) {
            th.join();
        }

        std::vector<ID3D12CommandList*> expected;
        expected.reserve(static_cast<size_t>(threadCount) * clsPerThread);
        for (unsigned t = 0; t < threadCount; ++t) {
            const size_t base = (static_cast<size_t>(f) * threadCount + t) * clsPerThread;
            for (int i = 0; i < clsPerThread; ++i) {
                expected.push_back(FakeCL(base + i));
            }
        }

        std::vector<ID3D12CommandList*> out;
        tl.GatherFrameLists(out, &UnexpectedFallback);
        ok = (out == expected);
    }

    char what[128];
    std::snprintf(what, sizeof(what), "concurrent registration threads=%u cls=%d frames=%d", threadCount, clsPerThread, frames);
    Check(ok, what);
}

// Persistent-pool reuse across frames with widely varying batch counts and
// per-batch list counts: stale pooled slots left over from a big frame must
// never leak into a later small frame's gather.
void ScenarioPoolReuse(int frames)
{
    SubmitTimeline tl;
    size_t id = 0;
    bool ok = true;
    std::vector<ID3D12CommandList*> expected;
    std::vector<ID3D12CommandList*> out;

    for (int f = 0; f < frames && ok; ++f) {
        tl.BeginTimeline();
        expected.clear();
        const size_t batchCount = 1 + (static_cast<size_t>(f) % 23);
        for (size_t b = 0; b < batchCount; ++b) {
            const size_t batch = tl.BeginBatch();
            const size_t directCount = (static_cast<size_t>(f) + b * 7) % 13; // 0..12, regularly past 8
            for (size_t i = 0; i < directCount; ++i) {
                ID3D12CommandList* cl = FakeCL(id++);
                tl.RegisterDirect(cl, batch);
                expected.push_back(cl);
            }
        }
        out.clear();
        tl.GatherFrameLists(out, &UnexpectedFallback);
        ok = (out == expected) && tl.ActiveBatchCount() == 0;
    }

    char what[128];
    std::snprintf(what, sizeof(what), "pool reuse frames=%d (batch counts 1..23)", frames);
    Check(ok, what);
}

// Bundle registration past the old inline capacity, verified by inspection
// (gathering would ExecuteBundle through the fake driver pointer), plus the
// pooled slot coming back clean after the next BeginTimeline.
void ScenarioBundleRetention()
{
    SubmitTimeline tl;
    tl.BeginTimeline();
    const size_t batch = tl.BeginBatch();

    ID3D12GraphicsCommandList* driver = FakeGfxCL(1);
    tl.RegisterDriver(driver, batch);
    std::vector<ID3D12GraphicsCommandList*> expected;
    for (size_t i = 0; i < 12; ++i) {
        ID3D12GraphicsCommandList* bundle = FakeGfxCL(100 + i);
        tl.RegisterBundle(bundle, batch);
        expected.push_back(bundle);
    }

    const SubmitTimeline::PassBatch& pb = tl.Batch(batch);
    bool ok = pb.driver == driver && pb.bundles == expected && pb.directs.empty();

    tl.BeginTimeline(); // reset without gathering
    ok = ok && tl.ActiveBatchCount() == 0;
    const size_t again = tl.BeginBatch();
    const SubmitTimeline::PassBatch& pb2 = tl.Batch(again);
    ok = ok && again == batch && pb2.driver == nullptr && pb2.bundles.empty() && pb2.directs.empty();

    Check(ok, "bundle retention >8 and pooled slot reset");
}

// Child side of a death test: commit one invariant violation; the process is
// EXPECTED to abort inside RendererInvariantFailure before reaching the end.
int RunDeathTestChild(const char* flag)
{
    // Abort quietly: no "abort() has been called" message box, no WER fault
    // report — the parent only inspects the exit code.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    SubmitTimeline tl;
    tl.BeginTimeline();

    if (std::strcmp(flag, kFlagNullCl) == 0) {
        const size_t batch = tl.BeginBatch();
        tl.RegisterDirect(nullptr, batch); // must abort
    }
    else if (std::strcmp(flag, kFlagInvalidBatch) == 0) {
        // Grow the pool to 4 slots in a "previous frame", then activate only
        // one: index 2 is inside the POOL but stale this frame — registering
        // into it would be a silently lost CL.
        for (int i = 0; i < 4; ++i) {
            tl.BeginBatch();
        }
        tl.BeginTimeline();
        tl.BeginBatch();
        tl.RegisterDirect(FakeCL(1), 2); // must abort
    }
    else if (std::strcmp(flag, kFlagDuplicateCl) == 0) {
        const size_t batch = tl.BeginBatch();
        tl.RegisterDirect(FakeCL(1), batch);
        tl.RegisterDirect(FakeCL(1), batch); // must abort
    }

    return kSurvivedExitCode; // not reached when the fail-fast works
}

// Parent side: spawn this executable with the death-test flag and require an
// abnormal exit.
bool SpawnDeathTest(const char* flag)
{
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
        Log("death test %s: GetModuleFileName failed (%lu)\n", flag, GetLastError());
        return false;
    }

    std::string cmd = "\"";
    cmd += exePath;
    cmd += "\" --renderer-submission-stress ";
    cmd += flag;
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log("death test %s: CreateProcess failed (%lu)\n", flag, GetLastError());
        return false;
    }

    bool ok = false;
    const DWORD wait = WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    if (wait == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code)) {
        ok = (code != 0) && (code != static_cast<DWORD>(kSurvivedExitCode));
        Log("death test %s: exit code %lu (%s)\n", flag, code, ok ? "aborted as expected" : "DID NOT ABORT");
    }
    else {
        TerminateProcess(pi.hProcess, 1);
        Log("death test %s: child did not finish (wait %lu)\n", flag, wait);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

} // namespace

int RunRendererSubmissionStress(const char* cmdLine)
{
    const char* deathFlags[] = { kFlagNullCl, kFlagInvalidBatch, kFlagDuplicateCl };
    if (cmdLine != nullptr) {
        for (const char* flag : deathFlags) {
            if (std::strstr(cmdLine, flag) != nullptr) {
                return RunDeathTestChild(flag);
            }
        }
    }

    gLog = nullptr;
    fopen_s(&gLog, "renderer_submission_stress.log", "w");
    gFailures = 0;
    gUnexpectedFallbacks = 0;

    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const auto t0 = std::chrono::steady_clock::now();
    Log("renderer submission stress, hardware threads: %u\n", hw);

    constexpr int kRounds = 25;
    for (int round = 0; round < kRounds; ++round) {
        Log("--- round %d/%d ---\n", round + 1, kRounds);

        ScenarioOrderRetention(round);
        ScenarioConcurrentRegistration(hw, 64, 40);
        ScenarioConcurrentRegistration(2, 256, 20);
        ScenarioPoolReuse(200);
        ScenarioBundleRetention();

        if (gFailures != 0) {
            Log("aborting after round %d: %d failures\n", round + 1, gFailures);
            break;
        }
    }

    Check(gUnexpectedFallbacks == 0, "no unexpected fallback drivers requested");

    if (gFailures == 0) {
        for (const char* flag : deathFlags) {
            Check(SpawnDeathTest(flag), flag);
        }
    }

    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Log("done in %.1fs, failures: %d\n", seconds, gFailures);
    if (gLog) {
        fclose(gLog);
        gLog = nullptr;
    }
    return gFailures;
}
