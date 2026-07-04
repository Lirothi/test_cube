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
class Material;

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
    // `frustums[i]` may be null (inactive view slot → reject-all sentinel so the cull emits
    // zero for it). `count` is the fixed view-slot count (stable view->slot mapping for the cull).
    void UpdateViewFrustums(Renderer* renderer, const Frustum* const* frustums, size_t count);

    // Rung 0 / Step 4: record the GPU cull for this frame — a clear dispatch (init the per-
    // (view, mesh-group) indirect args) + a cull dispatch (frustum-test every caster into the
    // visible list + InstanceCounts). Writes the current frame's ring region. Produces the
    // indirect args; NOTHING draws from them yet. Call from a render-graph pass before shadows.
    void RecordCull(Renderer* renderer, ID3D12GraphicsCommandList* cl);

    // Step 4 (temporary): once, a few frames after the cull first runs, read back the GPU
    // InstanceCounts and compare per-view totals against a CPU frustum cull of the same
    // snapshot; log PASS/FAIL. Pure CPU + deferred (uses the natural per-frame fence), so no
    // mid-frame stall. Call once per frame (main thread) from Scene::Render.
    void PollValidation(Renderer* renderer);

    // Drop CPU-side state on level unload; RETAINS the GPU buffers + SRVs (the LightManager
    // lesson: a pass may reference an SRV while frames are in flight). Next Rebuild reuses them.
    void Reset();

    std::uint32_t CasterCount() const { return count_; }
    std::uint32_t ViewFrustumCount() const { return viewFrustumCount_; }
    std::uint32_t MeshGroupCount() const { return numMeshGroups_; }

    // SRVs for ring region `frameIndex` (0..kFrameCount-1); {0} if not built. For the future
    // cull compute (Step 4) / indirect VS (Step 5); unused in Steps 1-2.
    D3D12_CPU_DESCRIPTOR_HANDLE InstanceSrv(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE BoundsSrv(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE ViewFrustumSrv(UINT frameIndex) const;

    // Rung 0 (Step 3): the GPU-driven indirect-execution buffers, produced by the Step 4 cull
    // compute (UAV) and consumed by ExecuteIndirect (Step 6) / the indirect VS (Step 5).
    // DEFAULT-heap, kFrameCount-region rings; unused (no descriptors) until Step 4. Region f
    // occupies [f*RegionBytes, (f+1)*RegionBytes). Getters return null / 0 until Rebuild.
    ID3D12Resource* IndirectArgsBuffer() const { return indirectArgs_.buffer.Get(); }     // D3D12_DRAW_INDEXED_ARGUMENTS[view*group]
    ID3D12Resource* VisibleListBuffer() const { return visibleList_.buffer.Get(); }       // uint32 caster ids
    ID3D12Resource* IndirectCountsBuffer() const { return indirectCounts_.buffer.Get(); } // uint32 per-view draw count
    size_t IndirectArgsRegionBytes() const { return indirectArgs_.regionBytes; }
    size_t VisibleListRegionBytes() const { return visibleList_.regionBytes; }
    size_t IndirectCountsRegionBytes() const { return indirectCounts_.regionBytes; }

    // The depth-only indirect shadow PSO (Step 5); the ExecuteIndirect draws bind it in Step 6.
    // Null until the shader resources are created (first RecordCull). Unused in Step 5.
    Material* IndirectShadowMaterial() const { return indirectShadowMat_.get(); }

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

    // A DEFAULT-heap UAV buffer of kFrameCount regions of `regionBytes` each (the GPU cull
    // writes these, so they are GPU-local + UAV, not CPU-mapped like Ring). No descriptors
    // here — the cull (Step 4) creates the UAVs, ExecuteIndirect (Step 6) reads by address.
    struct UavRing
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        size_t regionBytes = 0; // bytes per region; region f base offset = f * regionBytes

        bool Valid() const { return buffer != nullptr; }
    };

    // Ensure `ring` holds >= `regionBytes` per region; (re)allocate on growth. Registers the
    // resource state (COMMON) with the tracker. Returns false on allocation failure.
    static bool EnsureUavRing(Renderer* renderer, UavRing& ring, size_t regionBytes,
                              const wchar_t* name);
    static void ReleaseUavRing(Renderer* renderer, UavRing& ring);

    static bool IsCaster(const RenderableObjectBase* obj);
    static void FillInstance(const RenderableObjectBase* obj, render::InstancePerObject& out);
    static void FillBounds(const RenderableObjectBase* obj, render::CasterBounds& out);

    void EnsureShaderResources(Renderer* renderer);         // lazily create cull compute + indirect-draw PSOs
    void RebuildCullDescriptors(Renderer* renderer);        // per-region UAVs for args/visible/counts
    void EnsureReadback(Renderer* renderer, size_t bytes);  // validation readback buffer (READBACK heap)

    Ring instances_;     // per-caster InstancePerObject
    Ring bounds_;        // per-caster CasterBounds
    Ring viewFrustums_;  // per-view ShadowViewFrustum
    Ring casterGroup_;   // per-caster mesh-group id (uint); static, region 0 only
    Ring perGroup_;      // per-group {base, indexCount} (uint2); static, region 0 only

    UavRing indirectArgs_;   // per (view, mesh-group) D3D12_DRAW_INDEXED_ARGUMENTS
    UavRing visibleList_;    // per (view, mesh-group) visible caster ids (uint32)
    UavRing indirectCounts_; // per view draw count (uint32)

    // Non-shader-visible UAVs for the cull outputs, one per ring region:
    // [0..kFrameCount)=args (RAW), [kFrameCount..2k)=visibleList, [2k..3k)=counts.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cullUavHeap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3 * render::kFrameCount> cullUav_{};

    std::shared_ptr<Material> cullClearMat_;     // shadow_cull_clear_cs.hlsl
    std::shared_ptr<Material> cullMat_;          // shadow_cull_cs.hlsl
    std::shared_ptr<Material> indirectShadowMat_; // shadow_indirect_csm.hlsl (Step 5; used in Step 6)
    bool shaderResourcesTried_ = false;          // one-shot creation attempt (avoid re-log on failure)

    std::uint32_t count_ = 0;            // live caster count
    std::uint32_t viewFrustumCount_ = 0; // fixed shadow-view slot count
    std::uint32_t numMeshGroups_ = 0;    // distinct caster meshes (indirect-buffer sizing)

    std::vector<render::ShadowViewFrustum> cpuViewFrustums_; // CPU mirror (validation)

    // Step 4 validation: deferred readback of the args region + a snapshot to compare against.
    Microsoft::WRL::ComPtr<ID3D12Resource> valReadback_; // READBACK heap
    std::vector<render::CasterBounds>      valBounds_;
    std::vector<render::ShadowViewFrustum> valFrustums_;
    std::uint32_t valCasters_ = 0, valViews_ = 0, valGroups_ = 0;
    std::uint64_t valFrame_ = 0;
    int           valState_ = 0; // 0 = not started, 1 = readback pending, 2 = done

    // Authoritative current per-caster values (change-detection reference) + a per-entry
    // "frames remaining to propagate a change into all regions" counter (drives both the
    // instance and bounds writes, since bounds derive from the same transform).
    std::vector<render::InstancePerObject> cpuInstances_;
    std::vector<render::CasterBounds>      cpuBounds_;
    std::vector<std::uint8_t>              pending_;

    std::uint32_t logFramesRemaining_ = 5; // one-off warmup logging (see .cpp)
};
