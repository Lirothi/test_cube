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
    // Barrier plan step 15: log a MODULE-attributed stack for every barrier-interop message the
    // debug layer raises (1350/1334/527/538). The enhanced work reached a point where the message
    // names a resource but not the code that emitted the offending call — and the answer turned
    // out to be "not our code at all", which no amount of reading our own call sites could show.
    // Debug-only (it rides the info queue); set before device creation.
    static void EnableBarrierMessageTrace(bool enable);

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

    // Enhanced barriers (barrier plan step 9), queried once at device creation exactly like the
    // DXR pair above. Device10() is null on a runtime that never had ID3D12Device10, and
    // EnhancedBarriersSupported can still be false on one that does — both are checked.
    //
    // The Windows SDK on this machine (10.0.26100) already declares D3D12_BARRIER_GROUP,
    // ID3D12GraphicsCommandList7 and D3D12_FEATURE_D3D12_OPTIONS12, so the plan's Agility SDK
    // sub-step is NOT needed. Verified before any of this was written.
    //
    // Set from main.cpp BEFORE device creation (mirrors EnableDredForStress).
    static void ForceLegacyBarriers(bool enable);
    // Step 11 added this as the OPT-IN while the enhanced path was being built. Step 15 flipped
    // the default, so it is now a NO-OP kept only so existing command lines keep working;
    // `ForceLegacyBarriers` is the switch that still does something.
    static void EnableEnhancedBarriers(bool enable);
    ID3D12Device10* Device10() const { return device10_.Get(); }
    bool AreEnhancedBarriersSupported() const { return enhancedBarriers_; }
    // The EFFECTIVE switch. Since step 15 that means "supported, and not forced off by
    // --legacy-barriers"; `AreEnhancedBarriersSupported` stays a pure capability report.
    bool UseEnhancedBarriers() const { return enhancedBarriers_ && enhancedOptIn_; }

private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Device5> device5_; // null if DXR unsupported
    Microsoft::WRL::ComPtr<ID3D12Device10> device10_; // null without the enhanced-barrier interfaces
    bool enhancedBarriers_ = false;
    bool enhancedOptIn_ = false;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    D3D12_RAYTRACING_TIER raytracingTier_ = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
};
