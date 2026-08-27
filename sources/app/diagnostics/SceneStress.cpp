#include "app/diagnostics/SceneStress.h"

#include "app/App.h"
#include "core/math/Math.h"
#include "core/task/TaskSystem.h"
#include "core/profiling/Profiler.h"

#include "app/levels/JsonLevel.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "rendering/core/BarrierTranslation.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"

#if WITH_EDITOR
#include "app/scene/SceneObjectFactory.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "third_party/json/json.hpp"
#endif

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h> // ID3D12InfoQueue
#include "core/diagnostics/DiagPaths.h"
#include <dbghelp.h>        // StackWalk64 / SymFromAddr (crash-stack logger)
#include <wrl/client.h>

#pragma comment(lib, "dbghelp.lib")

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
FILE* gLog = nullptr;

void Log(const char* fmt, ...)
{
    if (!gLog)
    {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(gLog, fmt, args);
    va_end(args);
    fflush(gLog);
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

    FILE* cf = nullptr;
    fopen_s(&cf, diag::LogPath("crash_stack.txt").c_str(), "w");
    auto emit = [&](const char* fmt, ...)
    {
        char line[1024];
        va_list a; va_start(a, fmt);
        std::vsnprintf(line, sizeof(line), fmt, a);
        va_end(a);
        if (cf) { std::fputs(line, cf); std::fflush(cf); }
        Log("%s", line);
    };

    emit("==== UNHANDLED EXCEPTION code=0x%08lX tid=%lu ====\n", code, GetCurrentThreadId());
    if (ep && ep->ExceptionRecord && code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
        emit("access-violation %s VA=0x%llX\n",
             ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
             static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
    }

    if (!ep || !ep->ContextRecord) { if (cf) { std::fclose(cf); } return EXCEPTION_EXECUTE_HANDLER; }

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
    if (cf) { std::fclose(cf); }
    if (gLog) { std::fflush(gLog); }
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

// Drives the real render/scene loop autonomously and hammers the suspect
// scene-lifecycle operations. Holds no ownership: the App owns the systems.
class SceneStressDriver
{
public:
    SceneStressDriver(HWND hWnd, Renderer& renderer, Scene& scene,
                      LevelManager& levelManager, int iterations, bool gbvContinue, bool roughnessEdits)
        : hWnd_(hWnd)
        , renderer_(renderer)
        , scene_(scene)
        , levelManager_(levelManager)
        , iterations_(iterations)
        , gbvContinue_(gbvContinue)
        , roughnessEdits_(roughnessEdits)
    {
    }

    // Returns process exit code: 0 clean, nonzero fault.
    int Run()
    {
        SetupInfoQueue_();
        if (roughnessEdits_)
        {
            if (!renderer_.IsRaytracingSupported())
            {
                return FinishFault_(-1, "roughness-setup", "RT unsupported; regression not exercised");
            }
            auto settings = scene_.GetRenderSettings();
            settings.reflectionSource = ReflectionSource::RT;
            scene_.SetRenderSettings(settings);
            Log("roughness regression: wind_test #722, RT, one distinct value per frame, no idle waits\n");
        }

        // Pump a few warm-up frames so the pipeline is fully primed before churn.
        for (int i = 0; i < 4; ++i)
        {
            if (!RenderFrames_(1, "warmup"))
            {
                return FinishFault_(-1, "warmup", "render-frame threw");
            }
        }
        if (const char* early = CheckFault_())
        {
            return FinishFault_(-1, "warmup", early);
        }

        std::uint32_t asEnhancedStart = 0, asLegacyStart = 0;
        barriers::AsEmitStats(asEnhancedStart, asLegacyStart);
        for (int iter = 0; iter < iterations_ && !faultCaught_; ++iter)
        {
            // Rotate op order across iterations so races surface under different
            // timings (the resize/level-reload interleave especially).
            const int opCount = static_cast<int>(Op::Count);
            const Op op = static_cast<Op>((iter + iter / opCount) % opCount);

            const char* opName = roughnessEdits_ ? "RoughnessEdit" : OpName(op);
            Log("[iter %d] op=%s begin\n", iter, opName);

            // Do the churn op + frames under an SEH guard so a hard fault
            // (e.g. access violation from a dead/removed device) is attributed
            // to this op instead of crashing the process with no verdict. The
            // returned status distinguishes the failure modes.
            const StepStatus status = RunStep_(op, iter);
            switch (status)
            {
            case StepStatus::Ok:
                break;
            case StepStatus::FrameThrew:
                return FinishFault_(iter, opName, faultDetail_[0] ? faultDetail_ : "render-frame threw");
            case StepStatus::HardFault:
                return FinishFault_(iter, opName, faultDetail_);
            }

            if (const char* reason = CheckFault_())
            {
                return FinishFault_(iter, opName, reason);
            }

            Log("[iter %d] op=%s ok\n", iter, opName);
        }

        // Drain the final in-flight frames before declaring success, too.
        renderer_.WaitForPreviousFrame();
        if (const char* reason = CheckFault_()) { return FinishFault_(iterations_, "drain", reason); }
        if (roughnessEdits_)
        {
            std::uint32_t asEnhanced = 0, asLegacy = 0;
            barriers::AsEmitStats(asEnhanced, asLegacy);
            const auto builds = asEnhanced + asLegacy - asEnhancedStart - asLegacyStart;
            Log("roughness regression: AS barriers during edits=%u\n", builds);
            if (builds < static_cast<unsigned>(iterations_))
            {
                return FinishFault_(iterations_, "roughness-coverage", "RT stopped building; possible SSR fallback");
            }
        }
        LogBarrierEmits_();
        Log("verdict: CLEAN after %d iterations\n", iterations_);
        if (gLog) { fflush(gLog); }
        return 0;
    }

    bool FaultCaught() const { return faultCaught_; }

private:
    enum class StepStatus { Ok, FrameThrew, HardFault };

    // Runs one churn step under a structured-exception guard. A hardware/driver
    // fault surfacing as an SEH exception (commonly ACCESS_VIOLATION off a
    // removed device) is converted into StepStatus::HardFault with the code
    // recorded, so the harness reports the op that triggered it and exits with
    // its own code rather than dying with an opaque OS fault. Kept free of C++
    // objects requiring unwinding (the actual work lives in RunStepBody_).
    StepStatus RunStep_(Op op, int iter)
    {
        __try
        {
            return RunStepBody_(op, iter);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            const unsigned long code = static_cast<unsigned long>(GetExceptionCode());
            std::snprintf(faultDetail_, sizeof(faultDetail_),
                          "structured exception 0x%08lX (device removed reason=0x%08X)",
                          code,
                          static_cast<unsigned>(renderer_.GetDevice()
                              ? renderer_.GetDevice()->GetDeviceRemovedReason() : E_FAIL));
            return StepStatus::HardFault;
        }
    }

    StepStatus RunStepBody_(Op op, int iter)
    {
        faultDetail_[0] = '\0';
        if (roughnessEdits_)
        {
#if WITH_EDITOR
            auto* object = scene_.FindEditorObject(722);
            auto* gb = object ? object->AsGBufferRenderable() : nullptr;
            if (!gb)
            {
                std::snprintf(faultDetail_, sizeof(faultDetail_), "wind_test sphere #722 missing");
                return StepStatus::FrameThrew;
            }
            // Exactly the inspector's live mutation, without recreating materials or flushing GPU.
            auto& params = gb->MaterialParamsRef();
            params.metalRough.y = 0.001f + 0.998f * float((iter * 173) % 10007) / 10006.0f;
            return RenderFrames_(1, "roughness-edit") ? StepStatus::Ok : StepStatus::FrameThrew;
#else
            std::snprintf(faultDetail_, sizeof(faultDetail_), "roughness regression requires WITH_EDITOR");
            return StepStatus::FrameThrew;
#endif
        }

        // Some ops interleave a resize tightly with the churn to shake out
        // a deferred-target recreate racing an in-flight frame.
        const bool tightResize = (iter % 3 == 0);
        if (tightResize && op != Op::ResizeWindow)
        {
            CycleResize_();
        }

        PerformOp_(op, iter);

        // Render K real frames to pump the pipeline (K rotates 2..4).
        const int framesToRender = 2 + (iter % 3);
        if (!RenderFrames_(framesToRender, OpName(op)))
        {
            return StepStatus::FrameThrew;
        }
        return StepStatus::Ok;
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
            const UINT64 n = infoQueue_->GetNumStoredMessages();
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
        if (gLog) { fflush(gLog); }

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
        if (dredFile_) { std::fputs(line, dredFile_); std::fflush(dredFile_); }
    }

    void DumpDred_(int iter, const char* op)
    {
        ID3D12Device* dev = renderer_.GetDevice();
        if (!dev)
        {
            Log("DRED: no device to query\n");
            return;
        }

        fopen_s(&dredFile_, diag::LogPath("dred_dump.txt").c_str(), "w");

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
                if (dredFile_) { std::fclose(dredFile_); dredFile_ = nullptr; }
                return;
            }
        }

        // ---- Auto-breadcrumbs: which command list/queue + op was executing ----
        DumpBreadcrumbs_(dred1.Get(), dred0.Get(), have1);

        // ---- Page-fault allocations: existing + recently-freed at the fault VA --
        DumpPageFault_(dred1.Get(), dred0.Get(), have1);

        DredLine_("==== end DRED dump ====\n");
        if (dredFile_) { std::fclose(dredFile_); dredFile_ = nullptr; }
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

    // Renders `count` real frames. Mirrors the essential per-frame steps of the
    // interactive App loop (message pump + BeginFrame + level-request pump +
    // scene render). Returns false if a frame render threw (caught here).
    bool RenderFrames_(int count, const char* label)
    {
        for (int f = 0; f < count && !faultCaught_; ++f)
        {
            Profiler::Get().BeginFrame(renderer_.GetTotalFrameNumber());
            TaskSystem::Get().WaitForTrackedAsyncTasks();
            renderer_.BeginFrame();

            // Service window messages (WM_SIZE from our resizes, paint, etc.).
            MSG msg = {};
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            try
            {
                renderer_.BeginImGuiFrame();
                renderer_.Tick(1.0f / 60.0f);
                scene_.Tick(1.0f / 60.0f);
                levelManager_.Tick(1.0f / 60.0f);

                PumpPendingLevelRequest_();

                scene_.Render(&renderer_);
            }
            catch (const std::exception& e)
            {
                std::snprintf(faultDetail_, sizeof(faultDetail_),
                              "exception during %s frame: %.220s", label, e.what());
                Log("EXCEPTION: %s\n", faultDetail_);
                Profiler::Get().EndFrame();
                return false;
            }

            Profiler::Get().EndFrame();
        }
        return true;
    }

    // Copy of the interactive loop's level-request handling so a queued reload/
    // switch actually executes (teardown + rebuild) inside a rendered frame.
    void PumpPendingLevelRequest_()
    {
        auto pendingLevel = levelManager_.ConsumePendingLevelRequest();
        if (!pendingLevel)
        {
            return;
        }

        renderer_.WaitForPreviousFrame();

        UploadBatch levelBatch;
        if (levelBatch.Begin(&renderer_))
        {
            LevelLoadContext levelCtx{ levelBatch, renderer_, scene_
#if WITH_EDITOR
                , pendingLevel->options.editorDocument
#endif
            };

            const bool levelLoaded = pendingLevel->loadFromPath
                ? levelManager_.LoadLevelFromPath(pendingLevel->sourcePath, levelCtx, pendingLevel->options)
                : levelManager_.LoadLevel(pendingLevel->levelName, levelCtx, pendingLevel->options);
            if (levelLoaded)
            {
                levelBatch.SubmitAndWait(&renderer_);
            }
        }
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
            // Live-switch among the three demo levels (the Step 19 path).
            nextLevelIndex_ = (nextLevelIndex_ + 1) % 3;
            levelManager_.RequestLevelPathChange(kLevels_[nextLevelIndex_]);
            break;
        }
        case Op::ResizeWindow:
        {
            CycleResize_();
            break;
        }
        case Op::DlssMode:
        {
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
            static const float kScales[] = { 1.0f, 0.75f, 0.5f, 0.85f };
            renderScaleIndex_ = (renderScaleIndex_ + 1) % static_cast<int>(std::size(kScales));
            renderer_.SetRenderResolutionScale(kScales[renderScaleIndex_]);
            break;
        }
        case Op::ReflectionScale:
        {
            static const float kScales[] = { 0.5f, 0.25f, 0.75f, 1.0f };
            reflectionScaleIndex_ = (reflectionScaleIndex_ + 1) % static_cast<int>(std::size(kScales));
            renderer_.SetReflectionTextureScale(kScales[reflectionScaleIndex_]);
            break;
        }
#if WITH_EDITOR
        case Op::EditorSpawnDelete:
        {
            EditorSpawnRenderDelete_();
            break;
        }
#endif
        default:
            break;
        }
    }

    // Resize the OS window to the next size in the rotation. WM_SIZE is dispatched
    // during the next frame's message pump, which calls Renderer::OnResize ->
    // DestroyDeferredTargets + CreateDeferredTargets (the prime suspect). We also
    // call OnResize directly so the recreate happens right now, tightly against
    // the churn, in addition to the message-driven one.
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
        // Direct recreate too (the WM_SIZE path may be coalesced/deferred).
        renderer_.OnResize(w, h);
    }

#if WITH_EDITOR
    // Mirrors SpawnMeshCommand/DeleteObjectCommand: spawn an initialized editor
    // object, render a frame with it live, then delete it. Exercises the editor
    // scene-mutation path (AddInitializedEditorObject / RemoveEditorObject with
    // a full GPU sync around each mutation).
    void EditorSpawnRenderDelete_()
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
            added = scene_.AddInitializedEditorObject(renderer_, uploads, id, std::move(runtime));
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
        RenderFrames_(2, "editor-spawn");

        renderer_.WaitForPreviousFrame();
        scene_.RemoveEditorObject(id);
        scene_.RefreshShadowGpuForEditor(renderer_);
    }
#endif

    std::string CurrentLevelPath_() const
    {
        return kLevels_[nextLevelIndex_ % 3];
    }

    HWND hWnd_ = nullptr;
    Renderer& renderer_;
    Scene& scene_;
    LevelManager& levelManager_;
    int iterations_ = 0;
    bool gbvContinue_ = false;
    bool roughnessEdits_ = false;

    ComPtr<ID3D12InfoQueue> infoQueue_;
    FILE* dredFile_ = nullptr;
    bool faultCaught_ = false;
    char faultDetail_[320] = {};

    int nextLevelIndex_ = 0;
    int resizeIndex_ = 0;
    int dlssModeIndex_ = 0;
    int renderScaleIndex_ = 0;
    int reflectionScaleIndex_ = 0;
#if WITH_EDITOR
    Scene::SceneObjectId nextEditorStressId_ = 1;
#endif

    static constexpr const char* kLevels_[3] = {
        "data/levels/demo.json", "data/levels/demo1.json", "data/levels/new1.json" };
};

constexpr const char* SceneStressDriver::kLevels_[3];

} // namespace

int App::RunSceneStress(HINSTANCE hInstance, int nCmdShow, int iterations, bool gbvContinue, bool roughnessEdits)
{
    if (iterations <= 0)
    {
        iterations = 300; // default: a few hundred churn steps
    }

    fopen_s(&gLog, diag::LogPath("scene_stress.log").c_str(), "w");
    SetUnhandledExceptionFilter(StressCrashFilter); // symbolize worker-thread/teardown/WndProc faults
    Log("scene lifecycle stress harness\n");
    Log("gbvContinue=%d\n", gbvContinue ? 1 : 0);
    Log("iterations=%d WITH_EDITOR=%d\n", iterations,
#if WITH_EDITOR
        1
#else
        0
#endif
    );

    int exitCode = 1;

    // Same real bootstrap as App::Run().
    systems_ = std::make_unique<Systems::AppSystems>();
    Systems::Init(systems_.get());

    {
        auto& renderer = systems_->renderer;
        auto& scene = systems_->scene;
        auto& input = systems_->input;
        auto& levelManager = systems_->levelManager;

        InitWindow(hInstance, nCmdShow);
        TaskSystem::Get().Start(static_cast<unsigned int>(std::thread::hardware_concurrency() * 0.75f));
        Profiler::Get().SetThreadName("MainThread");

        InitScene();
        Log("boot: window + device + scene ready (initial level loaded)\n");

        bool faultCaught = false;
        try
        {
            (void)input;
            SceneStressDriver driver(hWnd_, renderer, scene, levelManager, iterations, gbvContinue, roughnessEdits);
            exitCode = driver.Run();
            faultCaught = driver.FaultCaught();
        }
        catch (const std::exception& e)
        {
            Log("EXCEPTION (outside frame try/catch): %.240s\n", e.what());
            Log("verdict: FAULT op=driver iter=? detail=uncaught-exception\n");
            exitCode = 2;
            faultCaught = true;
        }

        TaskSystem::Get().Stop();

        // On a caught fault the device is typically removed; the normal GPU
        // teardown (WaitForPreviousFrame / Clear / Shutdown) then dereferences
        // dead resources and crashes with an access violation during process
        // exit — which would mask our deterministic exit code and truncate the
        // log. Skip it: the process is terminating anyway. On a clean run, tear
        // down normally.
        if (!faultCaught)
        {
            renderer.WaitForPreviousFrame();
            scene.Clear();
            Systems::DestroyOceanSimulation();
            renderer.Shutdown();
        }
        else
        {
            // Flush the verdict and terminate immediately with our own exit code.
            // Unwinding through the destructors of dead GPU objects (Renderer,
            // Scene) would itself access-violate and mask the result. The OS
            // reclaims everything on process exit.
            Log("fault caught: skipping GPU teardown (device likely removed); terminating with exit code %d\n", exitCode);
            if (gLog) { fflush(gLog); fclose(gLog); gLog = nullptr; }
            TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
        }
    }

    Systems::Shutdown();
    systems_.reset();

    Log("shutdown complete; exit code %d\n", exitCode);
    if (gLog) { fflush(gLog); fclose(gLog); gLog = nullptr; }

    // Skip the CRT exit-time teardown (execute_onexit_table). Streamline/NGX registers a static
    // destructor that intermittently access-violates there — AFTER our explicit slShutdown() + device
    // release (Renderer::Shutdown) and after this deterministic exit code is set. It's an NVIDIA DLL
    // teardown-order bug (reproduces regardless of shadow mode; a dangling pointer into an unmapped
    // module), not ours, and would otherwise mask the harness's real exit code with 0xC0000005. All
    // our own cleanup has already run; the OS reclaims the rest. Mirrors the faultCaught path above.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
    return exitCode; // not reached (TerminateProcess doesn't return)
}

int RunSceneStress(HINSTANCE__* hInstance, int nCmdShow, int iterations, bool gbvContinue, bool roughnessEdits)
{
    App app;
    return app.RunSceneStress(reinterpret_cast<HINSTANCE>(hInstance), nCmdShow, iterations, gbvContinue, roughnessEdits);
}
