#include "rendering/core/GraphicsDevice.h"

#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <d3d12sdklayers.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "core/Helpers.h"
#include "core/diagnostics/DiagPaths.h"
#include "rendering/core/BarrierTranslation.h"
#include "rendering/core/TextureCreate.h"

namespace
{
    // Process-global diagnostics toggles (see EnableDred/GbvForStress). Mirror
    // the rt-force-as-fail static-flag pattern: set once from main.cpp before
    // device creation, read here.
    std::atomic<bool> g_dredForStress{ false };
    std::atomic<bool> g_gbvForStress{ false };
    // Step 9: --legacy-barriers. Lives here rather than in render:: because GraphicsDevice cannot
    // see Renderer.h — Renderer.h includes THIS header, so the dependency only runs one way.
    std::atomic<bool> g_forceLegacyBarriers{ false };
    // Was step 11's opt-in; step 15 made enhanced the default, so nothing reads this any more.
    // Kept so `--enhanced-barriers` remains an accepted (inert) flag rather than an error.
    std::atomic<bool> g_enhancedOptIn{ false };
    // Step 15: --barrier-msg-trace.
    std::atomic<bool> g_barrierMsgTrace{ false };

#ifdef _DEBUG
    // The barrier-interop family. Deliberately NOT every message: the point is to attribute the
    // legacy/enhanced mixing errors to a MODULE, and a trace of all debug-layer chatter would be
    // unreadable and would slow the run enough to change what it reproduces.
    bool IsBarrierInteropMessage(D3D12_MESSAGE_ID id)
    {
        switch (static_cast<int>(id)) {
        case 527:  // before-state does not match
        case 538:  // invalid state for use
        case 1334: // barrier layout does not match expected layout (at ExecuteCommandLists)
        case 1350: // enhanced -> legacy without passing through the common layout
            return true;
        default:
            return false;
        }
    }

    // Return addresses attributed to their MODULE, which is the whole question here. Deliberately
    // no dbghelp: symbol resolution needs PDBs the third-party modules do not ship, it is slow
    // inside a debug-layer callback, and "nvngx_dlss.dll+0x1234" already answers "whose code is
    // this". Our own frames still read as test_cube.exe and can be resolved later if needed.
    void AppendModuleBacktrace(char* out, size_t cap)
    {
        void* frames[24] = {};
        const USHORT captured = CaptureStackBackTrace(2, 24, frames, nullptr);
        size_t used = std::strlen(out);
        for (USHORT i = 0; i < captured && used + 64 < cap; ++i) {
            HMODULE mod = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCSTR>(frames[i]), &mod) && mod) {
                char full[MAX_PATH] = "";
                if (GetModuleFileNameA(mod, full, MAX_PATH) != 0) {
                    const char* slash = std::strrchr(full, '\\');
                    strncpy_s(name, slash ? slash + 1 : full, _TRUNCATE);
                }
            }
            const auto offset = static_cast<std::uintptr_t>(
                reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(mod));
            const int n = std::snprintf(out + used, cap - used, "    #%02u %s+0x%llx\n",
                                        static_cast<unsigned>(i), name,
                                        static_cast<unsigned long long>(offset));
            if (n <= 0) { break; }
            used += static_cast<size_t>(n);
        }
    }

    void CALLBACK BarrierMessageCallback(D3D12_MESSAGE_CATEGORY,
                                         D3D12_MESSAGE_SEVERITY,
                                         D3D12_MESSAGE_ID id,
                                         LPCSTR description,
                                         void*)
    {
        if (!IsBarrierInteropMessage(id)) { return; }

        static std::atomic_flag lock = ATOMIC_FLAG_INIT;
        while (lock.test_and_set(std::memory_order_acquire)) {}
        {
            // Capped PER RESOURCE NAME, not globally. A flat cap is useless here: the first
            // offender floods it and everything later — which is where the interesting emitters
            // are — never gets printed. That mistake cost one run.
            char resName[96] = "";
            if (description) {
                if (const char* open = std::strchr(description, '\'')) {
                    if (const char* close = std::strchr(open + 1, '\'')) {
                        const size_t len = static_cast<size_t>(close - open - 1);
                        strncpy_s(resName, open + 1, (std::min)(len, sizeof(resName) - 1));
                    }
                }
            }
            static char seen[64][96] = {};
            static int seenHits[64] = {};
            static int seenCount = 0;
            int slot = -1;
            for (int i = 0; i < seenCount; ++i) {
                if (std::strcmp(seen[i], resName) == 0) { slot = i; break; }
            }
            if (slot < 0 && seenCount < 64) {
                slot = seenCount++;
                strncpy_s(seen[slot], resName, _TRUNCATE);
            }
            const bool skip = (slot < 0) || (++seenHits[slot] > 2);
            if (skip) { lock.clear(std::memory_order_release); return; }

            char msg[4096];
            std::snprintf(msg, sizeof(msg), "\n[msg-trace] id=%d %s\n", static_cast<int>(id),
                          description ? description : "");
            AppendModuleBacktrace(msg, sizeof(msg));
            FILE* f = nullptr;
            if (fopen_s(&f, diag::LogPath("barrier_msg_trace.log").c_str(), "a") == 0 && f) {
                std::fputs(msg, f);
                std::fclose(f);
            }
            OutputDebugStringA(msg);
        }
        lock.clear(std::memory_order_release);
    }

    // GBV's own reporting channel. Deliberately NOT filtered to the barrier-interop ids the trace
    // above cares about: the point of GPU-based validation is the messages nobody predicted.
    // Identical texts are collapsed for the same reason the barrier log collapses them -- a
    // per-draw complaint repeats every frame and buries whatever came next.
    void CALLBACK GbvMessageCallback(D3D12_MESSAGE_CATEGORY,
                                     D3D12_MESSAGE_SEVERITY severity,
                                     D3D12_MESSAGE_ID id,
                                     LPCSTR description,
                                     void*)
    {
        if (severity != D3D12_MESSAGE_SEVERITY_ERROR &&
            severity != D3D12_MESSAGE_SEVERITY_CORRUPTION &&
            severity != D3D12_MESSAGE_SEVERITY_WARNING) {
            return;
        }

        static std::atomic_flag lock = ATOMIC_FLAG_INIT;
        while (lock.test_and_set(std::memory_order_acquire)) {}
        {
            static char seen[128][160] = {};
            static int seenCount = 0;
            char key[160] = "";
            std::snprintf(key, sizeof(key), "%d:%.140s", static_cast<int>(id),
                          description ? description : "");
            bool duplicate = false;
            for (int i = 0; i < seenCount && !duplicate; ++i) {
                duplicate = (std::strcmp(seen[i], key) == 0);
            }
            if (!duplicate) {
                if (seenCount < 128) { strncpy_s(seen[seenCount++], key, _TRUNCATE); }
                const char* sev = (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) ? "CORRUPTION"
                                : (severity == D3D12_MESSAGE_SEVERITY_ERROR)      ? "ERROR"
                                                                                  : "WARNING";
                char msg[4096];
                std::snprintf(msg, sizeof(msg), "[gbv] %s id=%d: %s\n", sev, static_cast<int>(id),
                              description ? description : "");
                FILE* f = nullptr;
                if (fopen_s(&f, diag::LogPath("gbv.log").c_str(), "a") == 0 && f) {
                    std::fputs(msg, f);
                    std::fclose(f);
                }
                OutputDebugStringA(msg);
            }
        }
        lock.clear(std::memory_order_release);
    }
#endif
}

void GraphicsDevice::ForceLegacyBarriers(bool enable)
{
    g_forceLegacyBarriers.store(enable, std::memory_order_relaxed);
}

void GraphicsDevice::EnableEnhancedBarriers(bool enable)
{
    g_enhancedOptIn.store(enable, std::memory_order_relaxed);
}

void GraphicsDevice::EnableDredForStress(bool enable)
{
    g_dredForStress.store(enable, std::memory_order_relaxed);
}

void GraphicsDevice::EnableGbvForStress(bool enable)
{
    g_gbvForStress.store(enable, std::memory_order_relaxed);
}

void GraphicsDevice::EnableBarrierMessageTrace(bool enable)
{
    g_barrierMsgTrace.store(enable, std::memory_order_relaxed);
}

void GraphicsDevice::InitDevice()
{
    const bool dredStress = g_dredForStress.load(std::memory_order_relaxed);
    const bool gbvStress = g_gbvForStress.load(std::memory_order_relaxed);

#ifdef _DEBUG
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();

            // GPU-based validation (heavier second signal): reports invalid
            // resource/descriptor access via the info queue, potentially at the
            // exact dispatch. OFF unless explicitly requested (--scene-stress-gbv)
            // because it perturbs timing and can fire on the first frame,
            // masking the device-hang race.
            if (gbvStress) {
                Microsoft::WRL::ComPtr<ID3D12Debug1> dbg1;
                if (SUCCEEDED(dbg.As(&dbg1))) {
                    dbg1->SetEnableGPUBasedValidation(TRUE);
                }
            }
        }
    }
#endif

    // DRED must be enabled BEFORE device creation. Forced on under --scene-stress
    // (and always in Debug) so a device removal produces auto-breadcrumbs + a
    // page-fault allocation report naming the faulting op/resource.
#if defined(_DEBUG)
    const bool enableDred = true;
#else
    const bool enableDred = dredStress;
#endif
    if (enableDred) {
        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)))) {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);

            // Breadcrumb context strings (op-level annotations) if the newer
            // settings interface is available.
            Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings1;
            if (SUCCEEDED(dredSettings.As(&dredSettings1))) {
                dredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
        }
    }

    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)));

    // DXR capability detection (S1). Both queries are optional: older runtimes
    // leave device5_ null and the tier at NOT_SUPPORTED, and every RT path is
    // gated on IsRaytracingSupported(), so this is a no-op on non-RT hardware.
    if (SUCCEEDED(device_.As(&device5_))) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
            raytracingTier_ = options5.RaytracingTier;
        }
    }

    // Enhanced barriers (barrier plan step 9). Same shape as the DXR probe above: both halves are
    // optional and any failure just leaves the capability false. Detected and LOGGED only —
    // nothing reads it yet; steps 10-16 build the emission path behind it.
    if (SUCCEEDED(device_.As(&device10_))) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12)))) {
            enhancedBarriers_ = options12.EnhancedBarriersSupported != FALSE;
        }
    }
    // Typed UAV loads of the "additional formats" set. The ocean mip chain now READS its source
    // mip through a UAV (R16G16B16A16_FLOAT) instead of an SRV, so this stopped being trivia and
    // became a capability the renderer depends on — recorded here rather than assumed.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
            typedUavLoadAdditionalFormats_ = options.TypedUAVLoadAdditionalFormats != FALSE;
        }
    }
    // Step 15 — THE FLIP. The capability is now the decision: enhanced on a machine that supports
    // it, legacy on one that does not. `--legacy-barriers` remains the escape hatch and is the
    // only thing that can turn it off, so a suspected barrier regression stays one flag away from
    // being bisected. `--enhanced-barriers` survives as a no-op so existing scripts keep working.
    const bool forceLegacy = g_forceLegacyBarriers.load(std::memory_order_relaxed);
    if (forceLegacy) { enhancedBarriers_ = false; }
    enhancedOptIn_ = !forceLegacy;
    // Publish the resolved decision for the texture-creation helper (step 11). Done here, once,
    // so the ~11 creation sites need no new parameter and cannot disagree about it.
    render::SetEnhancedTextureCreation(UseEnhancedBarriers());
    barriers::SetEnabled(UseEnhancedBarriers()); // step 13: direct sites read the same decision
    {
        char msg[288];
        std::snprintf(msg, sizeof(msg),
                      "[caps] raytracing tier=%d | enhanced barriers: device10=%s supported=%s in-use=%s%s"
                      " | typed UAV load additional formats=%s\n",
                      static_cast<int>(raytracingTier_),
                      device10_ ? "yes" : "no",
                      enhancedBarriers_ ? "yes" : "no",
                      UseEnhancedBarriers() ? "yes" : "no",
                      forceLegacy ? " (forced off by --legacy-barriers)" : "",
                      typedUavLoadAdditionalFormats_ ? "yes" : "no");
        OutputDebugStringA(msg);
        // Also to a file. A capability the whole enhanced-barrier half is gated on has to be
        // readable from a plain run, not only under a debugger — DBWIN output is lost otherwise,
        // the same trap that hid the stress verdict and the barrier trace earlier in this work.
        FILE* f = nullptr;
        if (fopen_s(&f, diag::LogPath("device_caps.log").c_str(), "w") == 0 && f) {
            std::fputs(msg, f);
            std::fclose(f);
        }
    }
}

void GraphicsDevice::SetupDebugBreaks()
{
#ifdef _DEBUG
    {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device_.As(&info))) {
            // --scene-stress-gbv asks for "errors logged, not fatal" so the harness can DRAIN the
            // info queue and print what the debug layer actually said. Breaking on ERROR defeats
            // that: the process dies inside the offending call and the message is never read. This
            // cost real time during the enhanced-barrier work — the stack named the call site over
            // and over but never the reason.
            const bool breakOnError = !g_gbvForStress.load(std::memory_order_relaxed);
            info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, breakOnError ? TRUE : FALSE);
            info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            // Add filters for noisy messages if desired
        }

        // GBV on an ordinary run has nowhere to report to: the stress harness drains the info
        // queue itself, and a normal run does not, so every message would go to a debugger that
        // is not attached. Break-on-error is already off under GBV (see above), which means
        // WITHOUT this the validation runs and says nothing at all. One callback, one file.
        if (g_gbvForStress.load(std::memory_order_relaxed)) {
            Microsoft::WRL::ComPtr<ID3D12InfoQueue1> gbvQueue;
            if (SUCCEEDED(device_.As(&gbvQueue))) {
                std::remove(diag::LogPath("gbv.log").c_str()); // fresh per run
                DWORD gbvCookie = 0;
                gbvQueue->RegisterMessageCallback(&GbvMessageCallback,
                                                  D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &gbvCookie);
            }
        }

        // Step 15: the message CALLBACK runs on the thread that raised the message, at the point
        // of the offending call — so a stack captured here names the emitter, which the message
        // text never does. ID3D12InfoQueue1 is optional; without it the flag is simply inert.
        if (g_barrierMsgTrace.load(std::memory_order_relaxed)) {
            Microsoft::WRL::ComPtr<ID3D12InfoQueue1> info1;
            if (SUCCEEDED(device_.As(&info1))) {
                std::remove(diag::LogPath("barrier_msg_trace.log").c_str()); // fresh per run
                DWORD cookie = 0;
                info1->RegisterMessageCallback(&BarrierMessageCallback,
                                               D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
            }
        }
    }
#endif
}

void GraphicsDevice::InitQueue()
{
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)));
    queue_->SetName(L"Queue.Direct");

    // Async-compute plan step 1: the second queue. Created here and left IDLE — nothing submits to
    // it, so this step is judged on the frame being unchanged, not on anything running.
    //
    // NOT fatal on failure, unlike the direct queue: a compute queue is a capability, and the
    // engine's whole existing frame works without one. A driver that refuses it should leave the
    // renderer running on the direct queue exactly as before rather than failing to boot, which is
    // also what makes `--no-async-compute` (step 8) a real fallback rather than a fiction.
    D3D12_COMMAND_QUEUE_DESC cqd{};
    cqd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    cqd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    const HRESULT computeHr = device_->CreateCommandQueue(&cqd, IID_PPV_ARGS(&computeQueue_));
    if (SUCCEEDED(computeHr) && computeQueue_) {
        computeQueue_->SetName(L"Queue.AsyncCompute");
    }
    else {
        computeQueue_.Reset();
    }

    // APPENDED to what InitDevice wrote (it opens with "w", this opens with "a"): the queue does
    // not exist yet when the caps line is written, because the caller runs Streamline hooks
    // between InitDevice and InitQueue.
    char msg[192];
    std::snprintf(msg, sizeof(msg), "[caps] async compute queue: %s%s\n",
                  computeQueue_ ? "created (idle)" : "NOT created",
                  computeQueue_ ? "" : " — async compute unavailable on this device");
    OutputDebugStringA(msg);
    FILE* f = nullptr;
    if (fopen_s(&f, diag::LogPath("device_caps.log").c_str(), "a") == 0 && f) {
        std::fputs(msg, f);
        std::fclose(f);
    }
}

void GraphicsDevice::ReportLiveObjects()
{
#if defined(_DEBUG)
    // 1) Detailed report from the device
    if (device_) {
        Microsoft::WRL::ComPtr<ID3D12DebugDevice> ddev;
        if (SUCCEEDED(device_.As(&ddev))) {
            ddev->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL);
        }
    }
    // 2) DXGI report (optional)
    {
        Microsoft::WRL::ComPtr<IDXGIDebug1> dxgiDbg;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDbg)))) {
            dxgiDbg->ReportLiveObjects(DXGI_DEBUG_ALL,
                (DXGI_DEBUG_RLO_FLAGS)(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        }
    }
#endif
}

void GraphicsDevice::ReleaseQueue()
{
    // The compute queue goes first: it is the one nothing else holds a reference to, and releasing
    // it before the direct queue keeps the original teardown order for everything that does.
    if (computeQueue_) {
        computeQueue_.Reset();
    }
    if (queue_) {
        queue_.Reset();
    }
}

void GraphicsDevice::ReleaseDevice()
{
    device5_.Reset();
    device10_.Reset();
    enhancedBarriers_ = false;
    if (device_) {
        device_.Reset();
    }
}
