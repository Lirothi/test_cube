#include "rendering/core/GraphicsDevice.h"

#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <d3d12sdklayers.h>

#include <atomic>
#include <cstdio>

#include "core/Helpers.h"
#include "core/diagnostics/DiagPaths.h"
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
    // Step 11: --enhanced-barriers. Step 9 detects the capability; the engine must still behave
    // exactly as before until this is opted into, so support alone changes nothing.
    std::atomic<bool> g_enhancedOptIn{ false };
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
    // --legacy-barriers keeps the old path reachable for bisecting once step 15 flips the default.
    const bool forceLegacy = g_forceLegacyBarriers.load(std::memory_order_relaxed);
    if (forceLegacy) { enhancedBarriers_ = false; }
    enhancedOptIn_ = g_enhancedOptIn.load(std::memory_order_relaxed) && !forceLegacy;
    // Publish the resolved decision for the texture-creation helper (step 11). Done here, once,
    // so the ~11 creation sites need no new parameter and cannot disagree about it.
    render::SetEnhancedTextureCreation(UseEnhancedBarriers());
    {
        char msg[224];
        std::snprintf(msg, sizeof(msg),
                      "[caps] raytracing tier=%d | enhanced barriers: device10=%s supported=%s in-use=%s%s\n",
                      static_cast<int>(raytracingTier_),
                      device10_ ? "yes" : "no",
                      enhancedBarriers_ ? "yes" : "no",
                      UseEnhancedBarriers() ? "yes" : "no",
                      forceLegacy ? " (forced off by --legacy-barriers)" : "");
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
            info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            // Add filters for noisy messages if desired
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
