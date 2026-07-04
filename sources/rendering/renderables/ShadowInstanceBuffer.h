#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

#include "rendering/core/RenderConstants.h"
#include "rendering/renderables/InstanceTypes.h"

class Renderer;
class RenderableObjectBase;

// Rung 0 / Step 1: the persistent per-caster shadow-instance buffer.
//
// One `render::InstancePerObject` entry per shadow caster (every object whose
// CastsShadow() is true), addressed by a stable "caster id" = its enumeration
// index over the scene's object list. The eventual GPU-driven shadow path
// (Steps 4/5) will read `world`/`prevWorld` from here, indexed by the cull
// pass's visible-caster list, instead of the CPU re-marshalling the instance
// array for every shadow view every frame.
//
// STORAGE. A single upload-heap resource holding render::kFrameCount contiguous
// regions of `capacity` entries — the LightManager spot/point ring pattern.
// The buffer is persistently mapped and written from the CPU; there are no GPU
// copies, no state transitions, and no barriers, so it never perturbs the
// D3D12 validation / GBV surface (Step 1 acceptance: "unused -> no
// perturbation"). Ring-buffering by frame index is the WAR guard the plan calls
// for: frame N writes region N % kFrameCount, and BeginFrame has fenced that
// slot's prior user (frame N - kFrameCount) before it is reused.
//
// UPDATE. Rebuild() does a full fill (level load / caster-set change). Per frame
// UpdateForFrame() recomputes each caster's entry and re-uploads ONLY the ones
// that changed, propagating each change across all kFrameCount regions. A fully
// static scene therefore does zero re-uploads after warmup.
//
// Step 1 note: this buffer is NOT yet read by any pass. It is allocated,
// populated, and maintained so the later steps can consume it.
class ShadowInstanceBuffer
{
public:
    // Full (re)build for a caster set — call at level load, under GPU idle. May
    // grow (reallocate) the GPU resource. Assigns caster ids = enumeration order
    // over `objects` (filtered to shadow casters). Fills all ring regions.
    void Rebuild(Renderer* renderer,
                 const std::vector<std::unique_ptr<RenderableObjectBase>>& objects);

    // Per-frame CPU update (call once per frame from the render path). Recomputes
    // every caster's instance data, writes only the changed entries into this
    // frame's ring region, and returns the number of entries re-uploaded this
    // frame (0 on a static scene after warmup). If the caster set changed since
    // the last build (count differs) it falls back to a full Rebuild.
    std::uint32_t UpdateForFrame(Renderer* renderer,
                                 const std::vector<std::unique_ptr<RenderableObjectBase>>& objects);

    // Drop CPU-side state on level unload. Deliberately RETAINS the GPU buffer +
    // its SRV descriptors (the LightManager lesson: freeing a buffer whose SRV a
    // pass still references is a use-after-free). The next Rebuild reuses it.
    void Reset();

    std::uint32_t CasterCount() const { return count_; }

    // SRV for ring region `frameIndex` (0..kFrameCount-1). {0} if not built.
    // For the future indirect shadow VS (Step 5); unused in Step 1.
    D3D12_CPU_DESCRIPTOR_HANDLE Srv(UINT frameIndex) const;

private:
    // Ensure the GPU resource holds at least `requiredCasters` entries per region.
    // Reallocates (upload heap, kFrameCount regions, persistently mapped) + rebuilds
    // the per-region SRVs on growth. Returns false on allocation failure.
    bool EnsureCapacity(Renderer* renderer, size_t requiredCasters);

    // True for objects that belong in the buffer (a shadow caster with a CPU model
    // matrix). Mirrors the CastsShadow() filter shadowCasterSource_ uses.
    static bool IsCaster(const RenderableObjectBase* obj);

    // Write one caster's InstancePerObject (world/prevWorld + material payload).
    // Uses the object's IInstanceable fill when available, else a depth-only fill
    // (world/prevWorld/objectId; material fields left zero — shadows are depth-only).
    static void FillEntry(const RenderableObjectBase* obj, render::InstancePerObject& out);

    Microsoft::WRL::ComPtr<ID3D12Resource>        buffer_;   // upload heap, kFrameCount regions
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>  srvHeap_;  // one SRV per region
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, render::kFrameCount> srvHandles_{};
    render::InstancePerObject*                    mapped_ = nullptr; // persistent map

    size_t        capacity_ = 0; // entries PER region
    std::uint32_t count_    = 0; // live caster count

    // Authoritative current value per caster (change-detection reference) and a
    // per-entry "frames remaining to propagate a change into all regions" counter.
    std::vector<render::InstancePerObject> cpuData_;
    std::vector<std::uint8_t>              pending_;

    std::uint32_t logFramesRemaining_ = 5; // one-off warmup logging (see .cpp)
};
