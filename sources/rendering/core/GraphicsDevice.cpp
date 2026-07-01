#include "rendering/core/GraphicsDevice.h"

#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <d3d12sdklayers.h>

#include <atomic>

#include "core/Helpers.h"

namespace
{
    // Process-global diagnostics toggles (see EnableDred/GbvForStress). Mirror
    // the rt-force-as-fail static-flag pattern: set once from main.cpp before
    // device creation, read here.
    std::atomic<bool> g_dredForStress{ false };
    std::atomic<bool> g_gbvForStress{ false };
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
    if (device_) {
        device_.Reset();
    }
}
