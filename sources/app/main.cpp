#include <windows.h>
#include "core/diagnostics/DiagPaths.h"
#include <mimalloc.h>
#pragma warning(push)
#pragma warning(disable: 28251)
#include "mimalloc-new-delete.h"
#pragma warning(pop)
#include "app/App.h"
#include "assets/AssetImporter.h"
#include "app/diagnostics/CullBenchmark.h"
#include "app/diagnostics/SceneStress.h"
#include "app/scene/SceneRenderQueue.h"
#include "core/logging/Log.h"
#include "core/logging/diagnostics/LogStress.h"
#include "core/task/diagnostics/TaskSystemStress.h"
#include "core/profiling/ProfilerScopes.h"
#include "ocean/OceanRenderable.h"
#include "rendering/core/BarrierTranslation.h"
#include "core/diagnostics/BootProfile.h"
#include "rendering/core/GBufferBindingGuard.h"
#include "rendering/core/GraphicsDevice.h"
#include "rendering/diagnostics/RendererSubmissionStress.h"
#include "rendering/meshes/MeshManager.h" // W7.1b: g_meshBakeMode (--bake-meshes)
#include "meshoptimizer.h"                // --reimport-drop-small= -> meshopt_SimplifyPrune
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/rt/RtSmoke.h"
#include "rendering/visibility/HzbCullSelfTest.h" // occlusion plan S2: --hzb-cull-selftest
#include "rendering/shadows/VirtualShadowMap.h" // temporary VSM perf-harness tunables (--vsm-*)
#include "rendering/meshes/LodSelect.h"          // shadow caster LOD curve (--vsm-lodbias/--vsm-lodstride)
#include "rendering/renderables/InstanceTypes.h"  // S0: g_shadowMode / g_csmDebugMode (--shadow-mode, --csm-tint)
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/core/VisibilityStats.h" // occlusion plan S0: --vis-readout
#include "rendering/core/CommandListBindState.h" // --no-bind-batching
#include "text/TextManager.h"
#include "vfx/WindState.h"                       // W8: g_windFreeze / g_windFrozenTime (--wind-freeze)

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

int ForceMi() { return mi_version(); }


namespace
{
void ApplyGbvModeArg(const char* cmd);
void ApplyGbvSelfTestArg(const char* cmd);

// Extract a CLI value for `key`, accepting both "key=value" and "key value" (optionally quoted
// for paths with spaces). Returns "" when the key is absent or has no value.
std::string ExtractArgValue(const char* cmd, const char* key)
{
    const char* p = std::strstr(cmd, key);
    if (!p) { return {}; }
    p += std::strlen(key);
    while (*p == '=' || std::isspace(static_cast<unsigned char>(*p))) { ++p; }
    std::string v;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') { v.push_back(*p); ++p; }
    } else {
        while (*p && !std::isspace(static_cast<unsigned char>(*p))) { v.push_back(*p); ++p; }
    }
    return v;
}


// `--gbv-selftest=N`: deliberately issue N draws with unbound descriptor tables, to find out what
// a given --gbv-mode actually CATCHES. Debug only; see GBufferBindingGuard.h.
void ApplyGbvSelfTestArg(const char* cmd)
{
    const std::string n = ExtractArgValue(cmd, "--gbv-selftest");
    if (!n.empty()) { render::g_gbvSelfTestDraws = std::atoi(n.c_str()); }
}

// `--gbv-mode=guarded|unguarded|state|none` -- how much shader rewriting GBV does.
//
// This is a WALL-CLOCK knob, not a correctness one for the common case. GBV rewrites every shader
// the first time its pipeline is bound, on the CPU, inside the pass body that binds it. Measured on
// this project with `--scene-stress-gbv=2`: 151 s total, of which 116 s were TWO frames, of which
// 76 s was the single first dispatch of lighting_cs.hlsl. Boot itself was 5 s.
//
// guarded (default) -- D3D12's own default; bounds-check control flow around every access.
// unguarded        -- same out-of-bounds/uninitialized reports, without the guard branches.
// state            -- resource-state and descriptor validation only; no shader rewriting.
// none             -- GBV on, shader patching off entirely.
void ApplyGbvModeArg(const char* cmd)
{
    const std::string mode = ExtractArgValue(cmd, "--gbv-mode");
    if (mode.empty()) { return; }
    if (mode == "none")           { GraphicsDevice::SetGbvShaderPatchMode(0); }
    else if (mode == "state")     { GraphicsDevice::SetGbvShaderPatchMode(1); }
    else if (mode == "unguarded") { GraphicsDevice::SetGbvShaderPatchMode(2); }
    else if (mode == "guarded")   { GraphicsDevice::SetGbvShaderPatchMode(3); }
}

// "--set=<name>:<value>[;<name>:<value>...]": hold settings at fixed values for the run, from the
// same name table --sweep uses (App::ApplyFixedSettings). EVERY occurrence, not just the first:
// this used to be a single strstr, so a command line with two `--set=` flags silently applied one
// and dropped the other -- and a measurement taken that way reports the settings you TYPED, not
// the ones that ran. Cost me an A/B. The documented `--set=a:1;b:2` form still works; the loop
// just also accepts the other one. Parsed ABOVE the scene-stress branch (occlusion plan S3a): a
// GBV gate has to be able to switch the pass under test on.
void ParseFixedSettingsArg(const char* lpCmdLine)
{
    if (!lpCmdLine) { return; }
    for (const char* flag = std::strstr(lpCmdLine, "--set="); flag != nullptr;
         flag = std::strstr(flag, "--set=")) {
        const char* p = flag + std::strlen("--set=");
        const char* end = p;
        while (*end && *end != ' ' && *end != '\t') { ++end; }
        flag = end; // resume scanning after this flag's value
        while (p < end) {
            const char* semi = std::strchr(p, ';');
            const char* itemEnd = (semi && semi < end) ? semi : end;
            const char* colon = std::strchr(p, ':');
            if (colon && colon < itemEnd) {
                g_fixedSettings.emplace_back(std::string(p, colon),
                                             static_cast<float>(std::atof(colon + 1)));
            }
            if (itemEnd >= end) { break; }
            p = itemEnd + 1;
        }
    }
}

void EnableDpiAwareness()
{
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

    // Try to opt-in to the most precise DPI awareness available on the host OS.
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setProcessDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setProcessDpiAwarenessContext &&
            setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        {
            return;
        }
    }

    HMODULE shcore = GetModuleHandleW(L"shcore.dll");
    if (!shcore)
    {
        shcore = LoadLibraryW(L"shcore.dll");
    }
    if (shcore)
    {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto setProcessDpiAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setProcessDpiAwareness &&
            SUCCEEDED(setProcessDpiAwareness(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/)))
        {
            return;
        }
    }

    if (user32)
    {
        using SetProcessDpiAwareFn = BOOL(WINAPI*)();
        if (auto setProcessDpiAware = reinterpret_cast<SetProcessDpiAwareFn>(
                GetProcAddress(user32, "SetProcessDPIAware")))
        {
            setProcessDpiAware();
        }
    }
}
} // namespace

// Entry point
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nShowCmd
)
{
    // "--log-stress" runs the logging stress harness instead of the app; exit code is the number
    // of failed checks. It owns the logger's lifetime itself (repeated Initialize/Shutdown is one
    // of the things it tests), so it is dispatched BEFORE the normal session below.
    if (lpCmdLine && std::strstr(lpCmdLine, "--log-stress") != nullptr) {
        return RunLogStress(lpCmdLine);
    }

    // The log session is the first thing that exists and the last thing that goes away: every
    // early-return harness below runs inside it, and the RAII object flushes the session file on
    // each of those returns. --log-level / --log-category / --log-sync / --log-no-file / --log-file
    // are read here so they reach the harnesses too.
    struct LogSession {
        explicit LogSession(const char* commandLine) {
            logging::LogConfig config;
            logging::ApplyCommandLine(commandLine, config);
            logging::Initialize(config);
            logging::SetCurrentThreadName("Main");
        }
        ~LogSession() { logging::Shutdown(); }
    } logSession(lpCmdLine);

    // "--tasksystem-stress" runs the task-system stress harness instead of the
    // app; exit code is the number of failed checks. With "--stress-overflow"
    // the process is expected to abort (dependents_ overflow death test).
    if (lpCmdLine && std::strstr(lpCmdLine, "tasksystem-stress") != nullptr) {
        const bool overflow = std::strstr(lpCmdLine, "stress-overflow") != nullptr;
        return RunTaskSystemStress(overflow);
    }

    // "--renderer-submission-stress" runs the CPU-only submission-timeline
    // stress harness instead of the app; exit code is the number of failed
    // checks. Its death-test sub-flags (--stress-*) intentionally abort and
    // are spawned by the harness itself in child processes.
    if (lpCmdLine && std::strstr(lpCmdLine, "renderer-submission-stress") != nullptr) {
        return RunRendererSubmissionStress(lpCmdLine);
    }

    if (lpCmdLine && std::strstr(lpCmdLine, "textmanager-benchmark") != nullptr) {
        return RunTextManagerBenchmark(diag::LogPath("textmanager_benchmark.csv").c_str());
    }

    // "--cull-benchmark" times the per-object frustum intersect (legacy DirectXMath vs the
    // precomputed-plane path) over a fixed AABB set; results in cull_benchmark.txt.
    if (lpCmdLine && std::strstr(lpCmdLine, "cull-benchmark") != nullptr) {
        return RunCullBenchmark(diag::LogPath("cull_benchmark.txt").c_str());
    }

    // "--cull-nofuse" forces the split Bucketize()+Cull() path for self-culling views, for an
    // in-engine A/B against the default fused BucketizeCull. Benchmark-only.
    if (lpCmdLine && std::strstr(lpCmdLine, "cull-nofuse") != nullptr) {
        g_useFusedBucketizeCull = false;
    }

    // "--rt-smoke" runs the headless DXR smoke harness (device + RT caps + a
    // trivial BLAS/TLAS build) instead of the app; verdict in rt_smoke.txt,
    // exit code 0 on PASS/SKIP, non-zero on FAIL.
    if (lpCmdLine && std::strstr(lpCmdLine, "rt-smoke") != nullptr) {
        return RunRtSmoke(diag::LogPath("rt_smoke.txt").c_str());
    }

    // "--hzb-cull-selftest" (occlusion plan S2) runs the headless box -> HZB visibility self-test
    // instead of the app: a synthetic pyramid, hand-set verdicts, the GPU held equal to the CPU
    // mirror. Per-case lines and the verdict go to the session log; exit code = failed checks.
    if (lpCmdLine && std::strstr(lpCmdLine, "hzb-cull-selftest") != nullptr) {
        return RunHzbCullSelfTest();
    }

    // "--rt-force-as-fail" (S13 test hook): make every acceleration-structure
    // allocation fail, so the graceful RT-disable → SSR-fallback path can be
    // exercised in the live app without actually exhausting VRAM.
    if (lpCmdLine && std::strstr(lpCmdLine, "rt-force-as-fail") != nullptr) {
        rt::AccelerationStructureManager::SetForceAllocFailureForTest(true);
    }

    // "--import <staging-dir> [--skybox <file.hdr>]" runs the offline asset conversion backend
    // (Part H1) headless — PNG/JPG -> mipped BC7 DDS, texture-set MR synthesis, flipbook atlases,
    // .hdr -> BC6H cubemap — then exits. Verdict in asset_import.log; exit code = failed count.
    if (lpCmdLine && std::strstr(lpCmdLine, "--import") != nullptr) {
        assets::ImportOptions opts;
        opts.stagingDir = ExtractArgValue(lpCmdLine, "--import");
        opts.skyboxHdr  = ExtractArgValue(lpCmdLine, "--skybox");
        if (const char* m = std::strstr(lpCmdLine, "--max-size=")) {
            opts.maxTextureSize = std::atoi(m + std::strlen("--max-size="));
        }
        if (const char* m = std::strstr(lpCmdLine, "--sky-face=")) {
            opts.skyboxFaceSize = std::atoi(m + std::strlen("--sky-face="));
        }
        // "--sky-target=<median luminance>": sky calibration target, 0 to keep the source's own
        // radiance. See ImportOptions::skyTargetMedianLuma.
        if (const char* m = std::strstr(lpCmdLine, "--sky-target=")) {
            opts.skyTargetMedianLuma = (float)std::atof(m + std::strlen("--sky-target="));
        }
        // P16.3: "--sky-keep-sun" leaves the sun disc in the LIGHTING derivatives (the pre-P16.3
        // behaviour, for a sky used with no directional light of its own); "--sky-sun-radius=<deg>"
        // sets the cut. See ImportOptions::skyRemoveSunFromIbl.
        opts.skyRemoveSunFromIbl = std::strstr(lpCmdLine, "--sky-keep-sun") == nullptr;
        if (const char* m = std::strstr(lpCmdLine, "--sky-sun-radius=")) {
            opts.skySunRadiusDeg = (float)std::atof(m + std::strlen("--sky-sun-radius="));
        }
        // "--sky-out=<textures root>": move the finished cube, its two IBL siblings and the shared
        // brdf_lut to where the engine loads them from, instead of leaving them in the staging
        // folder. Same destinations the GUI import panel uses.
        opts.skyOutputRoot = ExtractArgValue(lpCmdLine, "--sky-out");
        opts.highQuality = std::strstr(lpCmdLine, "--high") != nullptr; // opt-in exhaustive BC7
        opts.flipGreen   = std::strstr(lpCmdLine, "--flip-green") != nullptr;
        opts.bc5Normal   = std::strstr(lpCmdLine, "--bc5-normal") != nullptr;
        opts.centerNormals = std::strstr(lpCmdLine, "--center-normals") != nullptr;
        opts.useGpu      = std::strstr(lpCmdLine, "--cpu") == nullptr; // H5: GPU BC encode default-on
        return assets::RunImport(opts);
    }

    EnableDpiAwareness();

    // "--scene-stress" (optionally "--scene-stress=<iterations>") boots the real
    // renderer/device/scene and then autonomously hammers the scene-lifecycle
    // churn operations (level reload/switch, window resize, DLSS mode, render/
    // reflection scale, editor spawn/delete) to reproduce the intermittent
    // launch/render crash. Verdict in scene_stress.log; exit 0 = clean through
    // all iterations, nonzero = a fault was caught (the log names the op).
    // Barrier plan step 3: "--barrier-cmp" turns on the registered-vs-performed comparator
    // (DBWIN "[barrier-cmp]" lines). Default off — it observes every converted pass body.
    // Parsed BEFORE the scene-stress branch below, which returns without reading the rest of
    // the command line: the churn is exactly where this wants to run.
    if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-cmp")) {
        render::g_barrierComparator = true;
    }
    // "--set=...": above the scene-stress branch too, so the harness can pin the setting a gate
    // is about (App::ApplyFixedSettings runs after its bootstrap).
    ParseFixedSettingsArg(lpCmdLine);

    // Barrier plan step 6: "--canonical-check" logs every resource that did not END the frame in
    // its declared canonical state (DBWIN "[canonical]" lines). Parsed above the scene-stress
    // branch for the same reason as --barrier-cmp: the churn is where drift shows up.
    // Step 7 prerequisite: a few resting states are CONFIG-dependent, and the Step 6 constants were
    // all measured with the shipping defaults. These two flip the VSM page-draw path so
    // --canonical-check can be re-run against the other configuration; they must be parsed here for
    // the same reason as the flags above (the scene-stress branch returns first). Names match the
    // globals: "--vsm-page-multidraw" turns g_pageDrawSingle OFF; "--vsm-page-compact" /
    // "--vsm-page-nocompact" force g_pageDrawCompact (default ON) either way.
    if (lpCmdLine && std::strstr(lpCmdLine, "--vsm-page-multidraw")) {
        vsm::g_pageDrawSingle = false;
    }
    // Compaction is ON by default since 2026-08-24, so keep BOTH directions: a flag that only sets
    // the value it already has is a control that does nothing.
    // S5 A/B: "--vsm-no-local-scatter" puts local (spot/point) views back on the brute-force
    // per-page cull, so both local-light paths are reachable from one binary.
    if (lpCmdLine && std::strstr(lpCmdLine, "--vsm-no-local-scatter")) {
        vsm::g_scatterLocalViews = false;
    }
    if (lpCmdLine && std::strstr(lpCmdLine, "--vsm-page-nocompact")) {
        vsm::g_pageDrawCompact = false;
    }
    else if (lpCmdLine && std::strstr(lpCmdLine, "--vsm-page-compact")) {
        vsm::g_pageDrawCompact = true;
    }

    // Step 7: "--barrier-compile-log" prints the compiled barrier count per frame. The compile
    // itself always runs once every graph has Prepares; only the logging is gated.
    if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-compile-log")) {
        render::g_barrierCompileLog = true;
    }

    // "--barrier-flip-trace" logs every emitted barrier point plus every request that matched
    // none. Loud — for chasing one resource. (The flip itself is no longer a flag: the compiled
    // barriers are the only barrier path there is, ResourceStateTracker having been deleted.)
    if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-flip-trace")) {
        render::g_barrierFlipTrace = true;
    }
    // "--barrier-cache-verify" recompiles the barriers every frame and diffs them against what the
    // cross-frame cache would have served. Slower than no cache at all; it is a correctness gate.
    // Step 9: "--legacy-barriers" forces the LEGACY barrier path even where the device supports
    // enhanced barriers, so the old path stays bisectable once step 15 flips the default. Must be
    // set before device creation.
    if (lpCmdLine && std::strstr(lpCmdLine, "--legacy-barriers")) {
        GraphicsDevice::ForceLegacyBarriers(true);
    }
    // "--enhanced-barriers" was the opt-in while the path was being built. Step 15 made enhanced
    // the default on capable hardware, so this is now a no-op accepted for compatibility;
    // "--legacy-barriers" above is the switch that changes anything.
    if (lpCmdLine && std::strstr(lpCmdLine, "--enhanced-barriers")) {
        GraphicsDevice::EnableEnhancedBarriers(true);
    }
    // Step 15: "--barrier-msg-trace" writes a MODULE-attributed stack to logs/barrier_msg_trace.log
    // for each barrier-interop message (1350/1334/527/538). Debug-only; parsed above the
    // scene-stress branch like every other flag the churn needs.
    if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-msg-trace")) {
        GraphicsDevice::EnableBarrierMessageTrace(true);
    }
    // Step 16: "--barrier-census" counts the (before -> after) pairs the engine actually emits and
    // dumps them to logs/barrier_census.log, so sync narrowing is aimed at what the frame uses.
    if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-census")) {
        barriers::SetCensusEnabled(true);
    }
    // Step 16: "--barrier-sync-wide" restores the conservative sync scopes the narrowing removed.
    // The A/B switch for measuring it, and the bisect switch for a suspected sync race afterwards.
    if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-sync-wide")) {
        barriers::SetWideSync(true);
    }
    if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-cache-verify")) {
        render::g_barrierCacheVerify = true;
    }

    if (lpCmdLine && std::strstr(lpCmdLine, "--canonical-check")) {
        render::g_canonicalCheck = true;
    }

    // Async-compute plan step 1: "--compute-lane-probe" acquires one COMPUTE allocator + command
    // list at boot, checks it takes the frame's descriptor heaps and closes, and writes the verdict
    // to the session log ([caps] compute-lane probe). Submits nothing. Parsed here, above the scene-stress branch, for the
    // same reason as the flags around it — that branch returns before the boot flags are read.
    if (lpCmdLine && std::strstr(lpCmdLine, "--compute-lane-probe")) {
        render::g_computeLaneProbe = true;
    }

    // Async-compute plan step 2: "--async-empty-submit" puts one EMPTY compute command list on the
    // async queue every frame, so the two-queue frame fence is exercised by a real submission. The
    // step's proof device — the gates are run with it ON. Parsed above the scene-stress branch for
    // the same reason as everything around it.
    if (lpCmdLine && std::strstr(lpCmdLine, "--async-empty-submit")) {
        render::g_asyncEmptySubmit = true;
    }

    // Async-compute plan step 3: "--async-order-probe" adds a known-ordered cross-queue pair
    // (compute signals, next frame's graphics waits) so the two-track trace can be checked against
    // an ordering that is true by construction. Implies --async-empty-submit, since the signal is
    // issued from that submission. Deliberately serialises the queues; diagnostics only.
    // Async-compute plan step 6: "--dump-submit-order" writes the frame's submitted command-list
    // arrays (by debug name, per queue) to logs/submit_order.log once. The acceptance for per-queue
    // submission is that the GRAPHICS array is unchanged, and this is what makes that checkable.
    // Async-compute plan step 8 (D4): "--no-async-compute" forces every pass back onto the graphics
    // queue. PERMANENT, like "--legacy-barriers" — a suspected async regression must be one flag
    // away from being bisected, not a rebuild away.
    if (lpCmdLine && std::strstr(lpCmdLine, "--no-async-compute")) {
        render::g_noAsyncCompute = true;
    }

    // "--no-streamline": take the Streamline SDK out entirely (no slInit, no device proxy).
    // PERMANENT, like "--no-async-compute": a hang that survives "--dlss=off" still has the SDK
    // wrapping the queue, and that must be one flag away from being excluded, not a rebuild away.
    if (lpCmdLine && std::strstr(lpCmdLine, "--no-streamline")) {
        render::g_noStreamline = true;
    }

    // "--no-bind-batching": make every Material::Bind issue its root bindings in full instead of
    // skipping ones the per-command-list cache believes are already set. The cache is
    // thread_local and reset in BeginThreadCommandList, so it is only correct while a thread
    // records exactly one list at a time -- BUNDLES inherit their root state from the PARENT at
    // execution time, which the cache cannot model. A skipped bind leaves a root parameter
    // unbound, which GPU-based validation reports as "Uninitialized root argument accessed" and
    // which shows up in the wild as the graphics queue quietly stopping mid-batch.
    // PERMANENT, like "--no-async-compute": one flag, not a rebuild.
    if (lpCmdLine && std::strstr(lpCmdLine, "--no-bind-batching")) {
        render::g_bindBatchingEnabled = false;
    }

    // "--async-pass=<names>": narrow which passes may use the async compute queue. Only three ever
    // ask for it. `--no-async-compute` proves the second queue is involved in a hang but not WHICH
    // pass, and bisecting that by rebuild is a rebuild per guess.
    //   --async-pass=none                 every pass back on graphics (same as --no-async-compute)
    //   --async-pass=BuildAS              only BuildAS stays async
    //   --async-pass=BuildAS,RTTrace      those two stay async, ObjectCompute goes back
    if (lpCmdLine) {
        if (const char* flag = std::strstr(lpCmdLine, "--async-pass=")) {
            const char* list = flag + std::strlen("--async-pass=");
            // Read to the next space: the value is a comma-separated name list, never a path.
            char buf[128] = {};
            for (size_t i = 0; i < sizeof(buf) - 1 && list[i] && list[i] != ' '; ++i) { buf[i] = list[i]; }
            const auto has = [&buf](const char* n) { return std::strstr(buf, n) != nullptr; };
            render::g_asyncPassBuildAS = has("BuildAS");
            render::g_asyncPassObjectCompute = has("ObjectCompute");
            render::g_asyncPassRtTrace = has("RTTrace");
        }
    }

    // "--dr-check": poll GetDeviceRemovedReason() once per frame, so a device removal leaves
    // logs/device_removed.log behind even when it does NOT surface at Present (a fence wait that
    // never returns, a TDR between frames). Default OFF; Present's own catch reports either way.
    // Measured at 0.207 us per call = 0.006 % of the frame, so turning it on costs nothing
    // measurable — this is the switch to reach for when hunting a removal, not a perf lever.
    if (lpCmdLine && std::strstr(lpCmdLine, "--dr-check")) {
        render::g_deviceRemovalCheck = true;
    }

    // Async-compute plan step 7: "--dump-barriers" writes every compiled barrier once, for a
    // before/after diff of the barrier compile itself.
    if (lpCmdLine && std::strstr(lpCmdLine, "--dump-barriers")) {
        render::g_dumpBarriers = true;
    }

    if (lpCmdLine && std::strstr(lpCmdLine, "--dump-submit-order")) {
        render::g_dumpSubmitOrder = true;
    }

    if (lpCmdLine && std::strstr(lpCmdLine, "--async-order-probe")) {
        render::g_asyncOrderProbe = true;
        render::g_asyncEmptySubmit = true;
    }

    // Same reason as --barrier-cmp: parsed here rather than with the other boot flags below,
    // because the scene-stress branch returns first. Without it the stress run always takes the
    // VSM path and Main_CSM is never even added to the graph, so nothing exercises it.
    if (lpCmdLine) {
        if (const char* flag = std::strstr(lpCmdLine, "--shadow-mode=")) {
            const char* p = flag + std::strlen("--shadow-mode=");
            render::g_shadowMode = (std::strncmp(p, "legacy", 6) == 0) ? render::ShadowMode::Legacy
                                                                       : render::ShadowMode::VSM;
        }
    }

    if (lpCmdLine) {
        // "--dred": force DRED (auto-breadcrumbs + page-fault allocation report) on for a NORMAL
        // run. It is on automatically in Debug and under --scene-stress, which left the interactive
        // Release_Editor session -- where an editor-only device removal actually happens -- with no
        // way to produce the one artefact that names the faulting op and resource. Cheap enough to
        // leave on while chasing one; it must be set before device creation, hence here.
        if (std::strstr(lpCmdLine, "--dred")) {
            GraphicsDevice::EnableDredForStress(true);
        }
        if (const char* flag = std::strstr(lpCmdLine, "scene-stress")) {
            int iterations = 0; // 0 => driver default
            if (const char* eq = std::strchr(flag, '=')) {
                iterations = std::atoi(eq + 1);
            }
            // DRED before device creation so the driver can name the faulting
            // op/resource on a device removal (cheap; does not perturb the race).
            GraphicsDevice::EnableDredForStress(true);
            // GPU-based validation is a heavier, opt-in second signal
            // (--scene-stress-gbv); it perturbs timing so it's off by default.
            // In GBV mode, InfoQueue errors are logged-but-not-fatal so the run
            // reaches the actual device-removal with GBV annotating each frame.
            const bool gbv = std::strstr(lpCmdLine, "scene-stress-gbv") != nullptr;
            if (gbv) {
                GraphicsDevice::EnableGbvForStress(true);
                ApplyGbvModeArg(lpCmdLine);
                ApplyGbvSelfTestArg(lpCmdLine);
            }
            const bool roughnessEdits = std::strstr(lpCmdLine, "--scene-stress-roughness") != nullptr;
            if (roughnessEdits) { g_bootLevelPath = "data/levels/wind_test.json"; }
            // The stress harness is a diagnostic run by definition, so the per-frame buckets are
            // wanted here without a separate flag.
            boot::SetFrameProfiling(true);
            return RunSceneStress(hInstance, nShowCmd, iterations, /*gbvContinue=*/gbv, roughnessEdits);
        }

        // "--gbv" on an ORDINARY run. GBV was reachable only through the stress harness, and the
        // harness drives its own levels and never sees `--set`, so any pass behind a setting —
        // the whole screen-space reflection path, for one, since the default reflection source is
        // RT — could not be GPU-validated at all. Same switch, before device creation; Debug only,
        // because that is where the debug layer itself is enabled.
        if (std::strstr(lpCmdLine, "--gbv") != nullptr) {
            GraphicsDevice::EnableGbvForStress(true);
            ApplyGbvModeArg(lpCmdLine);
            boot::SetFrameProfiling(true);
        }

        // `--boot-profile`: the per-frame buckets on an ORDINARY run. The boot TIMELINE is always
        // written; this adds the per-pass / per-submit / per-present breakdown, which costs a mutex
        // and a string per pass per frame and so is not on by default.
        if (std::strstr(lpCmdLine, "--boot-profile") != nullptr) {
            boot::SetFrameProfiling(true);
        }
    }

    // "--level=<path>" overrides the boot level (headless verification of a specific level).
    if (lpCmdLine) {
        if (const char* flag = std::strstr(lpCmdLine, "--level=")) {
            const char* p = flag + std::strlen("--level=");
            std::string path;
            while (*p && !std::isspace(static_cast<unsigned char>(*p))) { path.push_back(*p); ++p; }
            g_bootLevelPath = path;
        }
        // "--shot=<path>" grabs the backbuffer to a PNG after "--shot-delay=<sec>" (default 7)
        // and exits — reliable headless verification on the flip-model swapchain.
        if (const char* flag = std::strstr(lpCmdLine, "--shot=")) {
            const char* p = flag + std::strlen("--shot=");
            std::string path;
            while (*p && !std::isspace(static_cast<unsigned char>(*p))) { path.push_back(*p); ++p; }
            g_shotPath = path;
        }
        if (const char* flag = std::strstr(lpCmdLine, "--shot-delay=")) {
            g_shotDelaySec = std::atof(flag + std::strlen("--shot-delay="));
        }
        // Phase series from one process (see App.h): "--shot-count=<n> --shot-step=<sec>" with
        // --wind-freeze steps the frozen clock between frames instead of relaunching per phase.
        if (const char* flag = std::strstr(lpCmdLine, "--shot-count=")) {
            g_shotCount = std::max(std::atoi(flag + std::strlen("--shot-count=")), 1);
        }
        if (const char* flag = std::strstr(lpCmdLine, "--shot-step=")) {
            g_shotStepSec = std::atof(flag + std::strlen("--shot-step="));
        }
        if (const char* flag = std::strstr(lpCmdLine, "--shot-interval=")) {
            g_shotIntervalSec = std::max(std::atof(flag + std::strlen("--shot-interval=")), 0.1);
        }
        // "--cam-pos=x,y,z --cam-rot=x,y,z,w": reproduce the exact view a screenshot was taken
        // from. Both are printed verbatim by the on-screen HUD, so a shot round-trips into a
        // headless repro instead of being re-guessed by hand (see AGENTS.md).
        {
            const auto readFloats = [lpCmdLine](const char* key, float* out, int n) -> bool {
                const char* flag = std::strstr(lpCmdLine, key);
                if (!flag) { return false; }
                const char* p = flag + std::strlen(key);
                for (int i = 0; i < n; ++i) {
                    out[i] = static_cast<float>(std::atof(p));
                    const char* comma = std::strchr(p, ',');
                    if (!comma) { return i == n - 1; }
                    p = comma + 1;
                }
                return true;
            };
            const bool hasPos = readFloats("--cam-pos=", g_camPos, 3);
            const bool hasRot = readFloats("--cam-rot=", g_camRot, 4);
            g_camOverride = hasPos || hasRot;
            // "--cam-fly=x,z": constant camera drift (m/s) — the headless stand-in for flying.
            readFloats("--cam-fly=", g_camFly, 2);
            // "--cam-fly-delay=<sec>": hold still this long first (motion-onset transient capture).
            readFloats("--cam-fly-delay=", &g_camFlyDelay, 1);
            // "--cam-fly-yaw=<deg/s>": turn while drifting (headless mouse look; moves the cascade boxes).
            readFloats("--cam-fly-yaw=", &g_camFlyYaw, 1);
            // "--cam-orbit=<radius>,<deg/s>": ring around the start position (grove fly-through).
            readFloats("--cam-orbit=", g_camOrbit, 2);
        }
        // W8: "--wind-freeze[=<seconds>]" pins the wind clock, so a --shot is reproducible to the
        // pixel without touching a single authored wind parameter. Two runs at the SAME value must
        // be byte-identical; two runs at DIFFERENT values differ by exactly that much wind time.
        // That pair is the test a frozen/cached shadow cannot pass, and the reason this exists:
        // authoring swayFrequency 0 for determinism instead makes a frozen shadow indistinguishable
        // from a correct static lean.
        if (const char* flag = std::strstr(lpCmdLine, "--wind-freeze")) {
            vfx::g_windFreeze = true;
            const char* p = flag + std::strlen("--wind-freeze");
            vfx::g_windFrozenTime = (*p == '=') ? static_cast<float>(std::atof(p + 1)) : 0.0f;
        }
        // Temporary VSM perf harness (see App.h). "--profdump=<path>" dumps profiler rows + exits.
        // "--vsm-extent=<f>" / "--vsm-refdist=<f>" pre-set the runtime VSM tunables for a sweep;
        // "--vsm-resident" flips g_residentIterOnly on. All read the globals in VirtualShadowMap.h.
        if (const char* flag = std::strstr(lpCmdLine, "--profdump=")) {
            const char* p = flag + std::strlen("--profdump=");
            std::string path;
            while (*p && !std::isspace(static_cast<unsigned char>(*p))) { path.push_back(*p); ++p; }
            g_profDumpPath = path;
        }
        // "--ocean-shore-sink": cut the ocean's run-up sheet by sinking it under the terrain in
        // the VS instead of discarding it in the PS, which lets the whole ocean draw keep early-Z.
        // Boot-only: it selects a shader VARIANT (a clip behind a runtime branch is still a discard
        // in the compiled shader and restores nothing). Compare two runs.
        if (std::strstr(lpCmdLine, "--ocean-shore-sink")) {
            ocean::g_shoreSinkCut = true;
        }
        // "--ocean-wind=<0..1>": force the sea state for a headless capture (see the global's
        // comment). The shore artifacts only appear in a swell, so a calm level proves nothing.
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-wind=")) {
            ocean::g_windForceOverride =
                (float)std::atof(flag + std::strlen("--ocean-wind="));
        }
        // "--ocean-vs-depth-probe": swap the shore SDF for a screen-space depth probe in the vertex
        // shader, to compare the two. See the global's comment for what it is expected to get wrong.
        if (std::strstr(lpCmdLine, "--ocean-vs-depth-probe")) {
            ocean::g_vsDepthProbe = true;
        }
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-geomfade=")) {
            ocean::g_geometryFadeOverride =
                (float)std::atof(flag + std::strlen("--ocean-geomfade="));
        }
        // Ocean surface variant (see OceanRenderable.h): the modern run-up stack vs the classic
        // pre-rework surface. Either flag overrides the compiled-in default.
        if (std::strstr(lpCmdLine, "--ocean-classic-shore")) {
            ocean::g_shoreRunup = false;
        }
        if (std::strstr(lpCmdLine, "--ocean-runup-shore")) {
            ocean::g_shoreRunup = true;
        }
        // "--ocean-foam-debug[=<view>]": compile the contact-foam diagnostic views into the ocean
        // shader. Without this flag the combo in the ocean window (F7) is inert, because the views
        // are not in the compiled shader at all. The optional "=<view>" preselects one so a view can
        // be captured headless with --shot; otherwise pick it from the combo.
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-foam-debug")) {
            ocean::g_foamDebug = true;
            const char* p = flag + std::strlen("--ocean-foam-debug");
            if (*p == '=') {
                ocean::g_foamDebugView = std::atoi(p + 1);
            }
        }
        // surf sim injection: "--ocean-surf-sim" force-enables the surf sim regardless of the
        // level's surfSimEnabled; "--ocean-surf-debug=<view>" preselects a debug tint so the sim
        // can be captured headless with --shot (both are plain runtime state, no variant).
        if (std::strstr(lpCmdLine, "--ocean-surf-sim")) {
            ocean::g_surfSimForce = true;
        }
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-surf-debug=")) {
            ocean::g_surfSimDebugView = std::atoi(flag + std::strlen("--ocean-surf-debug="));
        }
        // surf sim S1: auto-poke cadence for headless wave captures (seconds between pokes).
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-surf-poke=")) {
            ocean::g_surfSimPokeInterval =
                (float)std::atof(flag + std::strlen("--ocean-surf-poke="));
        }
        // "--trace=<frames>": headless equivalent of the CaptureTrace key.
        if (const char* flag = std::strstr(lpCmdLine, "--trace=")) {
            g_traceFrames = static_cast<uint32_t>(std::atoi(flag + std::strlen("--trace=")));
        }
        // "--dlss=<off|perf|balanced|quality|ultraperf|ultraquality|dlaa>": boot upscaler mode, so a
        // native-resolution capture no longer needs an F-key by hand (see App.h). An unrecognised
        // value leaves the compiled default in place rather than failing — check the capture's
        // render scale, not just the exit code, when a shot comes back at the wrong resolution.
        if (const char* flag = std::strstr(lpCmdLine, "--dlss=")) {
            const char* p = flag + std::strlen("--dlss=");
            const auto is = [p](const char* name) { return std::strncmp(p, name, std::strlen(name)) == 0; };
            if      (is("ultraperf"))    { g_bootDlssMode = static_cast<int>(sl::DLSSMode::eUltraPerformance); }
            else if (is("ultraquality")) { g_bootDlssMode = static_cast<int>(sl::DLSSMode::eUltraQuality); }
            else if (is("off"))          { g_bootDlssMode = static_cast<int>(sl::DLSSMode::eOff); }
            else if (is("perf"))         { g_bootDlssMode = static_cast<int>(sl::DLSSMode::eMaxPerformance); }
            else if (is("balanced"))     { g_bootDlssMode = static_cast<int>(sl::DLSSMode::eBalanced); }
            else if (is("quality"))      { g_bootDlssMode = static_cast<int>(sl::DLSSMode::eMaxQuality); }
            else if (is("dlaa"))         { g_bootDlssMode = static_cast<int>(sl::DLSSMode::eDLAA); }
        }
        // "--log-window": open the session-log viewer at boot (see App.h).
        if (std::strstr(lpCmdLine, "--log-window")) {
            g_bootLogWindow = true;
        }
        // "--no-hud": empty HUD text buffer, so a --shot carries no frame-varying FPS readout.
        if (std::strstr(lpCmdLine, "--no-hud")) {
            g_hudHidden = true;
        }
        // "--sweep=<setting>:<v0>,<v1>,...": one process, one shot per value (see App.h). The value
        // count drives --shot-count, so the caller does not have to keep the two in sync.
        if (const char* flag = std::strstr(lpCmdLine, "--sweep=")) {
            const char* p = flag + std::strlen("--sweep=");
            const char* colon = std::strchr(p, ':');
            const char* end = p;
            while (*end && *end != ' ' && *end != '\t') { ++end; }
            if (colon && colon < end) {
                g_sweepSetting.assign(p, colon);
                const char* v = colon + 1;
                while (v < end) {
                    g_sweepValues.push_back(static_cast<float>(std::atof(v)));
                    const char* comma = std::strchr(v, ',');
                    if (!comma || comma >= end) { break; }
                    v = comma + 1;
                }
            }
            if (!g_sweepValues.empty()) {
                g_shotCount = static_cast<int>(g_sweepValues.size());
            }
        }
        // "--set=...": parsed once, above the scene-stress branch (ParseFixedSettingsArg).
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-extent=")) {
            vsm::g_clipmapBaseExtent = (float)std::atof(flag + std::strlen("--vsm-extent="));
        }
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-refdist=")) {
            vsm::g_refDist = (float)std::atof(flag + std::strlen("--vsm-refdist="));
        }
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-normalbias=")) {
            vsm::g_clipmapNormalBias = (float)std::atof(flag + std::strlen("--vsm-normalbias="));
        }
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-clipblend=")) {
            const float width = std::clamp(
                (float)std::atof(flag + std::strlen("--vsm-clipblend=")), 0.0f, 0.5f);
            vsm::g_clipmapBlendEnabled = width > 0.0f;
            if (width > 0.0f) { vsm::g_clipmapBlendWidth = width; }
        }
        if (std::strstr(lpCmdLine, "--vsm-resident")) {
            vsm::g_residentIterOnly = true;
        }
        // "--vsm-singledraw=0|1" forces the single-draw page render off/on. The flag defaults ON, so
        // without this there is no headless A/B of the flip at all — the per-page loop would only be
        // reachable through the dev-window checkbox, i.e. not from --profdump.
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-singledraw=")) {
            vsm::g_pageDrawSingle = (std::atoi(flag + std::strlen("--vsm-singledraw=")) != 0);
        }
        // "--vsm-compactargs=0|1": same, for the compacted (page, group) arg records.
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-compactargs=")) {
            vsm::g_pageDrawCompact = (std::atoi(flag + std::strlen("--vsm-compactargs=")) != 0);
        }
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-lodbias=")) {
            render::g_shadowLodBias = std::atoi(flag + std::strlen("--vsm-lodbias="));
        }
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-lodstride=")) {
            render::g_shadowLodTierStride = std::clamp(
                std::atoi(flag + std::strlen("--vsm-lodstride=")), 1, 8);
        }
        // S0: "--shadow-mode=legacy|vsm" picks the directional shadow method at boot. The build
        // defaults to VSM, so without this every Legacy CSM measurement would need a Ctrl+V by
        // hand — i.e. no headless --profdump/--shot A/B between the two methods at all, which is
        // exactly what the CSM plan is judged on. (Parsed above too, so it also reaches the
        // scene-stress path.) "--csm-tint" additionally turns the cascade-tint debug view on
        // from the command line, so the tint can be captured with --shot.
        if (std::strstr(lpCmdLine, "--csm-tint")) {
            render::g_csmDebugMode = render::CsmDebugMode::CascadeTint;
        }
        // "--csm-readout": dump the cascade fit table to logs/csm_readout.log on the first frame.
        // The dev window shows the same numbers, but a headless capture cannot open it, and zRange
        // / D16 step are what the pancaking step is judged on.
        // "--vis-readout": dump the per-view visibility counters (objects in/frustum/occluded,
        // terrain chunks, instances, triangles) to logs/visibility_readout.log on frame 600.
        // S0 of docs/occlusion_culling_plan.md.
        if (std::strstr(lpCmdLine, "--vis-readout")) {
            render::g_visDumpReadout = true;
        }
        if (std::strstr(lpCmdLine, "--csm-readout")) {
            render::g_csmDumpReadout = true;
        }
        // "--reimport --reimport-src=<glTF> --reimport-out=<.mesh.bin>": headless CPU-only bake
        // (no device/window). Reads a staging glTF, regenerates normals/tangents + LODs, writes our
        // binary geometry that mesh.json's "geometry" references. The explicit content reimport step.
        if (std::strstr(lpCmdLine, "--reimport-src=")) {
            const auto getArg = [lpCmdLine](const char* key) -> std::string {
                const char* flag = std::strstr(lpCmdLine, key);
                if (!flag) { return {}; }
                const char* p = flag + std::strlen(key);
                std::string v;
                while (*p && !std::isspace(static_cast<unsigned char>(*p))) { v.push_back(*p); ++p; }
                return v;
            };
            const std::string src = getArg("--reimport-src=");
            const std::string out = getArg("--reimport-out=");
            if (src.empty() || out.empty()) { return 2; }
            MeshLoadOptions opt{};
            // Match the runtime mesh load exactly (StaticMesh/TransparentStaticMesh): the engine
            // loads glTF with CCW winding (wantCW=false) for its FrontCounterClockwise=FALSE PSOs.
            // Baking with the default wantCW=true flips the triangles -> the .bin renders backfaces.
            opt.wantCW = false;
            // "--reimport-recompute=2,3,4" mirrors the mesh.json recomputeNormalSlots so the baked
            // normals/tangents match what the runtime glTF path produced (two-sided foliage fix).
            const std::string recompute = getArg("--reimport-recompute=");
            for (size_t i = 0; i < recompute.size();) {
                size_t j = recompute.find(',', i);
                if (j == std::string::npos) { j = recompute.size(); }
                if (j > i) { opt.recomputeNormalSlots.push_back(static_cast<uint32_t>(std::atoi(recompute.substr(i, j - i).c_str()))); }
                i = j + 1;
            }
            // "--reimport-foliage=0,0,1,0,0" mirrors mesh.json "windFoliage" so the bake knows which
            // slots are wood. Without it the along-limb weight falls back to a per-component ramp,
            // which steps at every junction the modeller happened to cut a leaf at.
            const std::string foliage = getArg("--reimport-foliage=");
            for (size_t i = 0; i < foliage.size();) {
                size_t j = foliage.find(',', i);
                if (j == std::string::npos) { j = foliage.size(); }
                opt.slotFoliage.push_back(static_cast<float>(std::atof(foliage.substr(i, j - i).c_str())));
                i = j + 1;
            }
            // "--reimport-chunk=6" mirrors the mesh.json "chunkGrid": the LOD0 triangles are split
            // into a 6x6 grid of submeshes so a shadow page can rasterize only the chunks it
            // overlaps. MUST match what mesh.json carries at runtime — the runtime reads the grid
            // from mesh.json to decide the submeshes are independent casters, and a .bin baked
            // unchunked would then hand it one giant "chunk" with the object's own bounds.
            const std::string chunk = getArg("--reimport-chunk=");
            if (!chunk.empty()) {
                opt.chunkGrid = static_cast<unsigned int>(std::atoi(chunk.c_str()));
            }
            // "--reimport-manifest=models/x.mesh.json": take EVERY bake option from the manifest.
            // Applied before the individual flags below so an explicit flag still wins, which is
            // what makes "re-bake exactly what this asset says, but with one thing changed" a
            // one-liner. Without it a headless re-bake silently dropped every manifest key that had
            // no flag of its own -- lod3Aggressive, lodRatioScale, the per-level drop slots -- and
            // handed back geometry that disagreed with the manifest it was baked from.
            const std::string manifest = getArg("--reimport-manifest=");
            if (!manifest.empty()) {
                if (!MeshManager::ApplyManifestOptions(manifest, opt)) {
                    LOG_ERROR(logging::LogCategory::Asset, "--reimport-manifest: could not read {}", manifest);
                    return 3;
                }
            }
            // "--reimport-normal-weight=0.75" mirrors mesh.json "lodNormalWeight": the LOD
            // simplifier weights the NORMAL alongside position, so collapses stop breaking the
            // shading of a surface whose vertex normals it cannot rewrite (LODs share one vertex
            // buffer). 0 / absent = the position-only metric, byte-identical to the old bake.
            const std::string normalWeight = getArg("--reimport-normal-weight=");
            if (!normalWeight.empty()) {
                opt.lodNormalWeight = static_cast<float>(std::atof(normalWeight.c_str()));
            }
            // "--reimport-scale=0.0107" mirrors the import dialog's unit-fix bakeScale (see
            // MeshLoadOptions::bakeScale) — without it a centimetre-authored asset (the tent)
            // bakes 100x too large. The GUI import was the only way to pass this before.
            const std::string bakeScale = getArg("--reimport-scale=");
            if (!bakeScale.empty()) {
                opt.bakeScale = static_cast<float>(std::atof(bakeScale.c_str()));
            }
            // "--reimport-prune=0.35" — LOD3 foliage prune keep fraction (0 = off, plain meshopt).
            // See MeshLoadOptions::foliagePruneKeep; only acts on slots the --reimport-foliage
            // list marks as leaves.
            const std::string prune = getArg("--reimport-prune=");
            if (!prune.empty()) {
                opt.foliagePruneKeep = static_cast<float>(std::atof(prune.c_str()));
            }
            // "--reimport-lod3=0" — opt out of the harsh LOD3 solid budget (mirrors mesh.json
            // "lod3Aggressive"; the terrain manifests carry false and a CLI re-bake of them
            // must pass this or the dunes' far shadow casters coarsen).
            const std::string lod3 = getArg("--reimport-lod3=");
            if (!lod3.empty()) {
                opt.lod3Aggressive = std::atoi(lod3.c_str()) != 0;
            }
            // LOD3's own budget multipliers (mesh.json "lod3RatioScale"/"lod3ErrorScale").
            const std::string lod3r = getArg("--reimport-lod3-ratio=");
            if (!lod3r.empty()) { opt.lod3RatioScale = static_cast<float>(std::atof(lod3r.c_str())); }
            const std::string lod3e = getArg("--reimport-lod3-error=");
            if (!lod3e.empty()) { opt.lod3ErrorScale = static_cast<float>(std::atof(lod3e.c_str())); }
            // "--reimport-drop-small=1" — the dialog's "Drop small disconnected parts"
            // (meshopt_SimplifyPrune), so a GUI-observed chain is reproducible headless.
            const std::string dropSmall = getArg("--reimport-drop-small=");
            if (!dropSmall.empty() && std::atoi(dropSmall.c_str()) != 0) {
                opt.lodSimplifyOptions |= meshopt_SimplifyPrune;
            }
            // LOD3 leaf interior decimation (mesh.json "foliageInnerRatio"/"foliageInnerError").
            const std::string innerR = getArg("--reimport-inner-ratio=");
            if (!innerR.empty()) { opt.foliageInnerRatio = static_cast<float>(std::atof(innerR.c_str())); }
            const std::string innerE = getArg("--reimport-inner-error=");
            if (!innerE.empty()) { opt.foliageInnerError = static_cast<float>(std::atof(innerE.c_str())); }
            // "--reimport-lod3-drop=2,3" — slots that vanish at LOD3 (mesh.json "lod3DropSlots").
            const std::string lod3Drop = getArg("--reimport-lod3-drop=");
            for (size_t i = 0; i < lod3Drop.size();) {
                size_t j = lod3Drop.find(',', i);
                if (j == std::string::npos) { j = lod3Drop.size(); }
                if (j > i) { opt.lod3DropSlots.push_back(static_cast<uint32_t>(std::atoi(lod3Drop.substr(i, j - i).c_str()))); }
                i = j + 1;
            }
            MeshManager mm;
            return mm.BakeToBinary(src, out, opt) ? 0 : 1;
        }
    }

    App app;
    app.Run(hInstance, nShowCmd);
    return 0;
}
