#include "app/BootParse.h"

#include <crtdbg.h>
#include "core/memory/MemScope.h"
#include <windows.h>
#include "core/diagnostics/DiagPaths.h"
#include <mimalloc.h>
#include "app/BuildVersion.h"
#include "app/GpuRequirements.h"
#include "app/Shipping.h"
#include "app/BootParse.h"    // R3.2b: ExtractArgValue, the switches' parse
#include "app/BootSwitches.h"
#include "core/settings/SettingsRegistry.h"
#include "core/diagnostics/CrashHandler.h"
#include "rendering/core/StreamlineSignature.h"
#include "app/diagnostics/RtAudit.h" // game thread plan G9: --rt-audit
#include "app/scene/SceneRenderQueue.h"
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "materials/Material.h"
#include "core/memory/MemTrace.h"
#include "rendering/core/MemGates.h"
#include "core/threading/ThreadRole.h" // --thread-check-selftest (R15.5)
#include "core/profiling/ProfilerScopes.h"
#include "ocean/OceanRenderable.h"
#include "rendering/core/BarrierTranslation.h"
#include "core/diagnostics/BootProfile.h"
#include "rendering/core/GBufferBindingGuard.h"
#include "rendering/core/GbvPassFilter.h"
#include "rendering/core/GraphicsDevice.h"
#include "rendering/core/MemoryReport.h"
#include "rendering/diagnostics/GraphSelfTest.h" // --crash-selftest=pass-throw (R15.3), graphcap (R15.4)
#include "rendering/diagnostics/RetireSelfTest.h" // --retire-selftest (R16.1)
#include "core/diagnostics/ThrowSelfTest.h" // --throw-selftest, --crash-selftest=view-throw (the second review of wave 1)
#include "rendering/meshes/MeshManager.h" // W7.1b: g_meshBakeMode (--bake-meshes)
#include "rendering/streaming/TextureStreamingIo.h" // --texstream-selftest
#include <fstream>
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/lighting/TimeOfDaySettings.h"  // T3: --tod-legacy-light
#include "rendering/shadows/VirtualShadowMap.h" // temporary VSM perf-harness tunables (--vsm-*)
#include "rendering/meshes/LodSelect.h"          // shadow caster LOD curve (--vsm-lodbias/--vsm-lodstride)
#include "rendering/renderables/InstanceTypes.h"  // S0: g_shadowMode / g_csmDebugMode (--shadow-mode, --csm-tint)
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/TessySettings.h" // Tessy N7: --tessy-lod-build
#include "rendering/core/VisibilityStats.h" // occlusion plan S0: --vis-readout
#include "rendering/core/CommandListBindState.h" // --no-bind-batching
#include "rendering/wind/WindState.h"                       // W8: g_windFreeze / g_windFrozenTime (--wind-freeze)
#include "rendering/lighting/VolumetricCloud.h"  // render::g_cloudProfilePath (--cloud-profile)
#include <algorithm>
#include <filesystem> // --tessy-upgrade=all walks models/
#include <vector>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include "app/BootOptions.h"

// The debug switches' parse (architecture plan R3.2c): what WinMain parsed itself, a function on each switch's row of
// the switch table (app/BootSwitches.cpp), moved verbatim -- the comments with them. boot::ParseSwitches runs them in
// the table's order, which keeps the pairs where the later one wins: --vsm-page-multidraw before --vsm-singledraw,
// --vsm-page-[no]compact before --vsm-compactargs, --ocean-surf-sim-shore before --ocean-runup-shore, --scene-stress
// before --gbv (which asks whether the stress took GBV).
namespace
{
// `--gbv-selftest=N[@Pass]`: deliberately issue N draws with unbound descriptor tables, to find out
// what a given --gbv-mode (or --gbv-passes) actually CATCHES. `@Pass` aims them at one pass's lists
// (RenderPassToWString's name, e.g. 1@Skybox); without it they go to the first N graphics binds,
// wherever the scheduler puts them. `@`, not a second flag: ExtractArgValue is a strstr, and a
// `--gbv-selftest-pass` would be found by the `--gbv-selftest` lookup. Debug only; see
// GBufferBindingGuard.h. Each injected draw logs the list it landed in.
void ApplyGbvSelfTestArg(const char* cmd)
{
    const std::string value = ExtractArgValue(cmd, "--gbv-selftest");
    if (value.empty()) { return; }
    render::g_gbvSelfTestDraws = std::atoi(value.c_str());
    const size_t at = value.find('@');
    if (at == std::string::npos) { return; }
    const std::string pass = value.substr(at + 1);
    render::g_gbvSelfTestPass.assign(pass.begin(), pass.end()); // ASCII names
    bool known = false;
    for (uint16_t i = 0; i < static_cast<uint16_t>(RenderPass::Count) && !known; ++i) {
        const std::wstring_view name = RenderPassToWString(static_cast<RenderPass>(i)); // literal: NUL-terminated
        known = !name.empty() && _wcsicmp(name.data(), render::g_gbvSelfTestPass.c_str()) == 0;
    }
    if (!known) {
        LOG_WARNING(logging::LogCategory::RenderValidation,
                    "--gbv-selftest: '{}' names no render pass -- nothing will be injected", pass);
    }
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

// `--gbv-passes=<name,name,...>` -- the --gbv-mode above on the LISTED passes' command lists only;
// every other list gets the rest mode below, i.e. its shaders are not validated. The answer to
// "2.5 minutes to the first frame to check one pass": only the listed shaders pay the full rewrite
// (wind_test: 155 s -> 16-20 s).
// Names are RenderPassToWString's (CloudTrace, Compose, Lighting, GBuffer, ...), case-insensitive;
// a name also covers its dotted sub-passes (GBuffer -> GBuffer.Driver, ...), and a CL group's
// members are patched together because they share one list. Implies --gbv (the substring matches).
// What it gives up is measured in docs/gbv_startup_profile.md: an error in an UNLISTED pass is not
// reported. A narrowing knob, never a gate -- `--scene-stress-gbv` ignores it. Debug only.
//
// `--gbv-rest=state|none` -- what the UNLISTED lists (and the device default) get.
// STATE_TRACKING_ONLY (default) keeps GBV's resource-state tracking as exact as a full run's, for a
// light rewrite of every shader: +1..3.5 s on wind_test. NONE is the fastest, and the runtime then
// calls state tracking "possibly imprecise" (id=1044) from the first unpatched list on. Its own name,
// not `--gbv-passes-...`: ExtractArgValue is a strstr.
void ApplyGbvPassesArg(const char* cmd)
{
    std::string list = ExtractArgValue(cmd, "--gbv-passes");
    if (!list.empty()) { render::gbv::RequestPassFilter(std::move(list)); }
    const std::string rest = ExtractArgValue(cmd, "--gbv-rest");
    if (rest == "none")       { render::gbv::RequestRestPatchMode(0); }
    else if (rest == "state") { render::gbv::RequestRestPatchMode(1); }
    else if (!rest.empty()) {
        LOG_WARNING(logging::LogCategory::RenderRhi,
                    "--gbv-rest={} not understood (state|none) -- the unlisted lists keep STATE_TRACKING_ONLY", rest);
    }
}

    // "--cloud-profile=<image>" / "--cloud-formation=<dds>" / "--cloud-cells=<dds>": FILE OVERRIDES
    // for the volumetric clouds' generated profile LUT, formation and erosion volumes
    // (VolumetricCloud.h), read when the cloud resources are created -- for an A/B against another
    // set, which stays outside the repository; "--cloud-dump=<dir>" writes the generated set out.
    // Above the scene-stress branch, so a GBV churn can exercise the same uploads.
void WidePathArg(const char* lpCmdLine, const char* name, std::wstring& out)
{
    const char* flag = lpCmdLine ? std::strstr(lpCmdLine, name) : nullptr;
    if (!flag) { return; }
    const char* p = flag + std::strlen(name);
    std::string path;
    while (*p && !std::isspace(static_cast<unsigned char>(*p))) { path.push_back(*p); ++p; }
    const int count = MultiByteToWideChar(CP_ACP, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
    out.assign(static_cast<size_t>(std::max(count, 0)), L'\0');
    if (count > 0) {
        MultiByteToWideChar(CP_ACP, 0, path.c_str(), static_cast<int>(path.size()), out.data(), count);
    }
}
}

namespace boot::parse
{
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
void BarrierCmp(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-cmp")) {
            render::g_barrierComparator_game = true;
        }
}

    // "--retire-selftest[=control|upload|pass]" (docs/architecture_review_plan.md R16.1, R16.8): the deferred-release
    // service's own proof --
    // the game starts as always, a buffer is retired on its 20th frame, the verdict is the exit code
    // (rendering/diagnostics/RetireSelfTest.h).
void RetireSelftest(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--retire-selftest") != nullptr) {
            const std::string arm = ExtractArgValue(lpCmdLine, "--retire-selftest");
            render::ArmRetireSelfTest(arm == "control" ? render::RetireSelfTestMode::Control
                                      : arm == "upload" ? render::RetireSelfTestMode::Upload
                                      : arm == "pass" ? render::RetireSelfTestMode::Pass : render::RetireSelfTestMode::Retire);
        }
}

    // "--rt-audit[=<seconds>]" and "--rt-audit-selftest": the render thread's audit (app/diagnostics/RtAudit.h). Above
    // the scene-stress branch: the audit rides the harness's level churn too (its verdict at the harness's end).
void RtAudit(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--rt-audit")) {
            const char* timed = std::strstr(lpCmdLine, "--rt-audit=");
            const double seconds = timed ? std::atof(timed + std::strlen("--rt-audit=")) : 600.0;
            rtaudit::Configure(seconds, std::strstr(lpCmdLine, "--rt-audit-selftest") != nullptr);
        }
}

void CloudProfile(const char* lpCmdLine)
{
        WidePathArg(lpCmdLine, "--cloud-profile=", render::g_cloudProfilePath);
}

void CloudFormation(const char* lpCmdLine)
{
        WidePathArg(lpCmdLine, "--cloud-formation=", render::g_cloudFormationPath);
}

void CloudCells(const char* lpCmdLine)
{
        WidePathArg(lpCmdLine, "--cloud-cells=", render::g_cloudCellsPath);
}

void CloudDump(const char* lpCmdLine)
{
        WidePathArg(lpCmdLine, "--cloud-dump=", render::g_cloudDumpDir);
}

    // Barrier plan step 6: "--canonical-check" logs every resource that did not END the frame in
    // its declared canonical state (DBWIN "[canonical]" lines). Parsed above the scene-stress
    // branch for the same reason as --barrier-cmp: the churn is where drift shows up.
    // Step 7 prerequisite: a few resting states are CONFIG-dependent, and the Step 6 constants were
    // all measured with the shipping defaults. These two flip the VSM page-draw path so
    // --canonical-check can be re-run against the other configuration; they must be parsed here for
    // the same reason as the flags above (the scene-stress branch returns first). Names match the
    // globals: "--vsm-page-multidraw" turns g_pageDrawSingle OFF; "--vsm-page-compact" /
    // "--vsm-page-nocompact" force g_pageDrawCompact (default ON) either way.
void VsmPageMultidraw(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--vsm-page-multidraw")) {
            vsm::g_pageDrawSingle_game = false;
        }
}

    // Compaction is ON by default since 2026-08-24, so keep BOTH directions: a flag that only sets
    // the value it already has is a control that does nothing.
    // S5 A/B: "--vsm-no-local-scatter" puts local (spot/point) views back on the brute-force
    // per-page cull, so both local-light paths are reachable from one binary.
void VsmNoLocalScatter(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--vsm-no-local-scatter")) {
            vsm::g_scatterLocalViews_game = false;
        }
}

void VsmPageCompaction(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--vsm-page-nocompact")) {
            vsm::g_pageDrawCompact_game = false;
        }
        else if (lpCmdLine && std::strstr(lpCmdLine, "--vsm-page-compact")) {
            vsm::g_pageDrawCompact_game = true;
        }
}

    // Step 7: "--barrier-compile-log" prints the compiled barrier count per frame. The compile
    // itself always runs once every graph has Prepares; only the logging is gated.
void BarrierCompileLog(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-compile-log")) {
            render::g_barrierCompileLog_game = true;
        }
}

    // "--barrier-flip-trace" logs every emitted barrier point plus every request that matched
    // none. Loud — for chasing one resource. (The flip itself is no longer a flag: the compiled
    // barriers are the only barrier path there is, ResourceStateTracker having been deleted.)
void BarrierFlipTrace(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-flip-trace")) {
            render::g_barrierFlipTrace_game = true;
        }
}

    // "--barrier-cache-verify" recompiles the barriers every frame and diffs them against what the
    // cross-frame cache would have served. Slower than no cache at all; it is a correctness gate.
    // Step 9: "--legacy-barriers" forces the LEGACY barrier path even where the device supports
    // enhanced barriers, so the old path stays bisectable once step 15 flips the default. Must be
    // set before device creation.
void LegacyBarriers(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--legacy-barriers")) {
            GraphicsDevice::ForceLegacyBarriers(true);
        }
}

    // "--enhanced-barriers" was the opt-in while the path was being built. Step 15 made enhanced
    // the default on capable hardware, so this is now a no-op accepted for compatibility;
    // "--legacy-barriers" above is the switch that changes anything.
void EnhancedBarriers(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--enhanced-barriers")) {
            GraphicsDevice::EnableEnhancedBarriers(true);
        }
}

    // Step 15: "--barrier-msg-trace" writes a MODULE-attributed stack to logs/barrier_msg_trace.log
    // for each barrier-interop message (1350/1334/527/538). Debug-only; parsed above the
    // scene-stress branch like every other flag the churn needs.
void BarrierMsgTrace(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-msg-trace")) {
            GraphicsDevice::EnableBarrierMessageTrace(true);
        }
}

    // Step 16: "--barrier-census" counts the (before -> after) pairs the engine actually emits and
    // dumps them to logs/barrier_census.log, so sync narrowing is aimed at what the frame uses.
void BarrierCensus(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-census")) {
            barriers::SetCensusEnabled(true);
        }
}

    // Step 16: "--barrier-sync-wide" restores the conservative sync scopes the narrowing removed.
    // The A/B switch for measuring it, and the bisect switch for a suspected sync race afterwards.
void BarrierSyncWide(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-sync-wide")) {
            barriers::SetWideSync(true);
        }
}

void BarrierCacheVerify(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--barrier-cache-verify")) {
            render::g_barrierCacheVerify_game = true;
        }
}

void CanonicalCheck(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--canonical-check")) {
            render::g_canonicalCheck_game = true;
        }
}

    // Async-compute plan step 1: "--compute-lane-probe" acquires one COMPUTE allocator + command
    // list at boot, checks it takes the frame's descriptor heaps and closes, and writes the verdict
    // to the session log ([caps] compute-lane probe). Submits nothing. Parsed here, above the scene-stress branch, for the
    // same reason as the flags around it — that branch returns before the boot flags are read.
void ComputeLaneProbe(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--compute-lane-probe")) {
            render::g_computeLaneProbe_game = true;
        }
}

    // Async-compute plan step 2: "--async-empty-submit" puts one EMPTY compute command list on the
    // async queue every frame, so the two-queue frame fence is exercised by a real submission. The
    // step's proof device — the gates are run with it ON. Parsed above the scene-stress branch for
    // the same reason as everything around it.
void AsyncEmptySubmit(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--async-empty-submit")) {
            render::g_asyncEmptySubmit_game = true;
        }
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
void NoAsyncCompute(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--no-async-compute")) {
            render::g_noAsyncCompute_game = true;
        }
}

    // Tessy N7: "--tessy-lod-build=all|<part>,<part>" builds the cluster DAG at load for every clustered asset or the
    // ones whose .mesh.bin path holds a part -- mesh.json "tessyLod" without touching the manifests.
void TessyLodBuild(const char* lpCmdLine)
{
        if (const char* lb = lpCmdLine ? std::strstr(lpCmdLine, "--tessy-lod-build=") : nullptr) {
            for (const char* p = lb + std::strlen("--tessy-lod-build="); *p && !std::isspace(static_cast<unsigned char>(*p)); ++p) {
                tessy::g_lodBuild.push_back(*p);
            }
        }
}

    // N7e: "--tessy-lod-prune=0" -- the DAG's leaf cards simplified by edge collapses alone (the A/B of the whole-card prune).
void TessyLodPrune(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--tessy-lod-prune=0")) {
            tessy::g_lodPrune_game = false;
        }
}

    // "--tessy-lod-fold=0" -- a double-sided sheet of duplicates keeps both sides in the DAG's simplification (the A/B of
    // TessyDag's FoldTwins).
void TessyLodFold(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--tessy-lod-fold=0")) {
            tessy::g_lodFold_game = false;
        }
}

    // "--no-streamline": take the Streamline SDK out entirely (no slInit, no device proxy).
    // PERMANENT, like "--no-async-compute": a hang that survives "--dlss=off" still has the SDK
    // wrapping the queue, and that must be one flag away from being excluded, not a rebuild away.
void NoStreamline(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--no-streamline")) {
            render::g_noStreamline_game = true;
        }
}

    // "--no-bind-batching": make every Material::Bind issue its root bindings in full instead of
    // skipping ones the per-command-list cache believes are already set. The cache is
    // thread_local and reset in BeginThreadCommandList, so it is only correct while a thread
    // records exactly one list at a time -- BUNDLES inherit their root state from the PARENT at
    // execution time, which the cache cannot model. A skipped bind leaves a root parameter
    // unbound, which GPU-based validation reports as "Uninitialized root argument accessed" and
    // which shows up in the wild as the graphics queue quietly stopping mid-batch.
    // PERMANENT, like "--no-async-compute": one flag, not a rebuild.
void NoBindBatching(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--no-bind-batching")) {
            render::g_bindBatchingEnabled_game = false;
        }
}

// "--async-pass=<names>": narrow which passes may use the async compute queue (BuildAS,
// ObjectCompute, RTTrace, CloudTrace). `--no-async-compute` proves the second queue is involved in a hang but not WHICH
// pass, and bisecting that by rebuild is a rebuild per guess.
//   --async-pass=none                 every pass back on graphics (same as --no-async-compute)
//   --async-pass=BuildAS              only BuildAS stays async
//   --async-pass=BuildAS,RTTrace      those two stay async, ObjectCompute goes back
void AsyncPass(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--async-pass=")) {
            const char* list = flag + std::strlen("--async-pass=");
            // Read to the next space: the value is a comma-separated name list, never a path.
            char buf[128] = {};
            for (size_t i = 0; i < sizeof(buf) - 1 && list[i] && list[i] != ' '; ++i) { buf[i] = list[i]; }
            const auto has = [&buf](const char* n) { return std::strstr(buf, n) != nullptr; };
            render::g_asyncPassBuildAS_game = has("BuildAS");
            render::g_asyncPassObjectCompute_game = has("ObjectCompute");
            render::g_asyncPassRtTrace_game = has("RTTrace");
            render::g_asyncPassCloudTrace_game = has("CloudTrace");
        }
}

    // "--dr-check": poll GetDeviceRemovedReason() once per frame, so a device removal leaves
    // logs/device_removed.log behind even when it does NOT surface at Present (a fence wait that
    // never returns, a TDR between frames). Default OFF; Present's own catch reports either way.
    // Measured at 0.207 us per call = 0.006 % of the frame, so turning it on costs nothing
    // measurable — this is the switch to reach for when hunting a removal, not a perf lever.
void DrCheck(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--dr-check")) {
            render::g_deviceRemovalCheck_game = true;
        }
}

    // "--upload-ring-check[=selftest]": every frame's upload ring sealed at the end of its recording and compared
    // before the queue, after ExecuteCommandLists and when the GPU is done (FrameResource.h). Set before the renderer.
void UploadRingCheck(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--upload-ring-check")) {
            render::g_rtConstantsProbe = true;
            render::g_uploadRingCheck = std::strstr(lpCmdLine, "--upload-ring-check=gpu") == nullptr;
            render::g_uploadRingCheckSelftest = std::strstr(lpCmdLine, "--upload-ring-check=selftest") != nullptr;
        }
}

// "--water-mask-selftest=throw[:<path part>]": the first water mask the frame asks for (of a geometry whose path holds
// the part) skips its bake and its build task throws (ocean/WaterMask.h ArmSelfTestThrow; docs/architecture_review_plan.md M1).
void WaterMaskSelftest(const char* lpCmdLine)
{
        if (const std::string arm = ExtractArgValue(lpCmdLine, "--water-mask-selftest"); !arm.empty()) {
            if (arm == "throw") {
                ocean::watermask::ArmSelfTestThrow({});
            } else if (arm.rfind("throw:", 0) == 0 && arm.size() > 6) {
                ocean::watermask::ArmSelfTestThrow(arm.substr(6));
            } else {
                LOG_ERROR(logging::LogCategory::Ocean, "--water-mask-selftest={}: the only arm is throw[:<path part>]", arm);
            }
        }
}

// "--texstream-selftest=layout|size": the first texture stream-in is built wrong -- its last new mip one row past
// what is read, or its file expected 4 KB larger than it is (rendering/streaming/TextureStreamingIo.h;
// docs/architecture_review_plan.md M14). The worker refuses it and the texture goes off streaming.
void TexstreamSelftest(const char* lpCmdLine)
{
        if (const std::string arm = ExtractArgValue(lpCmdLine, "--texstream-selftest"); !arm.empty()) {
            if (arm == "layout") {
                streaming::TextureStreamingIo::ArmSelfTest(streaming::TextureStreamingIo::SelfTest::Layout);
            } else if (arm == "size") {
                streaming::TextureStreamingIo::ArmSelfTest(streaming::TextureStreamingIo::SelfTest::Size);
            } else {
                LOG_ERROR(logging::LogCategory::Asset, "--texstream-selftest={}: the arms are layout and size", arm);
            }
        }
}

    // Async-compute plan step 7: "--dump-barriers" writes every compiled barrier once, for a
    // before/after diff of the barrier compile itself.
void DumpBarriers(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--dump-barriers")) {
            render::g_dumpBarriers_game = true;
        }
}

void DumpSubmitOrder(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--dump-submit-order")) {
            render::g_dumpSubmitOrder_game = true;
        }
}

void AsyncOrderProbe(const char* lpCmdLine)
{
        if (lpCmdLine && std::strstr(lpCmdLine, "--async-order-probe")) {
            render::g_asyncOrderProbe_game = true;
            render::g_asyncEmptySubmit_game = true;
        }
}

// Same reason as --barrier-cmp: parsed here rather than with the other boot flags below,
// because the scene-stress branch returns first. Without it the stress run always takes the
// VSM path and Main_CSM is never even added to the graph, so nothing exercises it.
void ShadowMode(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--shadow-mode=")) {
            const char* p = flag + std::strlen("--shadow-mode=");
            // S15: three modes. "csm" is accepted as a synonym of "legacy" because that is what the
            // mode is called everywhere else -- the knobs (`csm.*`), the pass (`Pass_CSM`), the
            // readout (`--csm-readout`) and the atlas. An UNRECOGNISED token used to land on VSM,
            // which turned a typo into a silent mode switch (see ShadowSettings.h); it now leaves
            // the mode where it was and is reported.
            if (std::strncmp(p, "legacy", 6) == 0 || std::strncmp(p, "csm", 3) == 0) {
                render::g_shadowMode_game = render::ShadowMode::Legacy;
            } else if (std::strncmp(p, "sdsm", 4) == 0) {
                render::g_shadowMode_game = render::ShadowMode::Sdsm;
            } else if (std::strncmp(p, "vsm", 3) == 0) {
                render::g_shadowMode_game = render::ShadowMode::VSM;
            } else {
                render::g_shadowModeCliUnknown_game = true;
            }
            // An unrecognised token is NOT an override: the settings file decides, exactly as if
            // the flag had not been passed. Otherwise a typo would still pin the session to
            // whatever the build default happens to be.
            render::g_shadowModeFromCli_game = !render::g_shadowModeCliUnknown_game;
        }
}

    // "--dred": force DRED (auto-breadcrumbs + page-fault allocation report) on for a NORMAL
    // run. It is on automatically in Debug and under --scene-stress, which left the interactive
    // Release_Editor session -- where an editor-only device removal actually happens -- with no
    // way to produce the one artefact that names the faulting op and resource. Cheap enough to
    // leave on while chasing one; it must be set before device creation, hence here.
void Dred(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--dred")) {
            GraphicsDevice::EnableDredForStress(true);
        }
}

    // "--no-d3d-debug": a Debug run without the D3D12 debug layer -- what the layer's validation of every recorded call
    // costs the frame, and a faster Debug session when validation is not the question.
void NoD3dDebug(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--no-d3d-debug")) {
            GraphicsDevice::DisableDebugLayer(true);
        }
}

void SceneStress(const char* lpCmdLine)
{
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
                // The gate's verdict covers the whole frame; a narrowed one would be a false CLEAN
                // for every pass left out.
                if (std::strstr(lpCmdLine, "--gbv-passes") != nullptr) {
                    LOG_WARNING(logging::LogCategory::RenderRhi,
                                "--gbv-passes ignored under --scene-stress-gbv: the gate patches every pass");
                }
            }
            const bool roughnessEdits = std::strstr(lpCmdLine, "--scene-stress-roughness") != nullptr;
            const bool skyEdits = std::strstr(lpCmdLine, "--scene-stress-sky") != nullptr;
            if (roughnessEdits || skyEdits) { Boot().frame.levelPath = "data/levels/wind_test.json"; }
            // "--stress-level=<path>": a fourth level in the switch rotation (plan S9e: one with many shadowed lights),
            // and the one the run boots into -- so the churn starts on it (a reload, then every switch, leaves it).
            if (const char* extra = std::strstr(lpCmdLine, "--stress-level=")) {
                const char* p = extra + std::strlen("--stress-level=");
                while (*p && !std::isspace(static_cast<unsigned char>(*p))) { Boot().harness.sceneStressExtraLevel.push_back(*p); ++p; }
                if (!roughnessEdits && !skyEdits) { Boot().frame.levelPath = Boot().harness.sceneStressExtraLevel; }
            }
            // The stress harness is a diagnostic run by definition, so the per-frame buckets are
            // wanted here without a separate flag.
            boot::SetFrameProfiling(true);
            // R3.3e: a harness of App::Run's loop (app/diagnostics/SceneStress.h), no longer a loop of its own: the
            // rest of the command line is parsed as for any run.
            Boot().harness.sceneStress = true;
            Boot().harness.sceneStressIterations = iterations;
            Boot().harness.sceneStressGbv = gbv;
            Boot().harness.sceneStressRoughness = roughnessEdits;
            Boot().harness.sceneStressSky = skyEdits;
        }
}

    // "--gbv" on an ORDINARY run. GBV was reachable only through the stress harness, and the
    // harness drives its own levels and never sees `--set`, so any pass behind a setting —
    // the whole screen-space reflection path, for one, since the default reflection source is
    // RT — could not be GPU-validated at all. Same switch, before device creation; Debug only,
    // because that is where the debug layer itself is enabled.
    // Not under --scene-stress-gbv (above): its "--gbv-mode" would read as this switch, and the gate patches every pass.
void Gbv(const char* lpCmdLine)
{
        if (!Boot().harness.sceneStress && std::strstr(lpCmdLine, "--gbv") != nullptr) {
            GraphicsDevice::EnableGbvForStress(true);
            ApplyGbvModeArg(lpCmdLine);
            ApplyGbvPassesArg(lpCmdLine);
            // The injector on an ordinary run too: what `--gbv-passes` gives up can only be
            // measured where the flag is accepted.
            ApplyGbvSelfTestArg(lpCmdLine);
            boot::SetFrameProfiling(true);
        }
}

    // `--boot-profile`: the per-frame buckets on an ORDINARY run. The boot TIMELINE is always
    // written; this adds the per-pass / per-submit / per-present breakdown, which costs a mutex
    // and a string per pass per frame and so is not on by default.
void BootProfileBuckets(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--boot-profile") != nullptr) {
            boot::SetFrameProfiling(true);
        }
}

    // W8: "--wind-freeze[=<seconds>]" pins the wind clock, so a --shot is reproducible to the
    // pixel without touching a single authored wind parameter. Two runs at the SAME value must
    // be byte-identical; two runs at DIFFERENT values differ by exactly that much wind time.
    // That pair is the test a frozen/cached shadow cannot pass, and the reason this exists:
    // authoring swayFrequency 0 for determinism instead makes a frozen shadow indistinguishable
    // from a correct static lean.
void WindFreeze(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--wind-freeze")) {
            vfx::g_windFreeze_game = true;
            const char* p = flag + std::strlen("--wind-freeze");
            vfx::g_windFrozenTime_game = (*p == '=') ? static_cast<float>(std::atof(p + 1)) : 0.0f;
        }
}

    // "--memreport[=<sec>]": one memory report (platform, the GPU census by tag, every GPU
    // object by size) into the session log <sec> s after the first frame, default 10 (memory
    // profiling plan M1). Keep it inside --shot-delay: the shot ends the run.
void MemReport(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--memreport")) {
            const char* p = flag + std::strlen("--memreport");
            render::ScheduleMemoryReport(*p == '=' ? std::atof(p + 1) : 10.0);
        }
}

    // "--ocean-shore-sink": cut the ocean's run-up sheet by sinking it under the terrain in
    // the VS instead of discarding it in the PS, which lets the whole ocean draw keep early-Z.
    // Boot-only: it selects a shader VARIANT (a clip behind a runtime branch is still a discard
    // in the compiled shader and restores nothing). Compare two runs.
void OceanShoreSink(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--ocean-shore-sink")) {
            ocean::g_shoreSinkCut_game = true;
        }
}

    // "--ocean-wind=<0..1>": force the sea state for a headless capture (see the global's
    // comment). The shore artifacts only appear in a swell, so a calm level proves nothing.
void OceanWind(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-wind=")) {
            ocean::g_windForceOverride_game =
                (float)std::atof(flag + std::strlen("--ocean-wind="));
        }
}

    // "--ocean-vs-depth-probe": swap the shore SDF for a screen-space depth probe in the vertex
    // shader, to compare the two. See the global's comment for what it is expected to get wrong.
void OceanVsDepthProbe(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--ocean-vs-depth-probe")) {
            ocean::g_vsDepthProbe_game = true;
        }
}

void OceanGeomFade(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-geomfade=")) {
            ocean::g_geometryFadeOverride_game =
                (float)std::atof(flag + std::strlen("--ocean-geomfade="));
        }
}

    // Ocean surface variant (see OceanRenderable.h): the surf-sim surface (the default) vs the
    // run-up shore stack. Either flag overrides the compiled-in default. "--ocean-classic-shore"
    // is the surf-sim flag's old name (the surface was called "legacy"/"classic" until
    // 2026-09-12) and keeps working.
void OceanSurfSimShore(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--ocean-surf-sim-shore") || std::strstr(lpCmdLine, "--ocean-classic-shore")) {
            ocean::g_shoreRunup_game = false;
        }
}

void OceanRunupShore(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--ocean-runup-shore")) {
            ocean::g_shoreRunup_game = true;
        }
}

    // "--ocean-foam-debug[=<view>]": compile the contact-foam diagnostic views into the ocean
    // shader. Without this flag the combo in the ocean window (F7) is inert, because the views
    // are not in the compiled shader at all. The optional "=<view>" preselects one so a view can
    // be captured headless with --shot; otherwise pick it from the combo.
void OceanFoamDebug(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-foam-debug")) {
            ocean::g_foamDebug_game = true;
            const char* p = flag + std::strlen("--ocean-foam-debug");
            if (*p == '=') {
                ocean::g_foamDebugView_game = std::atoi(p + 1);
            }
        }
}

    // surf sim injection: "--ocean-surf-sim" force-enables the surf sim regardless of the
    // level's surfSimEnabled; "--ocean-surf-debug=<view>" preselects a debug tint so the sim
    // can be captured headless with --shot (both are plain runtime state, no variant).
void OceanSurfSim(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--ocean-surf-sim")) {
            ocean::g_surfSimForce_game = true;
        }
}

void OceanSurfDebug(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-surf-debug=")) {
            ocean::g_surfSimDebugView_game = std::atoi(flag + std::strlen("--ocean-surf-debug="));
        }
}

    // surf sim S1: auto-poke cadence for headless wave captures (seconds between pokes).
void OceanSurfPoke(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--ocean-surf-poke=")) {
            ocean::g_surfSimPokeInterval_game =
                (float)std::atof(flag + std::strlen("--ocean-surf-poke="));
        }
}

    // "--dlss-mvec=<engine|camera|legacy>": which motion vectors the upscaler is told about
    // (Renderer.h). The A/B that found the inverted-sign bug; `legacy` is the pre-fix sign.
void DlssMvec(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--dlss-mvec=")) {
            const char* p = flag + std::strlen("--dlss-mvec=");
            const auto is = [p](const char* name) { return std::strncmp(p, name, std::strlen(name)) == 0; };
            if      (is("engine")) { render::g_dlssMvecMode_game = 0; }
            else if (is("camera")) { render::g_dlssMvecMode_game = 1; }
            else if (is("legacy")) { render::g_dlssMvecMode_game = 2; }
        }
}

    // "--set=...": parsed once, above the scene-stress branch (ParseFixedSettingsArg).
void VsmRefDist(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-refdist=")) {
            vsm::g_refDist_game = (float)std::atof(flag + std::strlen("--vsm-refdist="));
        }
}

void VsmNormalBias(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-normalbias=")) {
            vsm::g_clipmapNormalBias_game = (float)std::atof(flag + std::strlen("--vsm-normalbias="));
        }
}

void VsmResident(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--vsm-resident")) {
            vsm::g_residentIterOnly_game = true;
        }
}

    // "--vsm-singledraw=0|1" forces the single-draw page render off/on. The flag defaults ON, so
    // without this there is no headless A/B of the flip at all — the per-page loop would only be
    // reachable through the dev-window checkbox, i.e. not from --profdump.
void VsmSingleDraw(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-singledraw=")) {
            vsm::g_pageDrawSingle_game = (std::atoi(flag + std::strlen("--vsm-singledraw=")) != 0);
        }
}

    // "--vsm-compactargs=0|1": same, for the compacted (page, group) arg records.
void VsmCompactArgs(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-compactargs=")) {
            vsm::g_pageDrawCompact_game = (std::atoi(flag + std::strlen("--vsm-compactargs=")) != 0);
        }
}

void VsmLodBias(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-lodbias=")) {
            render::g_shadowLodBias_game = std::atoi(flag + std::strlen("--vsm-lodbias="));
        }
}

void VsmLodStride(const char* lpCmdLine)
{
        if (const char* flag = std::strstr(lpCmdLine, "--vsm-lodstride=")) {
            render::g_shadowLodTierStride_game = std::clamp(
                std::atoi(flag + std::strlen("--vsm-lodstride=")), 1, 8);
        }
}

    // S0: "--shadow-mode=legacy|vsm" picks the directional shadow method at boot. The build
    // defaults to VSM, so without this every Legacy CSM measurement would need a Ctrl+V by
    // hand — i.e. no headless --profdump/--shot A/B between the two methods at all, which is
    // exactly what the CSM plan is judged on. (Parsed above too, so it also reaches the
    // scene-stress path.) "--csm-tint" additionally turns the cascade-tint debug view on
    // from the command line, so the tint can be captured with --shot.
void CsmTint(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--csm-tint")) {
            render::g_csmDebugMode_game = render::CsmDebugMode::CascadeTint;
        }
}

    // "--csm-readout": dump the cascade fit table to logs/csm_readout.log on the first frame.
    // The dev window shows the same numbers, but a headless capture cannot open it, and zRange
    // / D16 step are what the pancaking step is judged on.
    // "--vis-readout": dump the per-view visibility counters (objects in/frustum/occluded,
    // terrain chunks, instances, triangles) to logs/visibility_readout.log on frame 600.
    // S0 of docs/occlusion_culling_plan.md.
void VisReadout(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--vis-readout")) {
            render::g_visDumpReadout_game = true;
        }
}

void CsmReadout(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--csm-readout")) {
            render::g_csmDumpReadout_game = true;
        }
}

    // S15: "--sdsm-readout" dumps the partition table (interval, light-space box, texel size,
    // sample count, the CPU-caster tail) into the session log once the analysis has settled.
    // Same reason as --csm-readout: a headless --shot run has no dev window to read it from,
    // and the texel size is the whole acceptance criterion of the step.
void SdsmReadout(const char* lpCmdLine)
{
        if (std::strstr(lpCmdLine, "--sdsm-readout")) {
            render::sdsm::g_dumpReadout_game = true;
        }
}

} // namespace boot::parse
