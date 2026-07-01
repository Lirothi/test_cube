#pragma once

#include <d3d12.h>
#include <wrl/client.h>

// Owns the D3D12 device and the direct command queue, plus debug-layer setup.
// Device and queue creation are split so the caller (Renderer) can run
// Streamline hooks between them, matching the original init order.
class GraphicsDevice
{
public:
    // Enables the debug layer (debug builds) and creates the device.
    void InitDevice();
    // Info-queue break-on-severity setup (debug builds, no-op otherwise).
    void SetupDebugBreaks();
    void InitQueue();

    // Diagnostics hooks (mirror the rt-force-as-fail static-flag pattern): set
    // BEFORE device creation. DRED (auto-breadcrumbs + page-fault + breadcrumb
    // context) names the faulting op/resource on a device removal; it is cheap
    // and does not perturb the race, so the --scene-stress harness turns it on.
    // GPU-based validation is a heavier second signal that can catch a bad
    // access at the exact dispatch, but it also perturbs timing and can fire on
    // the first frame — kept OFF by default (opt in with --scene-stress-gbv) so
    // it doesn't hide the device-hang race DRED is meant to catch.
    static void EnableDredForStress(bool enable);
    static void EnableGbvForStress(bool enable);

    void ReportLiveObjects();

    // Shutdown is staged to preserve the original release order
    // (queue before Streamline shutdown, device last).
    void ReleaseQueue();
    void ReleaseDevice();

    ID3D12Device* Device() const { return device_.Get(); }
    ID3D12CommandQueue* Queue() const { return queue_.Get(); }

    // DXR capability (queried once at device creation). Device5() is null and
    // the tier is NOT_SUPPORTED on hardware/runtimes without ray tracing.
    ID3D12Device5* Device5() const { return device5_.Get(); }
    D3D12_RAYTRACING_TIER RaytracingTier() const { return raytracingTier_; }
    bool IsRaytracingSupported() const { return raytracingTier_ >= D3D12_RAYTRACING_TIER_1_1; }

private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Device5> device5_; // null if DXR unsupported
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    D3D12_RAYTRACING_TIER raytracingTier_ = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
};
