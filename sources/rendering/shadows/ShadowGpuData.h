#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

#include "rendering/core/RenderConstants.h"
#include "rendering/core/ResourceDeclarations.h"
#include "rendering/renderables/InstanceTypes.h"

class Renderer;
struct RenderGraphPassContext;
class RenderableObjectBase;
class Frustum;
class Material;
class Mesh;

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
    // D1.1: the gate RecordCull's one-shot validation readback uses, shared with
    // PrepareCullPass so Prepare cannot register what Record will skip.
    bool WillRecordValidationReadback(Renderer* renderer) const;
    bool WillUseUnifiedBuffers(Renderer* renderer) const;

    // Step 4 (temporary): once, a few frames after the cull first runs, read back the GPU
    // InstanceCounts and compare per-view totals against a CPU frustum cull of the same
    // snapshot; log PASS/FAIL. Pure CPU + deferred (uses the natural per-frame fence), so no
    // mid-frame stall. Call once per frame (main thread) from Scene::Render.
    void PollValidation(Renderer* renderer);

    // Step 6: true when the indirect shadow draw path can run (PSO + buffers + group meshes
    // ready, casters present). The shadow passes gate the ExecuteIndirect path on this.
    bool IndirectDrawReady() const;

    // Step 6: record the indirect depth-only shadow draws for ONE shadow view slot into the
    // currently-bound depth target (the caller set the viewport via Bind*ShadowTarget). Binds
    // the indirect PSO + instance SRV (t0) + visible-list per-instance vertex stream (slot 1) +
    // b1 viewCB, then one ExecuteIndirect per mesh-group (empty groups draw 0 instances).
    // `viewSlot` indexes the args/frustum layout (cascade i | 4+spot | 12+point-face). Returns
    // false (drew nothing) if not ready. Thread-safe for the parallel per-view shadow CLs.
    bool RecordIndirectShadowDraws(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                   std::uint32_t viewSlot, D3D12_GPU_VIRTUAL_ADDRESS viewCB);

    // Drop CPU-side state on level unload; RETAINS the GPU buffers + SRVs (the LightManager
    // lesson: a pass may reference an SRV while frames are in flight). Next Rebuild reuses them.
    void Reset();

    std::uint32_t CasterCount() const { return count_; }
    // The caster count actually populated in the unified buffers + visited by the cull THIS frame:
    // count_ (static + GI) when GI folding is active, else staticCount_ (GI region left stale). The
    // VSM per-page cull must use this so it never tests the un-scattered GI bounds when GI is off.
    std::uint32_t ActiveCasterCount() const { return IsGiIndirectActive() ? count_ : staticCount_; }

    // GI→VSM (Step 4): is the GI folding path active THIS frame — the runtime flag is on, there are
    // folded GI casters, and the scatter PSO is ready? When true, RecordCull scatters + culls all
    // count_ casters (GI included); when false it culls only the static Nstatic (GI stays dormant).
    bool IsGiIndirectActive() const;
    // True when `obj` is a GI object folded into the indirect path AND that path is active this frame
    // (IsGiIndirectActive). The Legacy shadow passes skip such objects in their CPU RenderShadow tail
    // (the indirect path draws them); everything else — flag off, over-cap, PSO failure — keeps the tail.
    bool IsGiFoldedActive(const RenderableObjectBase* obj) const;
    // Casters re-uploaded by the last UpdateForFrame (movers this frame + recent movers still
    // propagating across ring regions). >0 means shadow content changed -> VSM must re-render even
    // if the camera is still (drives SceneRenderer's skip-when-still gate).
    std::uint32_t MoverCount() const { return lastMoverCount_; }
    // W5: true when any caster carries windStrength > 0. Such a caster animates purely in the VERTEX
    // shader — its transform never changes, so MoverCount() stays 0 and every "nothing moved, reuse
    // last frame" gate (the VSM still-frame skip, the VSM per-page cache) would freeze its shadow
    // while the gbuffer tree keeps swaying. Those gates must consult this too.
    bool HasWindCasters() const { return hasWindCasters_; }
    // A GPU-idle editor rebuild can replace masked-material SRVs without moving any caster.
    // Preserve that content-change signal through the next UpdateForFrame so VSM does not keep
    // cached pages rendered with the old alpha mask.
    void ForceContentRefreshNextFrame() { forceContentRefresh_ = true; }
    std::uint32_t ViewFrustumCount() const { return viewFrustumCount_; }
    std::uint32_t MeshGroupCount() const { return numMeshGroups_; }

    // The shadow LOD bias the current caster geometry was built with (see render::g_shadowLodBias).
    // Scene compares this to the live global each frame and triggers a GPU-idle rebuild on a change.
    int BuiltShadowLod() const { return builtShadowLod_; }
    // Groups [0, StaticGroupCount()) are static submesh groups (biased to BuiltShadowLod()); groups
    // at/after it are GI whole-buffer groups (always LOD0). The per-group fallback binds accordingly.
    std::uint32_t StaticGroupCount() const { return numStaticGroups_; }

    // SRVs for ring region `frameIndex` (0..kFrameCount-1); {0} if not built. For the future
    // cull compute (Step 4) / indirect VS (Step 5); unused in Steps 1-2.
    D3D12_CPU_DESCRIPTOR_HANDLE InstanceSrv(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE BoundsSrv(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE ViewFrustumSrv(UINT frameIndex) const;
    // Per-caster mesh-group id SRV (static, region 0). Used by the VSM per-page cull to bucket each
    // visible caster into its mesh-group's draw slice. {0} until Rebuild.
    D3D12_CPU_DESCRIPTOR_HANDLE CasterGroupSrv() const;
    // Per-caster dynamic flag SRV (static, region 0; 1 = animating). The VSM page-cache marks a page
    // dirty when a dynamic caster overlaps it. {0} until Rebuild.
    D3D12_CPU_DESCRIPTOR_HANDLE CasterMetaSrv() const;
    // Per-group {visible-list base, index count, start index, 0}. The VSM scatter cull reads .x as
    // the group's global base inside every page's visible-list slice.
    D3D12_CPU_DESCRIPTOR_HANDLE PerGroupSrv() const;

    // GI→VSM (Step 2): SRVs onto the DEFAULT-heap "unified" instance/bounds buffers for ring region
    // `frameIndex`. RecordCull copies the upload ring's region into these each frame (a compute
    // shader will also scatter GI casters into them in Step 4), so the cull reads UnifiedBoundsSrv
    // and the indirect VS reads UnifiedInstanceSrv at t0. {0} until Rebuild allocates them.
    D3D12_CPU_DESCRIPTOR_HANDLE UnifiedInstanceSrv(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE UnifiedBoundsSrv(UINT frameIndex) const;
    // GI→VSM (Step 4): UAVs onto the same unified buffers for the GI-scatter compute write.
    D3D12_CPU_DESCRIPTOR_HANDLE UnifiedInstanceUav(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE UnifiedBoundsUav(UINT frameIndex) const;
    // The instance/bounds SRV a reader should bind: the unified copy when built (Step 2), else the
    // upload ring (fallback if allocation failed). Both back a verbatim per-caster stream, so
    // callers are agnostic to which one is used.
    D3D12_CPU_DESCRIPTOR_HANDLE InstanceReadSrv(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE BoundsReadSrv(UINT frameIndex) const;

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
    // Null until the shader resources are created (first RecordCull). C2: when the caster set
    // contains masked (alphaMode=MASK) groups, this returns the SHADOW_MASKED variant instead —
    // one PSO for the whole set (opaque groups early-out in the PS), so the single-ExecuteIndirect
    // structure of both consumers (per-view CSM + VSM per-page) is preserved.
    Material* IndirectShadowMaterial() const; // defined in the .cpp (needs Material's definition)

    // Same selection rule, but the VSM_PAGE permutations: ONE ExecuteIndirect over every pool page
    // instead of the per-page CPU loop. Null when those PSOs failed to build, which is the signal
    // for VirtualShadowMap::RecordPageRender to stay on the loop. Never used by the Legacy path.
    Material* IndirectShadowPageMaterial() const;

    // Barrier plan step 5: what RecordCull transitions, registered by the owner rather than
    // exposed buffer by buffer. Same shape as VirtualShadowMap::PrepareRequestPass — the
    // context is forward-declared, RenderGraph.h is included only in the .cpp.
    void PrepareCullPass(RenderGraphPassContext& ctx) const;

    // Barrier plan step 4/5: create the cull + indirect-draw PSOs once per frame BEFORE the
    // graph runs. RecordCull used to do this itself, which meant PrepareCullPass ran while
    // giScatterMat_ still did not exist: IsGiIndirectActive() read false, the GI buffers went
    // unregistered, and then the body created the material and transitioned them anyway.
    // The comparator caught exactly that as two MISSING lines.
    void EnsureShaderResources(Renderer* renderer);

    // C2: masked-shadow bindings for the indirect draw call sites. When active, srvTable[0] must
    // carry {instances, casterGroup, groupMask} and srvTable[3] the masked albedo table.
    static constexpr std::uint32_t kMaxMaskedGroups = 16; // matches gMaskAlbedo[16] in shadow_indirect_csm.hlsl
    bool MaskedShadowsActive() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GroupMaskSrv() const { return groupMask_.Srv(0); }
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxMaskedGroups>& MaskedAlbedoSrvs() const { return maskedAlbedoSrvs_; }
    std::uint32_t MaskedAlbedoCount() const { return maskedAlbedoCount_; }

    // Rung 2 / Step 22: mesh-group id -> Mesh* (VB/IB for the per-page indirect draws, reusing this
    // frame's per-view cull output). Empty until Rebuild.
    const std::vector<const Mesh*>& GroupMeshes() const { return groupMesh_; }

    // Rung 2 mega-buffer (VSM per-page draws): all caster mesh groups concatenated into ONE VB + ONE
    // IB, so the per-page render binds geometry ONCE and issues a single ExecuteIndirect(maxCount=
    // groups) per page instead of a bind + draw per (page, mesh-group) — ~4x fewer CPU calls, same
    // frame, no latency. Ready only when every group shares a vertex stride + R32 index format (the
    // common MeshManager/PNTUV case); otherwise the VSM path falls back to per-group binding. The
    // per-group offsets are in vertices / indices (added into the draw args by the setup shader).
    void EnsureMegaBuffer(Renderer* renderer, ID3D12GraphicsCommandList* cl); // one-time build on `cl`
    bool MegaReady() const { return megaReady_; }
    ID3D12Resource* MegaVertexBuffer() const { return megaVB_.Get(); }
    ID3D12Resource* MegaIndexBuffer() const { return megaIB_.Get(); }
    UINT MegaVertexBytes() const { return megaVBBytes_; }
    UINT MegaIndexBytes() const { return megaIBBytes_; }
    UINT MegaStride() const { return megaStride_; }
    DXGI_FORMAT MegaIndexFormat() const { return megaIndexFormat_; }
    std::uint32_t GroupBaseVertex(std::uint32_t g) const { return g < baseVertex_.size() ? baseVertex_[g] : 0u; }
    std::uint32_t GroupStartIndex(std::uint32_t g) const { return g < startIndex_.size() ? startIndex_[g] : 0u; }
    // Per-view LOD + per-(group,lod) mega table for the VSM setup CB (see viewLod_/groupLodMega_).
    const std::vector<std::uint32_t>& ViewLod() const { return viewLod_; }
    const std::vector<std::uint32_t>& GroupLodMegaTable() const { return groupLodMega_; }
    // Shadow LOD to bind a mesh's own index buffer at, for the per-page/per-view geometry FALLBACK
    // (mega off) and the Legacy per-view path. `cullView` indexes the cull-view layout; clamp per mesh
    // at the call site via Mesh::ClampExplicitLod. Returns 0 if out of range.
    std::uint32_t ViewLodAt(std::uint32_t cullView) const { return cullView < viewLod_.size() ? viewLod_[cullView] : 0u; }
    D3D12_CPU_DESCRIPTOR_HANDLE PerViewGroupSrv() const;

private:
    // One upload-heap structured buffer of kFrameCount regions x `capacity` elements of
    // `stride` bytes, persistently mapped, with one SRV per region. The shared boilerplate
    // behind all three logical buffers above.
    struct Ring
    {
        GpuResource                                   buffer; // step 6b: self-unregistering
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
        GpuResource buffer; // step 6b: self-unregistering
        size_t regionBytes = 0; // bytes per region; region f base offset = f * regionBytes

        bool Valid() const { return static_cast<bool>(buffer); } // GpuResource: explicit operator bool
    };

    // Ensure `ring` holds >= `regionBytes` per region; (re)allocate on growth. Registers the
    // resource state (COMMON) with the tracker. Returns false on allocation failure.
    // `canonical` = the state the FRAME LEAVES this buffer in (barrier plan step 6b). It differs
    // per buffer even though one helper creates them all, so the caller states it.
    static bool EnsureUavRing(Renderer* renderer, UavRing& ring, size_t regionBytes,
                              D3D12_RESOURCE_STATES canonical,
                              const wchar_t* name);
    static void ReleaseUavRing(Renderer* renderer, UavRing& ring);

    static bool IsCaster(const RenderableObjectBase* obj);
    // GI→VSM (Step 3): a GPU-instanced caster eligible to be folded into the caster set (casts
    // shadow, has a valid per-instance transform SRV + count + mesh). Distinct from IsCaster,
    // which excludes GPU-instanced casters.
    static bool IsGiFoldable(const RenderableObjectBase* obj);
    static void FillInstance(const RenderableObjectBase* obj, render::InstancePerObject& out);
    // The caster's world AABB, verbatim — deliberately NOT padded for the wind sway (see the
    // definition for the rationale and the measured cost of padding).
    static void FillBounds(const RenderableObjectBase* obj, render::CasterBounds& out);

    void RebuildCullDescriptors(Renderer* renderer);        // per-region UAVs for args/visible/counts
    void RebuildUnifiedDescriptors(Renderer* renderer);     // per-region SRVs for the unified instance/bounds buffers
    void EnsureReadback(Renderer* renderer, size_t bytes);  // validation readback buffer (READBACK heap)

    Ring instances_;     // per-caster InstancePerObject
    Ring bounds_;        // per-caster CasterBounds
    Ring viewFrustums_;  // per-view ShadowViewFrustum
    Ring casterGroup_;   // per-caster mesh-group id (uint); static, region 0 only
    Ring casterMeta_;    // per-caster meta (uint: bit0=dynamic, bits1+=object slot count on its FIRST slot); static, region 0 only
    Ring perGroup_;      // per-group {base, indexCount, startIndex, 0} (uint4); static, region 0 only
    Ring perViewGroup_;  // per (view, group) {base, indexCount@viewLOD, startIndex@viewLOD, 0} (uint4);
                         // static, region 0. Seeds the cull-clear args with each view's LOD (Legacy + Rung0).

    UavRing indirectArgs_;   // per (view, mesh-group) D3D12_DRAW_INDEXED_ARGUMENTS
    UavRing visibleList_;    // per (view, mesh-group) visible caster ids (uint32)
    UavRing indirectCounts_; // per view draw count (uint32)

    // GI→VSM (Step 2/4): DEFAULT-heap mirrors of instances_/bounds_ (kFrameCount regions x count_).
    // RecordCull copies the upload ring's static region into these each frame and (Step 4) scatters
    // the GI casters' region via a compute UAV write, so the cull + indirect VS read GPU-local copies
    // that a compute shader can also write. One CPU (non-shader-visible) heap holds per-region
    // descriptors: [0..k)=instance SRV, [k..2k)=bounds SRV, [2k..3k)=instance UAV, [3k..4k)=bounds UAV.
    UavRing instancesUnified_;
    UavRing boundsUnified_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> unifiedSrvHeap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4 * render::kFrameCount> unifiedDescr_{};

    // Non-shader-visible UAVs for the cull outputs, one per ring region:
    // [0..kFrameCount)=args (RAW), [kFrameCount..2k)=visibleList, [2k..3k)=counts.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cullUavHeap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3 * render::kFrameCount> cullUav_{};

    std::shared_ptr<Material> cullClearMat_;     // shadow_cull_clear_cs.hlsl
    std::shared_ptr<Material> cullMat_;          // shadow_cull_cs.hlsl
    std::shared_ptr<Material> indirectShadowMat_; // shadow_indirect_csm.hlsl (Step 5; used in Step 6)
    std::shared_ptr<Material> indirectShadowMaskedMat_; // C2: SHADOW_MASKED=1 variant (alpha-tested groups)
    // VSM single-draw page render: VSM_PAGE=1 permutations (page id unpacked from the caster id,
    // projection from an SRV, page borders via SV_ClipDistance). Optional — a failure just keeps the
    // per-page loop. Used ONLY by VirtualShadowMap::RecordPageRender, never by the Legacy path.
    std::shared_ptr<Material> indirectShadowPageMat_;
    std::shared_ptr<Material> indirectShadowPageMaskedMat_;
    std::shared_ptr<Material> giScatterMat_;     // shadow_gi_scatter_cs.hlsl (Step 4)
    bool shaderResourcesTried_ = false;          // one-shot creation attempt (avoid re-log on failure)

    // C2: per-group mask data — groupMask_ holds uint2 {albedo slot (~0 = opaque), asuint(cutoff)}
    // per mesh-group (region 0, static after Rebuild); maskedAlbedoSrvs_ are the masked groups'
    // albedo SRV CPU handles, staged into the masked PSO's t3 table by the draw call sites. The
    // handles point into MaterialData-owned descriptors — Rebuild refreshes them, Reset drops them
    // (they dangle across a level unload, same contract as groupMesh_).
    Ring groupMask_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxMaskedGroups> maskedAlbedoSrvs_{};
    std::uint32_t maskedAlbedoCount_ = 0;
    bool hasMaskedGroups_ = false;
    bool hasWindCasters_ = false; // W5: any caster with windStrength > 0 (see HasWindCasters)

    std::uint32_t count_ = 0;            // live caster count (TOTAL: static + folded GI instances)
    std::uint32_t staticCount_ = 0;      // CPU static casters (id range [0, staticCount_)); GI ids follow
    std::uint32_t lastMoverCount_ = 0;   // casters re-uploaded last UpdateForFrame (VSM skip gate)
    bool          forceContentRefresh_ = false; // editor rebuild changed material/geometry content
    std::uint32_t viewFrustumCount_ = 0; // fixed shadow-view slot count
    std::uint32_t numMeshGroups_ = 0;    // distinct caster meshes (indirect-buffer sizing)

    std::vector<const Mesh*> groupMesh_; // mesh-group id -> Mesh* (VB/IB for the indirect draws)

    // GI→VSM (Step 3): one entry per folded GPU-instanced caster. The scatter compute (Step 4)
    // walks this to write each GI object's instances into the unified buffers' GI id sub-range
    // [giBase, giBase+count). aabbCenter/aabbExtent are the mesh-LOCAL AABB (world AABB per instance
    // is derived on the GPU). `obj` is refreshed every Rebuild (lifetime = the scene's object list).
    struct GiCaster
    {
        RenderableObjectBase* obj = nullptr;
        std::uint32_t         giBase = 0;   // first caster id of this object's instances
        std::uint32_t         count = 0;    // instance count
        DirectX::XMFLOAT4     aabbCenter{}; // mesh-local AABB center
        DirectX::XMFLOAT4     aabbExtent{}; // mesh-local AABB half-extents
    };
    std::vector<GiCaster> giCasters_;
    std::uint32_t         giFoldableInstances_ = 0; // Σ instance counts of ALL foldable GI (pre-cap); change-detector for UpdateForFrame

    // Rung 2 mega-buffer: all group meshes concatenated into one VB/IB (see EnsureMegaBuffer).
    // megaWanted_ = layout is uniform + within limits (set in Rebuild); megaBuilt_ = the one-time
    // GPU copy has run (one-shot, success or fail); megaReady_ = built + usable.
    // Step 6b part 2: on the wrapper. The old ComPtr was Reset() on a level change, which
    // freed the buffer WITHOUT unregistering it — a measured leak (net climbed 2->5).
    GpuResource megaVB_;
    GpuResource megaIB_;
    std::vector<std::uint32_t> baseVertex_;   // per group: its MESH's vertex offset into megaVB_ (B3: submesh groups share it)
    std::vector<std::uint32_t> startIndex_;   // per group: its MESH's index offset into megaIB_ (the group's submesh range rides in the args)
    // Per-view shadow LOD (cull-view layout: [cascades | spots | point-faces | clipmap]); = the view's
    // tier base LOD + g_shadowLodBias, UNclamped per mesh (the mega table clamps per mesh). Consumed by
    // the VSM setup CB + the Legacy per-view index-buffer bind.
    std::vector<std::uint32_t> viewLod_;
    // Per (group, lod) mega geometry, flat 4 uints/entry: {megaAbsStart, lodRelStart, indexCount,
    // baseVertex}, pre-clamped to the mesh's available LODs. numMeshGroups_ * kMaxShadowLods entries.
    std::vector<std::uint32_t> groupLodMega_;
    // B3: mega copy list per UNIQUE mesh (submesh groups share one VB slice). Per-view shadow LOD: the
    // mesh's IB slot concatenates its first `lodCount` LOD index buffers ([LOD0|LOD1|...]), so different
    // shadow views can draw different LODs of the same mesh from one mega buffer. The VB (shared across
    // LODs) is copied once; `lodCount` = mesh LOD count for static casters, 1 for GI-only (LOD0) meshes.
    struct MegaCopy { const Mesh* mesh = nullptr; UINT vbBytes = 0; UINT lodCount = 1; };
    std::vector<MegaCopy> megaCopy_;
    UINT megaVBBytes_ = 0, megaIBBytes_ = 0, megaStride_ = 0;
    DXGI_FORMAT megaIndexFormat_ = DXGI_FORMAT_R32_UINT;
    bool megaWanted_ = false, megaBuilt_ = false, megaReady_ = false;
    int builtShadowLod_ = 0; // render::g_shadowLodBias snapshot the caster geometry was built with
    std::uint32_t numStaticGroups_ = 0; // count of static submesh groups (the rest are GI, always LOD0)

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
