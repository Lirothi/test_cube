#include "rendering/diagnostics/RendererSubmissionStress.h"
#include "core/diagnostics/ArtifactWriter.h"
#include "rendering/core/SubmitTimeline.h"
#include "rendering/core/BarrierTranslation.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
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
constexpr const char* kFlagDupOrderDirect = "--stress-dup-order-direct";
constexpr const char* kFlagDupOrderBundle = "--stress-dup-order-bundle";

diag::ArtifactFile gLog;
int gFailures = 0;

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

// Async-compute step 6: the timeline now returns per-queue SEGMENTS instead of one flat array.
// These tests are about ORDER RETENTION within a queue, so they flatten the segments back — which
// is the identity transform while every batch is Graphics, i.e. exactly what they used to receive.
// Flattening lives HERE and not in SubmitTimeline on purpose: merging two queues' arrays is
// meaningless for submission, and a production helper that did it would be a trap.
void GatherFlat(SubmitTimeline& tl, std::vector<ID3D12CommandList*>& out)
{
    std::vector<SubmitTimeline::Submission> segments;
    tl.GatherFrameLists(segments, &UnexpectedFallback);
    out.clear();
    for (const auto& seg : segments) {
        out.insert(out.end(), seg.lists.begin(), seg.lists.end());
    }
}

// Registration retention + localOrder sort across batches, including a batch
// far beyond the 8-entry inline capacity of the container this layer replaced
// (which went out of bounds in Release). Directs only: gather now closes
// drivers (a real D3D12 Close that fake pointers can't survive), so driver
// placement is covered separately by ScenarioDriverOrdering via inspection.
void ScenarioOrderRetention(int round)
{
    SubmitTimeline tl;
    std::vector<ID3D12CommandList*> expected;
    size_t id = static_cast<size_t>(round) * 1000;

    tl.BeginTimeline();
    const size_t bMixed = tl.BeginBatch();
    const size_t bBig = tl.BeginBatch();
    const size_t bEmpty = tl.BeginBatch();
    const size_t bSmall = tl.BeginBatch();
    (void)bEmpty; // an activated batch nothing registers into must gather to nothing

    for (int i = 0; i < 3; ++i) {
        ID3D12CommandList* cl = FakeCL(id++);
        tl.RegisterDirect(cl, bMixed, static_cast<uint32_t>(i));
        expected.push_back(cl);
    }

    for (int i = 0; i < 40; ++i) {
        ID3D12CommandList* cl = FakeCL(id++);
        tl.RegisterDirect(cl, bBig, static_cast<uint32_t>(i));
        expected.push_back(cl);
    }

    for (int i = 0; i < 11; ++i) {
        ID3D12CommandList* cl = FakeCL(id++);
        tl.RegisterDirect(cl, bSmall, static_cast<uint32_t>(i));
        expected.push_back(cl);
    }

    std::vector<ID3D12CommandList*> out;
    GatherFlat(tl, out);

    const bool ok = (out == expected) && tl.ActiveBatchCount() == 0;
    char what[128];
    std::snprintf(what, sizeof(what), "order retention round=%d (%zu lists, one batch >8)", round, expected.size());
    Check(ok, what);
}

// Driver placement + direct sort without gathering: gather closes the driver
// (real D3D12 Close) which a fake pointer can't survive, so this inspects the
// batch directly. Directs are registered in reverse localOrder; after
// ApplySubmitOrder the batch holds the driver plus directs sorted ascending,
// which gather then flattens driver-first by construction.
void ScenarioDriverOrdering()
{
    SubmitTimeline tl;
    tl.BeginTimeline();
    const size_t batch = tl.BeginBatch();

    ID3D12GraphicsCommandList* driver = FakeGfxCL(900000);
    tl.RegisterDriver(driver, batch);
    for (int i = 4; i >= 0; --i) {
        tl.RegisterDirect(FakeCL(900100 + i), batch, static_cast<uint32_t>(i));
    }
    tl.ApplySubmitOrder();

    const SubmitTimeline::PassBatch& pb = tl.Batch(batch);
    bool ok = pb.driver == driver && pb.directs.size() == 5 && pb.bundles.empty();
    for (int i = 0; i < 5 && ok; ++i) {
        ok = pb.directs[i].order == static_cast<uint32_t>(i) &&
             pb.directs[i].cl == FakeCL(900100 + i);
    }
    Check(ok, "driver present + directs reverse-order sorted (driver gathers first)");
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
                    tl.RegisterDirect(FakeCL(base + i), batch, static_cast<uint32_t>(i));
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
        GatherFlat(tl, out);
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
                tl.RegisterDirect(cl, batch, static_cast<uint32_t>(i));
                expected.push_back(cl);
            }
        }
        out.clear();
        GatherFlat(tl, out);
        ok = (out == expected) && tl.ActiveBatchCount() == 0;
    }

    char what[128];
    std::snprintf(what, sizeof(what), "pool reuse frames=%d (batch counts 1..23)", frames);
    Check(ok, what);
}

// Bundle registration past the old inline capacity, registered in REVERSE
// localOrder so the gather-time sort is exercised (gathering can't run here —
// ExecuteBundle would dereference the fake driver pointer — so ApplySubmitOrder
// is invoked directly and the batch inspected). Also checks the pooled slot
// comes back clean after the next BeginTimeline.
void ScenarioBundleRetention()
{
    SubmitTimeline tl;
    tl.BeginTimeline();
    const size_t batch = tl.BeginBatch();

    ID3D12GraphicsCommandList* driver = FakeGfxCL(1);
    tl.RegisterDriver(driver, batch);

    constexpr size_t kBundles = 12;
    for (size_t i = 0; i < kBundles; ++i) {
        // Register order i as the (kBundles-1-i)-th call: registration order is
        // the reverse of localOrder, so only the sort can recover it.
        const uint32_t order = static_cast<uint32_t>(kBundles - 1 - i);
        tl.RegisterBundle(FakeGfxCL(100 + order), batch, order);
    }

    tl.ApplySubmitOrder();
    const SubmitTimeline::PassBatch& pb = tl.Batch(batch);
    bool ok = pb.driver == driver && pb.bundles.size() == kBundles && pb.directs.empty();
    for (size_t i = 0; i < kBundles && ok; ++i) {
        ok = pb.bundles[i].order == static_cast<uint32_t>(i) &&
             pb.bundles[i].cl == FakeGfxCL(100 + i);
    }

    tl.BeginTimeline(); // reset without gathering
    ok = ok && tl.ActiveBatchCount() == 0;
    const size_t again = tl.BeginBatch();
    const SubmitTimeline::PassBatch& pb2 = tl.Batch(again);
    ok = ok && again == batch && pb2.driver == nullptr && pb2.bundles.empty() && pb2.directs.empty();

    Check(ok, "bundle retention >8, reverse-order sort, pooled slot reset");
}

// Deterministic GPU order: register directs whose localOrder is a fixed
// canonical mapping (order i -> CL i), but whose REGISTRATION sequence is
// shuffled differently each run — simulating workers finishing in arbitrary
// order. Gather must always reproduce the localOrder-sorted sequence, byte-for-
// byte identical across every shuffle.
void ScenarioDeterministicOrder(int permutations)
{
    constexpr int N = 64;
    std::vector<ID3D12CommandList*> canonical;
    canonical.reserve(N);
    for (int i = 0; i < N; ++i) {
        canonical.push_back(FakeCL(600000 + i)); // localOrder i maps to this CL
    }

    bool ok = true;
    std::vector<ID3D12CommandList*> firstOut;
    for (int p = 0; p < permutations && ok; ++p) {
        std::vector<int> seq(N);
        std::iota(seq.begin(), seq.end(), 0);
        std::mt19937 rng(static_cast<unsigned>(p) * 2654435761u + 12345u);
        std::shuffle(seq.begin(), seq.end(), rng);

        SubmitTimeline tl;
        tl.BeginTimeline();
        const size_t batch = tl.BeginBatch();
        for (int idx : seq) {
            tl.RegisterDirect(canonical[idx], batch, static_cast<uint32_t>(idx));
        }
        std::vector<ID3D12CommandList*> out;
        GatherFlat(tl, out);

        if (p == 0) {
            firstOut = out;
            ok = (out == canonical); // sorted output equals the canonical order
        }
        else {
            ok = (out == firstOut);  // identical regardless of registration order
        }
    }

    char what[128];
    std::snprintf(what, sizeof(what), "deterministic order: %d shuffled registrations -> identical sorted gather", permutations);
    Check(ok, what);
}

// --- Barrier translation (step 10) -------------------------------------------
//
// A table nothing calls yet is a table nobody notices is wrong. These assert design D4's mapping
// directly, plus the two cases the table's shape exists for: COMBINED read states (which the
// engine declares deliberately — VSM.PhysOwner rests in NON_PIXEL|COPY_SOURCE) and the
// buffer/texture split (a buffer has no layout at all).

void ScenarioBarrierTranslation()
{
    using namespace barriers;
    bool ok = true;
    const auto tex = [](D3D12_RESOURCE_STATES s) { return LegacyStateToBarrier(s, /*isBuffer=*/false); };
    const auto buf = [](D3D12_RESOURCE_STATES s) { return LegacyStateToBarrier(s, /*isBuffer=*/true); };

    // Single states: the D4 table, verbatim.
    {
        const Translated t = tex(D3D12_RESOURCE_STATE_RENDER_TARGET);
        ok = ok && t.sync == D3D12_BARRIER_SYNC_RENDER_TARGET
                && t.access == D3D12_BARRIER_ACCESS_RENDER_TARGET
                && t.layout == D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    }
    {
        const Translated t = tex(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ok = ok && t.access == D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE
                && t.layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    }
    {
        const Translated t = tex(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ok = ok && t.access == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS
                && t.layout == D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
    }
    {
        const Translated t = tex(D3D12_RESOURCE_STATE_COPY_SOURCE);
        ok = ok && t.sync == D3D12_BARRIER_SYNC_COPY
                && t.layout == D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    }
    Check(ok, "barrier translation: single states match design D4");

    // COMMON is zero, not a bit, and PRESENT shares its value.
    ok = tex(D3D12_RESOURCE_STATE_COMMON).layout == D3D12_BARRIER_LAYOUT_COMMON &&
         tex(D3D12_RESOURCE_STATE_COMMON).sync == D3D12_BARRIER_SYNC_ALL &&
         buf(D3D12_RESOURCE_STATE_COMMON).layout == D3D12_BARRIER_LAYOUT_UNDEFINED;
    Check(ok, "barrier translation: COMMON/PRESENT -> SYNC_ALL, common layout (none for buffers)");

    // Combined SHADER_RESOURCE: same access and layout, sync ORs. This is the engine's 0xC0.
    {
        const Translated t = tex(static_cast<D3D12_RESOURCE_STATES>(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        ok = t.access == D3D12_BARRIER_ACCESS_SHADER_RESOURCE &&
             t.layout == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE &&
             (t.sync & D3D12_BARRIER_SYNC_PIXEL_SHADING) != 0 &&
             (t.sync & D3D12_BARRIER_SYNC_COMPUTE_SHADING) != 0;
        Check(ok, "barrier translation: NON_PIXEL|PIXEL keeps one layout and ORs sync");
    }

    // Two DIFFERENT read layouts must collapse to GENERIC_READ — VSM.PhysOwner's resting state.
    {
        const Translated t = tex(static_cast<D3D12_RESOURCE_STATES>(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE));
        ok = t.layout == D3D12_BARRIER_LAYOUT_GENERIC_READ &&
             (t.access & D3D12_BARRIER_ACCESS_SHADER_RESOURCE) != 0 &&
             (t.access & D3D12_BARRIER_ACCESS_COPY_SOURCE) != 0;
        Check(ok, "barrier translation: NON_PIXEL|COPY_SOURCE collapses to GENERIC_READ");
    }

    // Buffer-only states carry no layout, and asking for a texture layout is a caller bug.
    {
        const Translated t = buf(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        ok = t.layout == D3D12_BARRIER_LAYOUT_UNDEFINED &&
             t.access == D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT &&
             t.sync == D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
        ok = ok && !IsTextureCompatible(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        ok = ok && !IsTextureCompatible(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        ok = ok && IsTextureCompatible(D3D12_RESOURCE_STATE_RENDER_TARGET);
        Check(ok, "barrier translation: buffer-only states have no texture layout");
    }

    // An unknown bit must widen, never silently drop: an under-specified barrier is a race.
    {
        const Translated t = tex(static_cast<D3D12_RESOURCE_STATES>(0x40000000));
        Check(t.sync == D3D12_BARRIER_SYNC_ALL && t.layout == D3D12_BARRIER_LAYOUT_COMMON,
              "barrier translation: unknown state falls back to the widest correct barrier");
    }
}

// --- Death tests -------------------------------------------------------------

int RunDeathTestChild(const char* flag)
{
    // Abort quietly: no "abort() has been called" message box, no WER fault
    // report — the parent only inspects the exit code.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    SubmitTimeline tl;
    tl.BeginTimeline();

    if (std::strcmp(flag, kFlagNullCl) == 0) {
        const size_t batch = tl.BeginBatch();
        tl.RegisterDirect(nullptr, batch, 0); // must abort
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
        tl.RegisterDirect(FakeCL(1), 2, 0); // must abort
    }
    else if (std::strcmp(flag, kFlagDuplicateCl) == 0) {
        const size_t batch = tl.BeginBatch();
        tl.RegisterDirect(FakeCL(1), batch, 0);
        tl.RegisterDirect(FakeCL(1), batch, 1); // same pointer (distinct order) -> must abort
    }
    else if (std::strcmp(flag, kFlagDupOrderDirect) == 0) {
        const size_t batch = tl.BeginBatch();
        tl.RegisterDirect(FakeCL(1), batch, 5);
        tl.RegisterDirect(FakeCL(2), batch, 5); // distinct CLs, same localOrder
        tl.ApplySubmitOrder();                  // must abort (nondeterministic tie)
    }
    else if (std::strcmp(flag, kFlagDupOrderBundle) == 0) {
        const size_t batch = tl.BeginBatch();
        tl.RegisterDriver(FakeGfxCL(1), batch);
        tl.RegisterBundle(FakeGfxCL(2), batch, 3);
        tl.RegisterBundle(FakeGfxCL(3), batch, 3); // distinct bundles, same localOrder
        tl.ApplySubmitOrder();                     // must abort (nondeterministic tie)
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
    const char* deathFlags[] = {
        kFlagNullCl, kFlagInvalidBatch, kFlagDuplicateCl,
        kFlagDupOrderDirect, kFlagDupOrderBundle,
    };
    if (cmdLine != nullptr) {
        for (const char* flag : deathFlags) {
            if (std::strstr(cmdLine, flag) != nullptr) {
                return RunDeathTestChild(flag);
            }
        }
    }

    gLog.Open("renderer_submission_stress.log", diag::ArtifactMode::PerRunTruncate);
    gFailures = 0;
    gUnexpectedFallbacks = 0;

    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const auto t0 = std::chrono::steady_clock::now();
    Log("renderer submission stress, hardware threads: %u\n", hw);

    constexpr int kRounds = 25;
    for (int round = 0; round < kRounds; ++round) {
        Log("--- round %d/%d ---\n", round + 1, kRounds);

        ScenarioOrderRetention(round);
        ScenarioDriverOrdering();
        ScenarioConcurrentRegistration(hw, 64, 40);
        ScenarioConcurrentRegistration(2, 256, 20);
        ScenarioPoolReuse(200);
        ScenarioBundleRetention();
        ScenarioDeterministicOrder(32);
        ScenarioBarrierTranslation();

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
    gLog.Close();
    return gFailures;
}
