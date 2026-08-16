#pragma once

#include <cstdint>

#include <d3d12.h>
#include <wrl/client.h>

#include "rendering/core/ResourceDeclarations.h"

class Renderer;

// Persistent GPU state for the photographic camera's eye adaptation
// (docs/photographic_rendering_improvement_plan.md, step P1 items 2-3).
//
// P1 lands ONLY the resources and their lifecycle. Nothing is dispatched, bound or transitioned
// yet, so this class cannot change a pixel; the histogram build, the percentile solve and the
// temporal adaptation are P2. The split is deliberate -- it lets the lifecycle (resize, device
// loss, level transition, shutdown) be exercised and gated on its own, before any metering logic
// exists to confuse a regression with.
//
// The two resources are RESOLUTION-INDEPENDENT: a fixed bin count and a single adapted value. That
// is why OnResize does not recreate them and only asks for a history reset. Together they are
// about 1 KB, which is why they are created unconditionally rather than gated on the feature being
// enabled -- gating would mean the dormant state exercises no lifecycle at all, which is the one
// thing this step exists to prove.
class ExposureMetering
{
public:
    ~ExposureMetering() { Release(); }

    // Idempotent. Safe to call every frame; does nothing once created.
    void EnsureResources(Renderer* renderer);
    // Device loss and shutdown. Undeclares the resources via GpuResource::Reset.
    void Release();

    // Section 6.4 of the plan: level load/unload, camera cut, large teleport, resolution
    // reallocation, disabled->enabled, and invalid luminance all invalidate the adapted value.
    // P1 only records the request; P2's solve consumes it and seeds from the current metered
    // target instead of adapting up from stale history.
    void RequestReset() { resetRequested_ = true; }
    bool ResetRequested() const { return resetRequested_; }
    bool ConsumeResetRequest()
    {
        const bool requested = resetRequested_;
        resetRequested_ = false;
        return requested;
    }

    bool IsReady() const { return created_; }

    ID3D12Resource* HistogramResource() const { return histogram_.Get(); }
    ID3D12Resource* ExposureResource() const { return exposure_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE HistogramUav() const { return histogramUav_; }
    D3D12_CPU_DESCRIPTOR_HANDLE HistogramSrv() const { return histogramSrv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE ExposureUav() const { return exposureUav_; }
    D3D12_CPU_DESCRIPTOR_HANDLE ExposureSrv() const { return exposureSrv_; }

    // Log-luminance bins. 256 is the usual choice: fine enough that a percentile lands within a
    // fraction of a stop over the plan's metering range, and it is one thread group of 256.
    static constexpr UINT kHistogramBins = 256u;

    // What the solve last wrote, read back from the GPU. Section 6.5 of the plan requires the dev
    // UI to surface these, and without them the settings are being tuned blind.
    struct Readback
    {
        float adaptedEv100 = 0.0f;
        float lowLuminance = 0.0f;
        float highLuminance = 0.0f;
        float targetEv100 = 0.0f;
        bool valid = false;
    };
    Readback LatestReadback() const;

    // Records the copies into this frame's ring slot: the 16-byte exposure record and the 256-bin
    // histogram. Called by the metering pass right after the solve; the resource state round-trip
    // is declared by that pass.
    void RecordReadbackCopy(ID3D12GraphicsCommandList* cl);
    ID3D12Resource* ReadbackSource() const { return exposure_.Get(); }

    // The bins the last readback frame metered, normalised so the tallest is 1.0, for the dev
    // window's plot. Returns false when no readback has landed yet. `outTotal` is the raw sample
    // count, which is what makes a percentile marker meaningful.
    bool LatestHistogram(float* outBins, UINT binCount, UINT* outTotal) const;

private:
    // RAW (byte-address) rather than structured, for two reasons that both bite in P2:
    // InterlockedAdd needs a raw or structured UAV, and ClearUnorderedAccessViewUint -- which is
    // how the bins get cleared every frame -- REJECTS structured buffers outright.
    GpuResource histogram_;
    // The adapted exposure. 16 bytes rather than 4: the section 6.5 debug contract has to surface
    // the metered low/high percentile luminance next to the adapted value, and keeping them in one
    // record avoids a second resource and a second readback for the sake of 12 bytes.
    GpuResource exposure_;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE histogramSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE histogramUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE exposureSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE exposureUav_{};

    // Readback ring. Four slots so the one we read is older than any frame still in flight, which
    // is what makes reading it without a fence safe: it was written frames ago and has retired.
    static constexpr UINT kReadbackSlots = 4u;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    const float* readbackPtr_ = nullptr; // persistently mapped, 4 slots x 4 floats
    // Separate resource rather than a suffix on the one above: the histogram is 1 KB against the
    // record's 16 bytes, and keeping them apart means the exposure readout costs one cache line.
    Microsoft::WRL::ComPtr<ID3D12Resource> histogramReadback_;
    const std::uint32_t* histogramReadbackPtr_ = nullptr; // 4 slots x kHistogramBins uints
    std::uint64_t readbackFrame_ = 0;

    bool created_ = false;
    // Starts true: the buffers are created with undefined contents, so the very first solve in P2
    // must seed rather than adapt from whatever the allocation happened to contain.
    bool resetRequested_ = true;
};
