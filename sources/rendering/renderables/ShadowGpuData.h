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
class Frustum;

// Rung 0 (Steps 1-2): the GPU-side data the future GPU-driven shadow pipeline consumes.
// All buffers are ALLOCATED + MAINTAINED here but NOT yet read by any pass (add-dormant).
//
// Three logical buffers, each an upload-heap ring of render::kFrameCount regions
// (the LightManager spot/point pattern — persistently mapped, CPU-written, no barriers,
// so nothing perturbs the D3D12 validation / GBV surface). Ring-buffering by frame index
// is the WAR guard: frame N writes region N % kFrameCount, and BeginFrame has fenced that
// slot's prior user before it is reused.
//
//  1. Per-caster INSTANCE data (Step 1) — `render::InstancePerObject` per shadow caster,
//     addressed by caster id = enumeration index over the scene's objects. The eventual
//     indirect shadow VS (Step 5) reads world/prevWorld from here.
//  2. Per-caster BOUNDS (Step 2) — `render::CasterBounds` (world center+radius+half-extents)
//     in lockstep with (1): same caster id, same change-detection, same ring. The Step 4
//     cull compute tests these against the view frustum planes.
//  3. Per-view FRUSTUM planes (Step 2) — `render::ShadowViewFrustum` (6 inward planes) for
//     every active shadow view, rewritten each frame (views move every frame). The other
//     per-view cull input for Step 4.
//
// UPDATE. Rebuild() does a full fill of the per-caster buffers at level load. Per frame
// UpdateForFrame() recomputes each caster's instance + bounds and re-uploads ONLY the
// changed ones (propagating each change across all ring regions) — a static scene does
// zero re-uploads after warmup. UpdateViewFrustums() rewrites this frame's frustum region
// from the active shadow views.
class ShadowGpuData
{
public:
    // Full (re)build of the per-caster buffers for a caster set — call at level load, under
    // GPU idle. Assigns caster ids = enumeration order over `objects` (shadow casters only).
    void Rebuild(Renderer* renderer,
                 const std::vector<std::unique_ptr<RenderableObjectBase>>& objects);

    // Per-frame per-caster CPU update. Recomputes instance + bounds, writes only changed
    // entries into this frame's ring region, returns the number re-uploaded (0 on a static
    // scene after warmup). Falls back to a full Rebuild if the caster set changed.
    std::uint32_t UpdateForFrame(Renderer* renderer,
                                 const std::vector<std::unique_ptr<RenderableObjectBase>>& objects);

    // Per-frame upload of the active shadow views' frustum planes into this frame's region.
    // `frustums[i]` may be null (inactive view slot → zeroed planes). `count` is the fixed
    // view-slot count (stable view->slot mapping for the future cull).
    void UpdateViewFrustums(Renderer* renderer, const Frustum* const* frustums, size_t count);

    // Drop CPU-side state on level unload; RETAINS the GPU buffers + SRVs (the LightManager
    // lesson: a pass may reference an SRV while frames are in flight). Next Rebuild reuses them.
    void Reset();

    std::uint32_t CasterCount() const { return count_; }
    std::uint32_t ViewFrustumCount() const { return viewFrustumCount_; }

    // SRVs for ring region `frameIndex` (0..kFrameCount-1); {0} if not built. For the future
    // cull compute (Step 4) / indirect VS (Step 5); unused in Steps 1-2.
    D3D12_CPU_DESCRIPTOR_HANDLE InstanceSrv(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE BoundsSrv(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE ViewFrustumSrv(UINT frameIndex) const;

private:
    // One upload-heap structured buffer of kFrameCount regions x `capacity` elements of
    // `stride` bytes, persistently mapped, with one SRV per region. The shared boilerplate
    // behind all three logical buffers above.
    struct Ring
    {
        Microsoft::WRL::ComPtr<ID3D12Resource>        buffer;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>  srvHeap;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, render::kFrameCount> srvHandles{};
        std::uint8_t* mapped = nullptr; // persistent map, region f at mapped + f*capacity*stride
        size_t        capacity = 0;     // elements per region
        UINT          stride = 0;

        bool Valid() const { return mapped != nullptr; }
        std::uint8_t* Region(UINT f) const;              // base of region f, or null
        D3D12_CPU_DESCRIPTOR_HANDLE Srv(UINT f) const;
    };

    // Ensure `ring` holds >= `elements` per region of `stride` bytes; (re)allocate + rebuild
    // its per-region SRVs on growth. Returns false on allocation failure.
    static bool EnsureRing(Renderer* renderer, Ring& ring, size_t elements, UINT stride,
                           const wchar_t* name);
    static void ReleaseRing(Renderer* renderer, Ring& ring);

    static bool IsCaster(const RenderableObjectBase* obj);
    static void FillInstance(const RenderableObjectBase* obj, render::InstancePerObject& out);
    static void FillBounds(const RenderableObjectBase* obj, render::CasterBounds& out);

    Ring instances_;     // per-caster InstancePerObject
    Ring bounds_;        // per-caster CasterBounds
    Ring viewFrustums_;  // per-view ShadowViewFrustum

    std::uint32_t count_ = 0;            // live caster count
    std::uint32_t viewFrustumCount_ = 0; // fixed shadow-view slot count

    // Authoritative current per-caster values (change-detection reference) + a per-entry
    // "frames remaining to propagate a change into all regions" counter (drives both the
    // instance and bounds writes, since bounds derive from the same transform).
    std::vector<render::InstancePerObject> cpuInstances_;
    std::vector<render::CasterBounds>      cpuBounds_;
    std::vector<std::uint8_t>              pending_;

    std::uint32_t logFramesRemaining_ = 5; // one-off warmup logging (see .cpp)
};
