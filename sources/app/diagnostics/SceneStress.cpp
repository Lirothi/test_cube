#include "app/diagnostics/SceneStress.h"

#include "app/BootOptions.h"

#include "app/App.h"
#include "core/math/Math.h"
#include "core/task/TaskSystem.h"
#include "core/profiling/Profiler.h"
#include "core/vfs/Vfs.h" // the churn's levels: the tree's or the packages'
#include <algorithm>

#include "app/levels/JsonLevel.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/wind/WindState.h"
#include <cmath>
#include "rhi/BarrierTranslation.h"
#include "rhi/GpuAlloc.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"

#if WITH_EDITOR
#include "app/scene/SceneObjectFactory.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "third_party/json/json.hpp"
#endif

#include <functional>
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h> // ID3D12InfoQueue
#include "core/diagnostics/BootProfile.h"
#include "core/diagnostics/ArtifactWriter.h"
#include "core/memory/MemTags.h"
#include "core/memory/MemTrace.h"
#include "core/threading/ThreadRole.h" // game thread plan G1: the frame's phases, as App::Run gives them
#include "core/threading/Knob.h"
#include "app/diagnostics/RtAudit.h"     // G9: --rt-audit over the level churn
#include "rhi/MemGates.h"
#include "core/logging/Log.h"

#include <chrono>
#include <dbghelp.h>        // StackWalk64 / SymFromAddr (crash-stack logger)
#include <wrl/client.h>

#pragma comment(lib, "dbghelp.lib")

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
// scene_stress.log: every write goes straight to the OS (ArtifactFile), so the TerminateProcess
// exits below lose nothing and the old per-line fflush protocol is gone.
diag::ArtifactFile gLog;

void Log(const char* fmt, ...)
{
    if (!gLog)
    {
        return;
    }
    va_list args;
    va_start(args, fmt);
    gLog.VPrintf(fmt, args);
    va_end(args);
}

// Last-chance crash-stack logger for the stress harness. The driver's
// __try/__except (RunStep_) only covers a MAIN-THREAD fault during a churn step.
// A fault on a TaskSystem worker thread, during teardown, or a C++ throw that
// unwinds through a Win32 WndProc (it cannot cross the KiUserCallbackDispatcher
// kernel-callback boundary, so it becomes a fatal STATUS_FATAL_USER_CALLBACK_
// EXCEPTION) all bypass that guard and kill the process with an opaque OS code
// and no diagnostics. This SetUnhandledExceptionFilter callback fires for exactly
// those cases: it symbolizes the faulting thread's stack via dbghelp (PDBs sit
// next to the exe) and writes it to crash_stack.txt + scene_stress.log before the
// process dies. Complements the driver's DRED dump (which covers GPU faults on
// the normal fault path).
LONG WINAPI StressCrashFilter(EXCEPTION_POINTERS* ep)
{
    const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;

    diag::ArtifactFile cf("crash_stack.txt", diag::ArtifactMode::PerRunTruncate);
    auto emit = [&](const char* fmt, ...)
    {
        char line[1024];
        va_list a; va_start(a, fmt);
        std::vsnprintf(line, sizeof(line), fmt, a);
        va_end(a);
        cf.Write(line);
        Log("%s", line);
    };

    emit("==== UNHANDLED EXCEPTION code=0x%08lX tid=%lu ====\n", code, GetCurrentThreadId());
    {
        // Session-log mirror of the verdict, before the stack walk (which can itself fault on a
        // corrupted stack): emergency path, no writer thread, points at the artifact.
        char line[192];
        std::snprintf(line, sizeof(line),
                      "unhandled exception code=0x%08lX tid=%lu; stack in logs/crash_stack.txt",
                      code, GetCurrentThreadId());
        logging::EmergencyWrite(logging::LogLevel::Fatal, logging::LogCategory::App, line);
        // The writer thread is (normally) still alive: drain whatever the frame queued before it
        // died, so the session log ends with the frames leading up to the fault, not a gap.
        logging::Flush(2000);
    }
    if (ep && ep->ExceptionRecord && code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
        emit("access-violation %s VA=0x%llX\n",
             ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
             static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
    }

    if (!ep || !ep->ContextRecord) { cf.Close(); return EXCEPTION_EXECUTE_HANDLER; }

    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;      sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;   sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;   sf.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 64; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &sf, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
        {
            break;
        }
        const DWORD64 addr = sf.AddrPC.Offset;
        if (addr == 0) { break; }

        char modname[64] = "?";
        const DWORD64 modbase = SymGetModuleBase64(proc, addr);
        if (modbase)
        {
            IMAGEHLP_MODULE64 mi{}; mi.SizeOfStruct = sizeof(mi);
            if (SymGetModuleInfo64(proc, modbase, &mi)) { std::snprintf(modname, sizeof(modname), "%s", mi.ModuleName); }
        }

        char symbuf[sizeof(SYMBOL_INFO) + 512] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 512;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, addr, &disp, sym))
        {
            IMAGEHLP_LINE64 ln{}; ln.SizeOfStruct = sizeof(ln); DWORD lnDisp = 0;
            if (SymGetLineFromAddr64(proc, addr, &lnDisp, &ln))
            {
                emit("  #%02d %s!%s+0x%llX  (%s:%lu)\n", i, modname, sym->Name,
                     static_cast<unsigned long long>(disp), ln.FileName, ln.LineNumber);
            }
            else
            {
                emit("  #%02d %s!%s+0x%llX\n", i, modname, sym->Name, static_cast<unsigned long long>(disp));
            }
        }
        else
        {
            emit("  #%02d %s!0x%llX\n", i, modname, static_cast<unsigned long long>(addr));
        }
    }
    emit("==== end stack ====\n");
    cf.Close();
    return EXCEPTION_EXECUTE_HANDLER; // log, then let the process terminate
}

// The churn operation performed at a given step. Named so a caught fault can be
// attributed to the operation that immediately preceded it.
enum class Op
{
    ReloadLevel,
    SwitchLevel,
    ResizeWindow,
    DlssMode,
    RenderScale,
    ReflectionScale,
#if WITH_EDITOR
    EditorSpawnDelete,
#endif
    Count
};

const char* OpName(Op op)
{
    switch (op)
    {
    case Op::ReloadLevel:      return "ReloadLevel";
    case Op::SwitchLevel:      return "SwitchLevel";
    case Op::ResizeWindow:     return "ResizeWindow";
    case Op::DlssMode:         return "DlssMode";
    case Op::RenderScale:      return "RenderScale";
    case Op::ReflectionScale:  return "ReflectionScale";
#if WITH_EDITOR
    case Op::EditorSpawnDelete: return "EditorSpawnDelete";
#endif
    default:                   return "Unknown";
    }
}

// The scene-lifecycle stress (architecture plan R3.3e): a harness of App::Run's own loop. Its churn ops run once a frame
// at afterFrame -- after Present, the render half idle, where the renderer's own requests (dlss.mode, aa.renderScale,
// --scene-replicate) are taken too -- and the frames after an op are App::Run's frames, the render thread's overlap and
// the UI included. Until R3.3e it drove a loop of its own (RenderFrames_: a frame without the UI, dt 1/60, the render
// thread waited for after every op's frames); the ops, their order, the fault checks and the log lines are as they were.
// Holds no ownership: the App owns the systems.
class SceneStressHarness final : public harness::Harness
{
public:
    SceneStressHarness(HWND hWnd, Renderer& renderer, Scene& scene, LevelManager& levelManager)
        : hWnd_(hWnd)
        , renderer_(renderer)
        , scene_(scene)
        , levelManager_(levelManager)
        , iterations_(Boot().harness.sceneStressIterations > 0 ? Boot().harness.sceneStressIterations : 300) // a few hundred churn steps
        , gbvContinue_(Boot().harness.sceneStressGbv)
        , skyEdits_(Boot().harness.sceneStressSky)
        , roughnessEdits_(Boot().harness.sceneStressRoughness)
    {
    }

    ~SceneStressHarness() override
    {
        if (phase_ != Phase::Start) { SetUnhandledExceptionFilter(previousFilter_); } // the build's own crash handler again (its minidump)
        if (gLog)
        {
            Log("shutdown complete; exit code %d at %.1f s\n", exitCode_, boot::ElapsedMs() / 1000.0);
            gLog.Close();
        }
    }

    const char* Name() const override { return "scene-stress"; }
    bool Wanted() const override { return Boot().harness.sceneStress; }
    int Verdict() const override { return exitCode_; }

    // Once a frame, after Present (App::Run's afterFrame).
    void OnFrame(harness::FrameContext& ctx) override
    {
        switch (phase_)
        {
        case Phase::Done:
            return;
        case Phase::Start:
            Begin_();
            return;
        case Phase::Warmup:
            // A few warm-up frames so the pipeline is fully primed before churn.
            if (--framesLeft_ > 0) { return; }
            if (const char* early = CheckFault_()) { return Fault_(-1, "warmup", early); }
            if (skyEdits_) { Profiler::Get().RequestTraceCapture(iterations_); }
            barriers::AsEmitStats(asEnhancedStart_, asLegacyStart_);
            return NextStep_(ctx);
        case Phase::Spawned:
            // EditorSpawnDelete: the box drawn for two frames, then deleted; then the step's own frames.
            if (--framesLeft_ > 0) { return; }
            if (!Guarded_(&SceneStressHarness::DeleteSpawned_) || faultDetail_[0]) { return Fault_(iter_, opName_, faultDetail_); }
            framesLeft_ = framesAfterOp_;
            phase_ = Phase::Frames;
            return;
        case Phase::Frames:
            if (--framesLeft_ > 0) { return; }
            if (resizeDuringFrame_)
            {
                LOG_INFO(logging::LogCategory::RenderRhi,
                         "resize boundary regression PASS: {} frames, deferred and coalesced requests", framesAfterOp_);
            }
            if (!EndStep_()) { return; }
            return NextStep_(ctx);
        case Phase::Drain:
            if (--framesLeft_ > 0) { return; }
            return Final_(ctx);
        }
    }

    // ResizeWindow's frames: right after each BeginFrame, where its driver made the requests -- they must wait for the
    // next BeginFrame and be coalesced there.
    void OnBeginFrame(harness::FrameContext&) override
    {
        if (phase_ == Phase::Frames && resizeDuringFrame_ && !ResizeBoundary_()) { Fault_(iter_, opName_, faultDetail_); }
    }

    // A frame threw (App::Run rethrows after this): the FAULT verdict and DRED, attributed to the step in progress.
    void OnFrameThrew(const char* what) override
    {
        if (phase_ == Phase::Done) { return; }
        EnsureLog_();
        std::snprintf(faultDetail_, sizeof(faultDetail_), "exception during %s frame: %.220s", opName_[0] ? opName_ : "warmup", what);
        Log("EXCEPTION: %s\n", faultDetail_);
        Fault_(iter_, opName_[0] ? opName_ : "warmup", faultDetail_);
    }

private:
    enum class Phase { Start, Warmup, Spawned, Frames, Drain, Done };

    // A fault before the first afterFrame (a throw in the first frame: OnFrameThrew) comes before Begin_ opened the log --
    // and the verdict names scene_stress.log, which must be this run's, not the last run's CLEAN (the second review of
    // wave 2: its own loop had opened the log before any frame).
    void EnsureLog_()
    {
        if (gLog) { return; }
        gLog.Open("scene_stress.log", diag::ArtifactMode::PerRunTruncate);
        Log("scene lifecycle stress harness (a fault before its first frame was presented)\n");
    }

    // The first frame: the stress's log, crash filter and InfoQueue, and the sky / roughness runs' conditions.
    void Begin_()
    {
        gLog.Open("scene_stress.log", diag::ArtifactMode::PerRunTruncate);
        previousFilter_ = SetUnhandledExceptionFilter(StressCrashFilter); // symbolize worker-thread/WndProc faults of the churn
        Boot().harness.sceneStressExit = 1;              // until the verdict: a run that ends another way is not CLEAN
        Log("scene lifecycle stress harness\n");
        Log("gbvContinue=%d\n", gbvContinue_ ? 1 : 0);
        Log("iterations=%d WITH_EDITOR=%d\n", iterations_,
#if WITH_EDITOR
            1
#else
            0
#endif
        );
        Log("boot: window + device + scene ready (initial level loaded) at %.1f s\n", boot::ElapsedMs() / 1000.0);
        boot::Dump("scene-stress: boot complete");
        SetupInfoQueue_();
        if (skyEdits_)
        {
            // Its measurement conditions, set over whatever the command line and the level said, without changing
            // authored settings.
            {
                threading::RoleScope renderRole(threading::ThreadRole::Render); // the renderer's targets, as afterFrame's requests
                renderer_.SetDlssMode(sl::DLSSMode::eOff);
                renderer_.SetRenderResolutionScale(1.0f);
            }
            vfx::g_windFreeze_game = true; vfx::g_windFrozenTime_game = 0.0f;
            scene_.SkyAtmosphereRef().mode = 1;
            scene_.SkyAtmosphereRef().environmentLighting = true;
            auto settings = scene_.GetRenderSettings();
            settings.reflectionSource = ReflectionSource::RT;
            scene_.SetRenderSettings(settings);
            LOG_INFO(logging::LogCategory::Render, "sky environment stress: native {}x{}, frozen wind; sun edits, hold, exposure, IBL/mode toggles; no idle waits",
                     renderer_.GetRenderWidth(), renderer_.GetRenderHeight());
        }
        if (roughnessEdits_)
        {
            if (!renderer_.IsRaytracingSupported())
            {
                return Fault_(-1, "roughness-setup", "RT unsupported; regression not exercised");
            }
            auto settings = scene_.GetRenderSettings();
            settings.reflectionSource = ReflectionSource::RT;
            scene_.SetRenderSettings(settings);
            Log("roughness regression: wind_test #722, RT, one distinct value per frame, no idle waits\n");
        }
        framesLeft_ = skyEdits_ ? 32 : 4;
        phase_ = Phase::Warmup;
    }

    // The next churn op, or the end of the churn.
    void NextStep_(harness::FrameContext& ctx)
    {
        if (iter_ >= iterations_)
        {
            RecordVisitEnd_(iterations_);
            LogVisitVerdict_();
            if (skyEdits_)
            {
                framesLeft_ = 30; // the trace's last frames ("sky-trace-drain")
                phase_ = Phase::Drain;
                return;
            }
            return Final_(ctx);
        }
        // Rotate op order across iterations so races surface under different
        // timings (the resize/level-reload interleave especially).
        const int opCount = static_cast<int>(Op::Count);
        op_ = static_cast<Op>((iter_ + iter_ / opCount) % opCount);
        opName_ = skyEdits_ ? "SkyEdit" : (roughnessEdits_ ? "RoughnessEdit" : OpName(op_));
        Log("[iter %d] op=%s begin\n", iter_, opName_);
        stepBegin_ = std::chrono::steady_clock::now();
        spawned_ = false;
        // The churn op under an SEH guard so a hard fault (e.g. access violation from a dead/removed device) is
        // attributed to this op instead of crashing the process with no verdict.
        if (!Guarded_(&SceneStressHarness::RunOp_)) { return Fault_(iter_, opName_, faultDetail_); }
        if (faultDetail_[0]) { return Fault_(iter_, opName_, faultDetail_); } // the op could not run (roughness: no sphere)
        opEnd_ = std::chrono::steady_clock::now();
        resizeDuringFrame_ = !skyEdits_ && !roughnessEdits_ && op_ == Op::ResizeWindow;
        expectedResizeWidth_ = 0;
        if (spawned_)
        {
            framesLeft_ = 2; // "editor-spawn"
            phase_ = Phase::Spawned;
            return;
        }
        framesLeft_ = framesAfterOp_;
        phase_ = Phase::Frames;
    }

    // Runs a churn op under a structured-exception guard. A hardware/driver fault surfacing as an SEH exception
    // (commonly ACCESS_VIOLATION off a removed device) is converted into a fault with the code recorded, so the harness
    // reports the op that triggered it rather than dying with an opaque OS fault. Kept free of C++ objects requiring
    // unwinding (the work lives in the member it calls).
    bool Guarded_(void (SceneStressHarness::*body)())
    {
        __try
        {
            (this->*body)();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            const unsigned long code = static_cast<unsigned long>(GetExceptionCode());
            std::snprintf(faultDetail_, sizeof(faultDetail_),
                          "structured exception 0x%08lX (device removed reason=0x%08X)",
                          code,
                          static_cast<unsigned>(renderer_.GetDevice()
                              ? renderer_.GetDevice()->GetDeviceRemovedReason() : E_FAIL));
            return false;
        }
    }

    // The step's frames are drawn: its checks, the GPU census, the level visit.
    bool EndStep_()
    {
        const auto now = std::chrono::steady_clock::now();
        boot::AddBucket("stress frames (after op)", std::chrono::duration<double, std::milli>(now - opEnd_).count(), OpName(op_));
        boot::AddBucket("stress iteration", std::chrono::duration<double, std::milli>(now - stepBegin_).count(), opName_);
        if (const char* reason = CheckFault_())
        {
            Fault_(iter_, opName_, reason);
            return false;
        }
        // The GPU census after every op (memory profiling plan M1): a record whose release
        // never comes back shows as `live` climbing across the run.
        gpu::CensusTotals census;
        gpu::ReadCensus(census);
        Log("[iter %d] op=%s ok | gpu %.0f/%.0f MB %u res %u heaps live %llu\n", iter_, opName_,
            static_cast<double>(census.all.bytes[0]) / (1024.0 * 1024.0),
            static_cast<double>(census.all.bytes[1]) / (1024.0 * 1024.0),
            census.all.resources, census.all.heaps,
            static_cast<unsigned long long>(census.created - census.released));
        TrackVisit_(iter_, !skyEdits_ && !roughnessEdits_ && (op_ == Op::ReloadLevel || op_ == Op::SwitchLevel));
        ++iter_;
        return true;
    }

    // ResizeWindow's frames (OnBeginFrame): two resize requests after BeginFrame must be deferred to the next one and
    // coalesced there -- the size this frame draws is unchanged by them, and the next frame takes the later alone.
    bool ResizeBoundary_()
    {
        const UINT width = renderer_.GetWidth();
        const UINT height = renderer_.GetHeight();
        const auto generation = renderer_.GetDeferredTargetsGeneration();
        const bool applied = !expectedResizeWidth_ || width == expectedResizeWidth_;
        renderer_.OnResize(width + 1, height);
        renderer_.OnResize(width + 2, height); // only the latest request must be applied
        expectedResizeWidth_ = width + 2;
        if (!applied || renderer_.GetWidth() != width ||
            renderer_.GetDeferredTargetsGeneration() != generation)
        {
            std::snprintf(faultDetail_, sizeof(faultDetail_),
                          "resize boundary regression: resize was not deferred/coalesced at BeginFrame");
            LOG_ERROR(logging::LogCategory::RenderRhi, "{}", faultDetail_);
            return false;
        }
        return true;
    }

    // The churn is over: drain, the end checks, the verdict.
    void Final_(harness::FrameContext& ctx)
    {
        // Drain the final in-flight frames before declaring success, too.
        renderer_.WaitForPreviousFrame();
        if (const char* reason = CheckFault_()) { return Fault_(iterations_, "drain", reason); }
        if (roughnessEdits_)
        {
            std::uint32_t asEnhanced = 0, asLegacy = 0;
            barriers::AsEmitStats(asEnhanced, asLegacy);
            const auto builds = asEnhanced + asLegacy - asEnhancedStart_ - asLegacyStart_;
            Log("roughness regression: AS barriers during edits=%u\n", builds);
            if (builds < static_cast<unsigned>(iterations_))
            {
                return Fault_(iterations_, "roughness-coverage", "RT stopped building; possible SSR fallback");
            }
        }
        rtaudit::Finished(0.0, /*now=*/true);
        threading::LogRoleCheckSummary();
        if (const std::uint32_t roleViolations = threading::RoleViolationCount())
        {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "%u thread role violations (see 'thread role:' errors)", roleViolations);
            return Fault_(iterations_, "thread-roles", detail);
        }
        LogBarrierEmits_();
        Log("verdict: CLEAN after %d iterations (total %.1f s)\n",
            iterations_, boot::ElapsedMs() / 1000.0);
        LOG_INFO(logging::LogCategory::App, "scene-stress verdict: CLEAN after {} iterations ({:.1f} s); details in logs/scene_stress.log",
                 iterations_, boot::ElapsedMs() / 1000.0);
        boot::Dump("scene-stress: iterations complete");
        exitCode_ = 0;
        Boot().harness.sceneStressExit = 0;
        phase_ = Phase::Done;
        ctx.running = false;
    }

    // A fault ends the run where it stands: the verdict and DRED, then the process terminated without the GPU teardown
    // (the device likely removed), as the stress always has.
    void Fault_(int iter, const char* op, const char* reason)
    {
        EnsureLog_();
        exitCode_ = FinishFault_(iter, op, reason);
        phase_ = Phase::Done;
        memtrace::Stop();
        Log("fault caught: skipping GPU teardown (device likely removed); terminating with exit code %d\n", exitCode_);
        gLog.Close();
        logging::Shutdown();
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode_));
    }

    // The step's churn op (the frames after it are App::Run's: framesAfterOp_ of them).
    void RunOp_()
    {
        const Op op = op_;
        const int iter = iter_;
        faultDetail_[0] = '\0';
        if (skyEdits_)
        {
            auto& sky = scene_.SkyAtmosphereRef();
            const int phase = iter % 64;
            sky.mode = phase >= 56 ? unsigned(phase & 1) : 1u;
            sky.environmentLighting = phase >= 48 && phase < 56 ? (phase & 1) != 0 : true;
            if (phase < 32)
            {
                const float angle = (5.0f + float(phase)) * 3.14159265358979323846f / 180.0f;
                scene_.SetSunToward(Math::float3(std::cos(angle)*0.6f, std::sin(angle), std::cos(angle)*0.8f)); // T3: a time of day's time
            }
            if (phase >= 40 && phase < 48)
                scene_.CameraExposureRef().compensationEv = float(phase - 44) * 0.125f;
            framesAfterOp_ = 1; // "sky-edit"
            return;
        }
        if (roughnessEdits_)
        {
#if WITH_EDITOR
            auto* object = scene_.FindEditorObject(722);
            auto* gb = object ? object->AsGBufferRenderable() : nullptr;
            if (!gb)
            {
                std::snprintf(faultDetail_, sizeof(faultDetail_), "wind_test sphere #722 missing");
                return;
            }
            // Exactly the inspector's live mutation, without recreating materials or flushing GPU.
            auto& params = gb->MaterialParamsRef();
            params.metalRough.y = 0.001f + 0.998f * float((iter * 173) % 10007) / 10006.0f;
            framesAfterOp_ = 1; // "roughness-edit"
            return;
#else
            std::snprintf(faultDetail_, sizeof(faultDetail_), "roughness regression requires WITH_EDITOR");
            return;
#endif
        }

        // Some ops interleave a resize tightly with the churn to shake out
        // a deferred-target recreate racing an in-flight frame.
        const bool tightResize = (iter % 3 == 0);
        if (tightResize && op != Op::ResizeWindow)
        {
            const auto resizeBegin = std::chrono::steady_clock::now();
            CycleResize_();
            boot::AddBucket("stress CycleResize_",
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - resizeBegin).count(),
                            OpName(op));
        }

        {
            const auto opBegin = std::chrono::steady_clock::now();
            PerformOp_(op, iter);
            boot::AddBucket("stress op (churn only)",
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - opBegin).count(),
                            OpName(op));
        }

        // Render K real frames to pump the pipeline (K rotates 2..4).
        framesAfterOp_ = 2 + (iter % 3);
    }

    void SetupInfoQueue_()
    {
        ID3D12Device* dev = renderer_.GetDevice();
        if (!dev)
        {
            return;
        }
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&infoQueue_))) && infoQueue_)
        {
            // The device was set up with break-on-severity ERROR/CORRUPTION. In
            // stress mode we want to CAPTURE such messages and report them, not
            // hard-break the process, so disable the breaks and poll instead.
            infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            infoQueue_->ClearStoredMessages();
            Log("info queue: attached (ERROR/CORRUPTION breaks disabled, polling enabled)\n");
        }
        else
        {
            Log("info queue: unavailable (non-debug device?) — relying on device-removed + exceptions\n");
        }
    }

    // Returns a fault description string if a fault is detected, else nullptr.
    const char* CheckFault_()
    {
        ID3D12Device* dev = renderer_.GetDevice();
        if (dev)
        {
            const HRESULT dr = dev->GetDeviceRemovedReason();
            if (FAILED(dr))
            {
                std::snprintf(faultDetail_, sizeof(faultDetail_),
                              "device removed: reason=0x%08X", static_cast<unsigned>(dr));
                return faultDetail_;
            }
        }

        if (infoQueue_)
        {
            const auto drainBegin = std::chrono::steady_clock::now();
            const UINT64 n = infoQueue_->GetNumStoredMessages();
            boot::AddCount("infoqueue messages drained", static_cast<long long>(n));
            const char* firstError = nullptr;
            for (UINT64 i = 0; i < n; ++i)
            {
                SIZE_T len = 0;
                if (FAILED(infoQueue_->GetMessage(i, nullptr, &len)) || len == 0)
                {
                    continue;
                }
                std::vector<uint8_t> buf(len);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (FAILED(infoQueue_->GetMessage(i, msg, &len)))
                {
                    continue;
                }
                if (msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                    msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
                {
                    const char* sev = msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : "ERROR";
                    // Log the FULL description (may be long, e.g. GBV) unabridged.
                    Log("InfoQueue %s (id=%d): %s\n", sev, static_cast<int>(msg->ID),
                        msg->pDescription ? msg->pDescription : "");
                    if (!firstError)
                    {
                        std::snprintf(faultDetail_, sizeof(faultDetail_),
                                      "d3d12 %s (id=%d): %.240s",
                                      sev, static_cast<int>(msg->ID),
                                      msg->pDescription ? msg->pDescription : "");
                        firstError = faultDetail_;
                    }
                }
            }
            infoQueue_->ClearStoredMessages();
            boot::AddBucket("InfoQueue drain",
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - drainBegin).count(),
                            "CheckFault_");
            // In continue-mode (GBV diagnostics) InfoQueue errors are logged but
            // NOT treated as fatal, so the run proceeds through the churn to the
            // actual device-hung — letting GBV annotate the frames leading up to
            // it. Otherwise the first InfoQueue error is the fault.
            if (firstError && !gbvContinue_)
            {
                return firstError;
            }
        }
        return nullptr;
    }

    // Barrier plan step 14. A CLEAN verdict proves nothing about a path that never RAN, and the
    // barrier work has exactly two such paths: the enhanced emission (off unless opted into) and
    // the acceleration-structure barrier, which is zero unless raytracing was enabled AND a
    // BLAS/TLAS actually built. Printed on both outcomes — on a fault it says which model was in
    // use when it happened.
    void LogBarrierEmits_()
    {
        std::uint32_t emitEnhanced = 0, emitLegacy = 0, asEnhanced = 0, asLegacy = 0;
        barriers::EmitStats(emitEnhanced, emitLegacy);
        barriers::AsEmitStats(asEnhanced, asLegacy);
        Log("barriers: emit enhanced=%u legacy=%u | as enhanced=%u legacy=%u\n",
            emitEnhanced, emitLegacy, asEnhanced, asLegacy);
        barriers::DumpCensus("barrier_census.log"); // step 16; no-op unless --barrier-census
    }

    int FinishFault_(int iter, const char* op, const char* reason)
    {
        faultCaught_ = true;
        Log("[iter %d] op=%s FAULT: %s\n", iter, op, reason);
        LogBarrierEmits_();
        Log("verdict: FAULT op=%s iter=%d detail=%s\n", op, iter, reason);
        LOG_ERROR(logging::LogCategory::App,
                  "scene-stress verdict: FAULT op={} iter={} detail={}; details in logs/scene_stress.log, DRED in logs/dred_dump.txt",
                  op, iter, reason);

        // Ground truth on WHAT was executing / faulting: dump DRED (breadcrumbs
        // + page-fault allocations) to scene_stress.log and dred_dump.txt.
        DumpDred_(iter, op);
        return 2;
    }

    static const char* DredOpName_(D3D12_AUTO_BREADCRUMB_OP op)
    {
        switch (op)
        {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SETMARKER";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BEGINEVENT";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "ENDEVENT";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DRAWINSTANCED";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DRAWINDEXEDINSTANCED";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "EXECUTEINDIRECT";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "DISPATCH";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "COPYBUFFERREGION";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "COPYTEXTUREREGION";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "COPYRESOURCE";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "COPYTILES";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "RESOLVESUBRESOURCE";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "CLEARRENDERTARGETVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "CLEARUNORDEREDACCESSVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "CLEARDEPTHSTENCILVIEW";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "RESOURCEBARRIER";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "EXECUTEBUNDLE";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "PRESENT";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "RESOLVEQUERYDATA";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DISPATCHRAYS";
        case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BUILD_RTAS";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "COPY_RTAS";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DISPATCHMESH";
        default: return "OP";
        }
    }

    static const char* DredAllocTypeName_(D3D12_DRED_ALLOCATION_TYPE t)
    {
        switch (t)
        {
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "COMMAND_QUEUE";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "COMMAND_ALLOCATOR";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "PIPELINE_STATE";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "COMMAND_LIST";
        case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "FENCE";
        case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "DESCRIPTOR_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "QUERY_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "COMMAND_SIGNATURE";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return "PIPELINE_LIBRARY";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER: return "VIDEO_DECODER";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_PROCESSOR: return "VIDEO_PROCESSOR";
        case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "RESOURCE";
        case D3D12_DRED_ALLOCATION_TYPE_PASS: return "PASS";
        case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSION: return "CRYPTOSESSION";
        case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSIONPOLICY: return "CRYPTOSESSIONPOLICY";
        case D3D12_DRED_ALLOCATION_TYPE_PROTECTEDRESOURCESESSION: return "PROTECTEDRESOURCESESSION";
        case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER_HEAP: return "VIDEO_DECODER_HEAP";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_POOL: return "COMMAND_POOL";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_RECORDER: return "COMMAND_RECORDER";
        case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT: return "STATE_OBJECT";
        case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND: return "METACOMMAND";
        case D3D12_DRED_ALLOCATION_TYPE_SCHEDULINGGROUP: return "SCHEDULINGGROUP";
        default: return "OTHER";
        }
    }

    // Resolve a DRED object name, preferring the narrow variant, else converting
    // the wide one. Writes "(unnamed)" if both are absent.
    static void NameToNarrow_(const char* a, const wchar_t* w, char* out, size_t outSize)
    {
        if (a && a[0]) { std::snprintf(out, outSize, "%s", a); }
        else if (w && w[0]) { std::snprintf(out, outSize, "%ls", w); }
        else { std::snprintf(out, outSize, "(unnamed)"); }
    }

    // Dual-log helper: write to scene_stress.log (via Log) and the DRED file.
    void DredLine_(const char* fmt, ...)
    {
        char line[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        Log("%s", line);
        dredFile_.Write(line);
    }

    void DumpDred_(int iter, const char* op)
    {
        ID3D12Device* dev = renderer_.GetDevice();
        if (!dev)
        {
            Log("DRED: no device to query\n");
            return;
        }

        dredFile_.Open("dred_dump.txt", diag::ArtifactMode::PerRunTruncate);

        DredLine_("==== DRED dump (fault op=%s iter=%d) ====\n", op, iter);
        DredLine_("GetDeviceRemovedReason=0x%08X\n",
                  static_cast<unsigned>(dev->GetDeviceRemovedReason()));

        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred0;
        const bool have1 = SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dred1)));
        if (!have1)
        {
            if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred0))))
            {
                DredLine_("DRED: DeviceRemovedExtendedData interface unavailable (DRED not enabled or unsupported)\n");
                dredFile_.Close();
                return;
            }
        }

        // ---- Auto-breadcrumbs: which command list/queue + op was executing ----
        DumpBreadcrumbs_(dred1.Get(), dred0.Get(), have1);

        // ---- Page-fault allocations: existing + recently-freed at the fault VA --
        DumpPageFault_(dred1.Get(), dred0.Get(), have1);

        DredLine_("==== end DRED dump ====\n");
        dredFile_.Close();
    }

    void DumpBreadcrumbs_(ID3D12DeviceRemovedExtendedData1* dred1,
                          ID3D12DeviceRemovedExtendedData* dred0, bool have1)
    {
        const D3D12_AUTO_BREADCRUMB_NODE1* node1 = nullptr;
        const D3D12_AUTO_BREADCRUMB_NODE* node0 = nullptr;

        if (have1)
        {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 out{};
            if (FAILED(dred1->GetAutoBreadcrumbsOutput1(&out)))
            {
                DredLine_("breadcrumbs: GetAutoBreadcrumbsOutput1 failed\n");
                return;
            }
            node1 = out.pHeadAutoBreadcrumbNode;
        }
        else
        {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT out{};
            if (FAILED(dred0->GetAutoBreadcrumbsOutput(&out)))
            {
                DredLine_("breadcrumbs: GetAutoBreadcrumbsOutput failed\n");
                return;
            }
            node0 = out.pHeadAutoBreadcrumbNode;
        }

        DredLine_("---- auto-breadcrumbs (per command list) ----\n");
        int nodeCount = 0;
        for (; node1 || node0; )
        {
            const char* clNameA = nullptr;
            const wchar_t* clNameW = nullptr;
            const char* queueNameA = nullptr;
            const wchar_t* queueNameW = nullptr;
            const D3D12_AUTO_BREADCRUMB_OP* history = nullptr;
            UINT32 historyCount = 0;
            const UINT32* lastValue = nullptr;
            UINT32 ctxCount = 0;
            const D3D12_DRED_BREADCRUMB_CONTEXT* contexts = nullptr;

            if (node1)
            {
                clNameA = node1->pCommandListDebugNameA;
                clNameW = node1->pCommandListDebugNameW;
                queueNameA = node1->pCommandQueueDebugNameA;
                queueNameW = node1->pCommandQueueDebugNameW;
                history = node1->pCommandHistory;
                historyCount = node1->BreadcrumbCount;
                lastValue = node1->pLastBreadcrumbValue;
                ctxCount = node1->BreadcrumbContextsCount;
                contexts = node1->pBreadcrumbContexts;
            }
            else
            {
                clNameA = node0->pCommandListDebugNameA;
                clNameW = node0->pCommandListDebugNameW;
                queueNameA = node0->pCommandQueueDebugNameA;
                queueNameW = node0->pCommandQueueDebugNameW;
                history = node0->pCommandHistory;
                historyCount = node0->BreadcrumbCount;
                lastValue = node0->pLastBreadcrumbValue;
            }

            // Command-list / queue names in this engine are set via the WIDE
            // SetName, so prefer the W variants (the A variants are null).
            char clName[160];
            char queueName[96];
            NameToNarrow_(clNameA, clNameW, clName, sizeof(clName));
            NameToNarrow_(queueNameA, queueNameW, queueName, sizeof(queueName));

            const UINT32 completed = lastValue ? *lastValue : 0;

            // Only report nodes that were mid-execution (completed < total): a
            // fully-completed list did not cause the hang. Also always report
            // the first few for context.
            const bool interesting = (historyCount > 0) && (completed < historyCount);
            if (interesting || nodeCount < 3)
            {
                DredLine_("[node %d] queue=\"%s\" cmdlist=\"%s\" ops=%u completed=%u %s\n",
                          nodeCount, queueName, clName, historyCount, completed,
                          interesting ? "<-- MID-EXECUTION (the op at index=completed was executing when the GPU stopped)" : "");

                if (interesting && history && historyCount > 0)
                {
                    // Dump the whole op history for a mid-execution list (they are
                    // short) so the stuck op and its surroundings are unambiguous.
                    for (UINT32 i = 0; i < historyCount; ++i)
                    {
                        const char* mark = (i == completed) ? " <== EXECUTING / STUCK (not yet completed)" : "";
                        DredLine_("    op[%u]=%s%s\n", i, DredOpName_(history[i]), mark);
                    }
                    for (UINT32 c = 0; c < ctxCount && contexts; ++c)
                    {
                        if (contexts[c].pContextString)
                        {
                            char narrow[256];
                            std::snprintf(narrow, sizeof(narrow), "%ls", contexts[c].pContextString);
                            DredLine_("    context@op[%u]: %s\n",
                                      contexts[c].BreadcrumbIndex, narrow);
                        }
                    }
                }
            }

            ++nodeCount;
            if (node1) { node1 = node1->pNext; }
            else { node0 = node0->pNext; }
            if (nodeCount > 512) { DredLine_("(breadcrumb list truncated)\n"); break; }
        }
        DredLine_("(total breadcrumb nodes: %d)\n", nodeCount);
    }

    void DumpPageFault_(ID3D12DeviceRemovedExtendedData1* dred1,
                        ID3D12DeviceRemovedExtendedData* dred0, bool have1)
    {
        D3D12_GPU_VIRTUAL_ADDRESS faultVA = 0;
        const D3D12_DRED_ALLOCATION_NODE1* existing1 = nullptr;
        const D3D12_DRED_ALLOCATION_NODE1* freed1 = nullptr;
        const D3D12_DRED_ALLOCATION_NODE* existing0 = nullptr;
        const D3D12_DRED_ALLOCATION_NODE* freed0 = nullptr;

        if (have1)
        {
            D3D12_DRED_PAGE_FAULT_OUTPUT1 out{};
            if (FAILED(dred1->GetPageFaultAllocationOutput1(&out)))
            {
                DredLine_("page-fault: GetPageFaultAllocationOutput1 failed (no page fault recorded — likely pure TDR)\n");
                return;
            }
            faultVA = out.PageFaultVA;
            existing1 = out.pHeadExistingAllocationNode;
            freed1 = out.pHeadRecentFreedAllocationNode;
        }
        else
        {
            D3D12_DRED_PAGE_FAULT_OUTPUT out{};
            if (FAILED(dred0->GetPageFaultAllocationOutput(&out)))
            {
                DredLine_("page-fault: GetPageFaultAllocationOutput failed (no page fault recorded — likely pure TDR)\n");
                return;
            }
            faultVA = out.PageFaultVA;
            existing0 = out.pHeadExistingAllocationNode;
            freed0 = out.pHeadRecentFreedAllocationNode;
        }

        DredLine_("---- page fault ----\n");
        DredLine_("PageFaultVA=0x%llX\n", static_cast<unsigned long long>(faultVA));
        if (faultVA == 0)
        {
            DredLine_("(no page-fault VA — the removal was likely a pure TDR/hang with no invalid access recorded)\n");
        }

        DredLine_("-- existing allocations at/around fault VA --\n");
        DumpAllocNodes_(existing1, existing0, have1);
        DredLine_("-- RECENTLY FREED allocations (a match here = use-after-free) --\n");
        DumpAllocNodes_(freed1, freed0, have1);
    }

    void DumpAllocNodes_(const D3D12_DRED_ALLOCATION_NODE1* node1,
                         const D3D12_DRED_ALLOCATION_NODE* node0, bool have1)
    {
        int count = 0;
        while (node1 || node0)
        {
            const char* nameA = nullptr;
            const wchar_t* nameW = nullptr;
            D3D12_DRED_ALLOCATION_TYPE type{};
            if (have1)
            {
                nameA = node1->ObjectNameA;
                nameW = node1->ObjectNameW;
                type = node1->AllocationType;
            }
            else
            {
                nameA = node0->ObjectNameA;
                nameW = node0->ObjectNameW;
                type = node0->AllocationType;
            }
            char name[192];
            NameToNarrow_(nameA, nameW, name, sizeof(name));
            DredLine_("    [%d] type=%s name=\"%s\"\n", count,
                      DredAllocTypeName_(type), name);
            ++count;
            if (have1) { node1 = node1->pNext; }
            else { node0 = node0->pNext; }
            if (count > 4096) { DredLine_("    (allocation list truncated)\n"); break; }
        }
        if (count == 0) { DredLine_("    (none)\n"); }
    }

    void PerformOp_(Op op, int iter)
    {
        switch (op)
        {
        case Op::ReloadLevel:
        {
            // Reload the current level via the request path (teardown + rebuild:
            // LightManager::Reset, deferred usage, FinalizeLevelLoad).
            levelManager_.RequestLevelPathChange(CurrentLevelPath_());
            break;
        }
        case Op::SwitchLevel:
        {
            // Live-switch among the three demo levels (the Step 19 path) and the --stress-level one, if any.
            nextLevelIndex_ = (nextLevelIndex_ + 1) % LevelCount_();
            levelManager_.RequestLevelPathChange(CurrentLevelPath_());
            break;
        }
        case Op::ResizeWindow:
        {
            CycleResize_();
            break;
        }
        case Op::DlssMode:
        {
            threading::RoleScope renderRole(threading::ThreadRole::Render); // as afterFrame's dlss.mode / aa.renderScale requests
            // Rotate DLSS modes — each render-resolution change recreates the
            // deferred targets. No-ops gracefully if DLSS is unavailable.
            static const sl::DLSSMode kModes[] = {
                sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance,
                sl::DLSSMode::eMaxQuality, sl::DLSSMode::eOff };
            dlssModeIndex_ = (dlssModeIndex_ + 1) % static_cast<int>(std::size(kModes));
            renderer_.SetDlssMode(kModes[dlssModeIndex_]);
            break;
        }
        case Op::RenderScale:
        {
            threading::RoleScope renderRole(threading::ThreadRole::Render); // as afterFrame's dlss.mode / aa.renderScale requests
            static const float kScales[] = { 1.0f, 0.75f, 0.5f, 0.85f };
            renderScaleIndex_ = (renderScaleIndex_ + 1) % static_cast<int>(std::size(kScales));
            renderer_.SetRenderResolutionScale(kScales[renderScaleIndex_]);
            break;
        }
        case Op::ReflectionScale:
        {
            threading::RoleScope renderRole(threading::ThreadRole::Render); // as afterFrame's dlss.mode / aa.renderScale requests
            static const float kScales[] = { 0.5f, 0.25f, 0.75f, 1.0f };
            reflectionScaleIndex_ = (reflectionScaleIndex_ + 1) % static_cast<int>(std::size(kScales));
            renderer_.SetReflectionTextureScale(kScales[reflectionScaleIndex_]);
            break;
        }
#if WITH_EDITOR
        case Op::EditorSpawnDelete:
        {
            Spawn_();
            break;
        }
#endif
        default:
            break;
        }
    }

    // Resize the OS window to the next size in the rotation. The message-driven and direct
    // requests coalesce at BeginFrame, before the new swapchain slot's pools are recycled.
    void CycleResize_()
    {
        static const struct { UINT w; UINT h; } kSizes[] = {
            { 1280, 720 }, { 1920, 1080 }, { 800, 600 }, { 2560, 1440 }, { 1024, 768 } };
        resizeIndex_ = (resizeIndex_ + 1) % static_cast<int>(std::size(kSizes));
        const UINT w = kSizes[resizeIndex_].w;
        const UINT h = kSizes[resizeIndex_].h;

        if (hWnd_)
        {
            RECT rect{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
            AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hWnd_, nullptr, 0, 0,
                         rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // Queue directly too, in case the OS did not send a notification.
        renderer_.OnResize(w, h);
    }

#if WITH_EDITOR
    // Mirrors SpawnMeshCommand/DeleteObjectCommand: spawn an initialized editor
    // object, two frames with it live, then delete it (DeleteSpawned_). Exercises the editor
    // scene-mutation path (AddInitializedEditorObject / RemoveEditorObject with
    // a full GPU sync around each mutation).
    void Spawn_()
    {
        nlohmann::json o = nlohmann::json::object();
        o["type"] = "staticMesh";
        o["model"] = "models/box/box.mesh.bin";
        o["material"] = "damaged_plaster";
        o["shader"] = "shaders/gbuffer.hlsl";
        o["inputLayout"] = "PosNormTanUV";
        const Math::float3 camPos = scene_.CameraRef().GetPosition();
        const Math::float3 camDir = scene_.CameraRef().GetDirection();
        o["position"] = nlohmann::json::array({
            camPos.x + camDir.x * 5.0f, camPos.y + camDir.y * 5.0f, camPos.z + camDir.z * 5.0f });
        o["scale"] = nlohmann::json::array({ 1.0f, 1.0f, 1.0f });

        std::unique_ptr<RenderableObjectBase> runtime = SceneObjectFactory::CreateStaticMeshFromJson(o);
        if (!runtime)
        {
            return;
        }

        const Scene::SceneObjectId id = nextEditorStressId_++;
        renderer_.WaitForPreviousFrame();
        UploadBatch uploads;
        bool added = false;
        if (uploads.Begin(&renderer_))
        {
            added = scene_.AddInitializedEditorObject(renderer_, uploads, id, std::move(runtime), true);
            uploads.SubmitAndWait(&renderer_);
        }
        if (!added)
        {
            return;
        }
        // Mirror the editor command path (EditorCommandStack): rebuild the shadow-caster GPU data +
        // mega VB/IB after the caster-set change, else VSM's per-page draw falls into the per-group
        // fallback. Also gives GBV coverage of the mid-game EnsureMegaBuffer.
        scene_.RefreshShadowGpuForEditor(renderer_);

        // Render a couple of frames with the object live, then delete it.
        spawnedId_ = id;
        spawned_ = true; // NextStep_: two frames with it, then DeleteSpawned_
    }

    void DeleteSpawned_()
    {
        renderer_.WaitForPreviousFrame();
        const std::size_t before = scene_.ObjectCount();
        scene_.RemoveEditorObject(spawnedId_);
        const std::size_t removed = before - scene_.ObjectCount();
        scene_.RefreshShadowGpuForEditor(renderer_);
        if (removed != 1)
        {
            std::snprintf(faultDetail_, sizeof(faultDetail_), "editor delete of id %llu removed %zu objects (one expected)",
                          static_cast<unsigned long long>(spawnedId_), removed);
        }
    }
#else
    void DeleteSpawned_() {}
#endif

    // Memory profiling plan M6: a level's tags at the end of each visit against its earlier visits. A
    // visit ends where the next level op begins; the snapshot is the one taken after the iteration
    // before it, when the previous level's retired resources have long left. A visit of one
    // iteration is skipped -- 2-4 frames after a load the retire bins still hold the last level.
    //
    // A drift is memory a level leaves behind EVERY time it is loaded, so the verdict looks for a tag
    // that climbs from visit to visit, not one that differs from some baseline. Measured on two
    // 150-iteration runs: a level's first visit is cold (tex.stream 2 / 46 / 95 MB lower -- the 50 MB
    // upload ring is created by the first stream-in; mesh and tex.material shifted by shared caches)
    // and is not compared; after it a tag may still settle in one step (tex.stream +2.00 MB on new1
    // from its third visit to its twelfth, -0.75 MB on demo1) -- a step, which a "differs from the
    // second visit" rule called a drift. Tags sized by the render or display resolution are left
    // out: the churn's resize, DLSS and scale ops move them by hundreds of MB between visits.
    static bool ResolutionSized_(std::uint32_t t)
    {
        const MemTag tag = static_cast<MemTag>(t);
        const MemTag parent = MemTagParent(tag);
        return tag == MemTag::Frame || parent == MemTag::Frame || tag == MemTag::Reflect || tag == MemTag::Ao ||
               tag == MemTag::Bloom || tag == MemTag::Fog || tag == MemTag::Shafts || tag == MemTag::Cloud ||
               tag == MemTag::Exposure || tag == MemTag::Dlss || tag == MemTag::Vis;
    }

    void TrackVisit_(int iter, bool levelOp)
    {
        std::vector<std::int64_t> tags;
        memgate::ReadTagBytes(tags);
        if (levelOp)
        {
            RecordVisitEnd_(iter);
            visitLevel_.clear();
        }
        if (visitLevel_.empty())
        {
            visitLevel_ = std::string(levelManager_.GetActiveLevelSourcePath());
            visitIters_ = 0;
        }
        ++visitIters_;
        visitTags_ = std::move(tags);
    }

    void RecordVisitEnd_(int iter)
    {
        if (visitLevel_.empty() || visitTags_.empty()) { return; }
        if (visitIters_ < 2)
        {
            ++shortVisits_;
            return;
        }
        LevelVisits& lv = levelVisits_[visitLevel_];
        ++lv.seen;
        if (lv.seen == 1)
        {
            Log("[iter %d] mem tags: %s first visit (cold, not compared; %d iterations)\n", iter, visitLevel_.c_str(), visitIters_);
            return;
        }
        lv.visits.push_back(visitTags_);
        if (lv.visits.size() == 1) { return; }
        const std::vector<std::int64_t>& prev = lv.visits[lv.visits.size() - 2];
        std::string diffs;
        for (std::uint32_t t = 0; t < kMemTagCount; ++t)
        {
            if (ResolutionSized_(t)) { continue; }
            const std::int64_t d = visitTags_[t] - prev[t];
            if (d >= kVisitNoteBytes || d <= -kVisitNoteBytes)
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), " %s %+.2f MB", kMemTagNames[t], static_cast<double>(d) / (1024.0 * 1024.0));
                diffs += buf;
            }
        }
        Log("[iter %d] mem tags vs the previous visit of %s (visit %zu, %d iterations):%s\n", iter, visitLevel_.c_str(),
            lv.visits.size() + 1, visitIters_, diffs.empty() ? " same" : diffs.c_str());
    }

    // The verdict, per level and tag (resolution-sized tags excepted): DRIFT when the level has at
    // least 3 steps between compared visits, the tag grew by kVisitStepBytes or more in two thirds
    // of them, and ended kVisitDriftBytes or more above where it started. A Warning, not a fault:
    // the run's exit code stays the churn's (crash) verdict.
    void LogVisitVerdict_()
    {
        std::string drift;
        int judged = 0, steps = 0;
        double largest = 0.0;
        std::string largestText = "none";
        for (const auto& [level, lv] : levelVisits_)
        {
            const std::size_t n = lv.visits.size();
            if (n < 4) { continue; }
            ++judged;
            steps += static_cast<int>(n - 1);
            for (std::uint32_t t = 0; t < kMemTagCount; ++t)
            {
                if (ResolutionSized_(t)) { continue; }
                std::size_t grew = 0;
                for (std::size_t i = 1; i < n; ++i)
                {
                    if (lv.visits[i][t] - lv.visits[i - 1][t] >= kVisitStepBytes) { ++grew; }
                }
                const std::int64_t total = lv.visits[n - 1][t] - lv.visits[0][t];
                const double totalMb = static_cast<double>(total) / (1024.0 * 1024.0);
                char buf[192];
                std::snprintf(buf, sizeof(buf), "%s %+.2f MB on %s (grew in %zu of %zu steps)", kMemTagNames[t], totalMb, level.c_str(), grew, n - 1);
                if (totalMb > largest)
                {
                    largest = totalMb;
                    largestText = buf;
                }
                if (total >= kVisitDriftBytes && grew * 3 >= (n - 1) * 2)
                {
                    drift += " ";
                    drift += buf;
                }
            }
        }
        if (judged == 0)
        {
            // Silence here would be a false STABLE: nothing was compared (60 iterations leave every
            // level short of 4 compared visits; 150 give each 6-10).
            Log("mem tags: NOT JUDGED -- no level reached 4 compared visits (%d short visits skipped); run 150 iterations or more\n", shortVisits_);
            LOG_WARNING(logging::LogCategory::App, "scene-stress mem tags: NOT JUDGED -- no level reached 4 compared visits; run 150 iterations or more");
        }
        else if (drift.empty())
        {
            Log("mem tags: STABLE -- %d levels judged over %d steps (%d short visits skipped); largest climb %s; resolution-sized tags not compared\n",
                judged, steps, shortVisits_, largestText.c_str());
            LOG_INFO(logging::LogCategory::App, "scene-stress mem tags: STABLE -- {} levels judged over {} steps, largest climb {}",
                     judged, steps, largestText);
        }
        else
        {
            Log("mem tags: DRIFT --%s\n", drift.c_str());
            LOG_WARNING(logging::LogCategory::App, "scene-stress mem tags: DRIFT --{}; details in logs/scene_stress.log", drift);
        }
    }

    // The churn's levels: the three demo levels where they are (the tree), else -- a packaged Development build holds only
    // its profile's levels (docs/scalability_plan.md §7.4) -- the first three of the packages' data/levels; then
    // --stress-level's. Found once, at the first ask.
    const std::vector<std::string>& Levels_() const
    {
        if (levels_.empty())
        {
            for (const char* level : kLevels_)
            {
                if (vfs::Exists(level)) { levels_.push_back(level); }
            }
            if (levels_.empty())
            {
                std::vector<std::string> packaged;
                vfs::List("data/levels", "*.json", [&packaged](const vfs::ListEntry& e)
                {
                    if (!e.directory) { packaged.push_back("data/levels/" + e.name); }
                });
                std::sort(packaged.begin(), packaged.end());
                for (std::size_t i = 0; i < packaged.size() && i < 3; ++i) { levels_.push_back(packaged[i]); }
                LOG_INFO(logging::LogCategory::App, "scene-stress: the demo levels are not here -- the packages' {} level(s)", levels_.size());
            }
            if (!Boot().harness.sceneStressExtraLevel.empty()) { levels_.push_back(Boot().harness.sceneStressExtraLevel); }
            if (levels_.empty()) { levels_.push_back(kLevels_[0]); } // nothing found: the load says why
        }
        return levels_;
    }
    int LevelCount_() const { return static_cast<int>(Levels_().size()); }
    std::string CurrentLevelPath_() const { return Levels_()[static_cast<std::size_t>(nextLevelIndex_ % LevelCount_())]; }
    mutable std::vector<std::string> levels_;

    HWND hWnd_ = nullptr;
    Renderer& renderer_;
    Scene& scene_;
    LevelManager& levelManager_;
    int iterations_ = 0;
    bool gbvContinue_ = false;
    bool skyEdits_ = false;
    bool roughnessEdits_ = false;

    ComPtr<ID3D12InfoQueue> infoQueue_;
    diag::ArtifactFile dredFile_;
    bool faultCaught_ = false;
    char faultDetail_[320] = {};
    // The churn's state, advanced once a frame (OnFrame).
    Phase phase_ = Phase::Start;
    int framesLeft_ = 0;        // frames still to draw in this phase
    int framesAfterOp_ = 0;     // the step's frames after its op (2-4; 1 for a sky / roughness edit)
    int iter_ = 0;
    Op op_ = Op::ReloadLevel;
    const char* opName_ = "";
    bool resizeDuringFrame_ = false;
    UINT expectedResizeWidth_ = 0;
    bool spawned_ = false;
#if WITH_EDITOR
    Scene::SceneObjectId spawnedId_ = 0;
#endif
    int exitCode_ = 1;
    LPTOP_LEVEL_EXCEPTION_FILTER previousFilter_ = nullptr;
    std::uint32_t asEnhancedStart_ = 0;
    std::uint32_t asLegacyStart_ = 0;
    std::chrono::steady_clock::time_point stepBegin_{};
    std::chrono::steady_clock::time_point opEnd_{};

    int nextLevelIndex_ = Boot().harness.sceneStressExtraLevel.empty() ? 0 : 3; // --stress-level boots into the extra level
    int resizeIndex_ = 0;
    int dlssModeIndex_ = 0;
    int renderScaleIndex_ = 0;
    int reflectionScaleIndex_ = 0;
#if WITH_EDITOR
    // The spawned boxes' ids: past any level's (a document numbers its objects from 1, and RemoveEditorObject removes
    // every object of an id -- from 1 each delete took a level object along with the box).
    Scene::SceneObjectId nextEditorStressId_ = Scene::SceneObjectId(1) << 40;
#endif

    static constexpr std::int64_t kVisitNoteBytes = 256 * 1024;      // logged per comparison
    static constexpr std::int64_t kVisitStepBytes = 64 * 1024;       // a step that counts as growth
    static constexpr std::int64_t kVisitDriftBytes = 1024 * 1024;    // the climb the verdict needs
    struct LevelVisits
    {
        int seen = 0;                                  // visits recorded; the first (cold) is not kept
        std::vector<std::vector<std::int64_t>> visits; // tags at the end of each compared visit
    };
    std::string visitLevel_;
    int visitIters_ = 0;
    std::vector<std::int64_t> visitTags_;
    std::map<std::string, LevelVisits> levelVisits_;
    int shortVisits_ = 0;

    static constexpr const char* kLevels_[3] = {
        "data/levels/demo.json", "data/levels/demo1.json", "data/levels/new1.json" };
};

constexpr const char* SceneStressHarness::kLevels_[3];

} // namespace

namespace harness
{
std::unique_ptr<Harness> MakeSceneStressHarness(HWND__* window, Renderer& renderer, Scene& scene, LevelManager& levelManager)
{
    return std::make_unique<SceneStressHarness>(window, renderer, scene, levelManager);
}
}
