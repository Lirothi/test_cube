#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

#include "core/containers/inl_vector.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/ResourceDeclarations.h"
#include "rendering/renderables/InstanceTypes.h"
#include "rendering/shadows/CascadeHzb.h"       // occlusion plan S5b: the cascade light-space pyramids
#include "rendering/shadows/VirtualShadowMap.h" // vsm::kMaxMeshGroups (the per-group override table)

class Renderer;
struct RenderGraphPassContext;
class RenderableObjectBase;
class Frustum;
class Material;
class MaterialData;
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
//  3. Per-view FRUSTUM planes (Step 2) — `render::ShadowViewFrustum` (up to 12 inward planes) for
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
    // pass-flow S7a: this frame's cull decisions and the ABSOLUTE indices of the barrier points
    // they were declared under. Produced by PrepareCullPass (which runs in the AddPass2 builder,
    // serially, before any recording) and handed straight to RecordCull, so the two cannot
    // disagree — which is what the two `Will*` predicates existed to prevent by hand.
    //
    // `giCasterIdx` is the FILTERED caster list, not a re-derivable hint: the declaration walk and
    // the record walk have to skip exactly the same entries, and the only way to guarantee that is
    // for the record to iterate what the declaration produced.
    struct CullDecisions
    {
        bool active = false;      // every gate RecordCull used to re-check
        bool useUnified = false;  // the DEFAULT-heap instance/bounds mirror is usable this frame
        bool giOn = false;        // GI folding runs, so the scatter fills the GI region
        bool readback = false;    // the one-shot validation readback runs on this frame
        // Occlusion plan S5b: the cascade views' casters are tested against last frame's light
        // pyramids and the hidden ones deferred (never on a validation-readback frame: the CPU
        // reference is the frustum alone). Also what the three S5b passes gate on this frame.
        bool hzb = false;
        // Occlusion plan S4: the cull also fills the camera's args + visible list this frame.
        bool gbuffer = false;
        // Occlusion plan S5: ...and tests them against last frame's camera pyramid, deferring
        // the hidden ones (the previous pyramid was valid; the three S5 passes run).
        bool camHzb = false;
        std::uint32_t base = 0;         // cull outputs -> UAV
        std::uint32_t unifiedCopy = 0;  // unified instance/bounds -> COPY_DEST
        std::uint32_t giWrite = 0;      // ...-> UAV for the scatter
        std::uint32_t giRead = 0;       // each folded object's instance buffer -> NPS
        std::uint32_t giRestore = 0;    // ...and back to its owner's canonical
        std::uint32_t unifiedRead = 0;  // unified instance/bounds -> NPS for the cull + the draws
        std::uint32_t valCopy = 0;      // indirect args -> COPY_SOURCE for the readback
        std::uint32_t consume = 0;      // args -> INDIRECT_ARGUMENT, visible list -> vertex stream
        tc::inl_vector<std::uint16_t, vsm::kMaxMeshGroups> giCasterIdx;
    };
    // NOT const: it commits this frame's cross-frame state — the one-shot validation snapshot and
    // the readback buffer — the way an AddPass2 builder is supposed to, instead of leaving it to
    // the record body where an inner allocation failure could skip a point that was declared.
    CullDecisions PrepareCullPass(RenderGraphPassContext& ctx);
    void RecordCull(Renderer* renderer, ID3D12GraphicsCommandList* cl, const CullDecisions& dec);

    // Occlusion plan S5b: the POST cull (Main_ShadowCullPost, after Main_CsmHzb rebuilt the
    // cascade pyramids from pass A's tiles): the deferred casters against THIS frame's pyramids
    // -> pass-B args + visible list, consumed by Main_CSMPost. Same builder/record contract as
    // the main cull. `stats` = this frame's counters are copied out for the readout.
    struct CullPostDecisions
    {
        bool active = false;
        bool stats = false;
        std::uint32_t base = 0;     // pass-B args/list + deferred -> UAV (already, from the main cull)
        std::uint32_t consume = 0;  // args -> INDIRECT_ARGUMENT, list -> vertex stream, counters -> COPY_SOURCE
        std::uint32_t restore = 0;  // counters -> UAV (their canonical)
    };
    CullPostDecisions PrepareCullPostPass(RenderGraphPassContext& ctx);
    void RecordCullPost(Renderer* renderer, ID3D12GraphicsCommandList* cl, const CullPostDecisions& dec);
    // Decided by PrepareCullPass; the S5b passes' builders read it (builders run serially, in
    // schedule order, so the cull's decision is final by the time they ask).
    bool CascadeHzbCullThisFrame() const { return hzbCullThisFrame_; }

    // ---- Occlusion plan S4: the camera's indirect G-buffer -------------------------------------
    // The registry already holds every shadow caster's record (world, material params per submesh
    // slot since S4, wind) and bounds; the same cull tests them against the CAMERA frustum (slot
    // kMaxShadowViews of the frustum ring) into the camera's own args + visible list, and
    // Main_GBuffer's indirect pass draws one ExecuteIndirect per (group, camera LOD) with the
    // group's material bound by the CPU. An object is ELIGIBLE when every one of its submesh
    // slots has the indirect PSO and the same MaterialData as its group's first object (decided
    // at Rebuild, stamped on the object); the camera prepare drops eligible objects from the CPU
    // buckets on the frames this runs, so nothing draws twice. Per frame the cull also honours
    // the camera occlusion verdict (S3a) and the chunk mask (S1) through per-caster flags, and
    // draws a crossfading caster in BOTH LOD buckets (the CPU path's two draws).
    // Once per frame from Scene, BEFORE PrepareViews: run the indirect G-buffer this frame?
    void SetGBufferIndirect(bool on) { gbufferIndirectFrame_ = on; }
    bool GBufferIndirectThisFrame() const { return gbufferIndirectFrame_; }
    // The registry has eligible objects and the camera rings: what Scene asks before switching on.
    bool GBufferIndirectReady() const;
    std::uint32_t GBufferIndirectEligibleCasters() const { return eligibleCasterCount_; }
    // The Main_GBuffer indirect pass body: the render targets are bound by the caller; one
    // ExecuteIndirect per virtual group in [vgBegin, vgEnd) with camera candidates, the group's
    // PSO/textures/surface params bound here. The pass fans the range out over worker lists.
    // Returns false when nothing could be recorded.
    // S5: `passB` draws the post cull's pair (the bad guesses of the main cull) instead of pass A's.
    bool RecordIndirectGBufferDraws(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                    D3D12_GPU_VIRTUAL_ADDRESS viewCB, bool wireframe,
                                    std::uint32_t vgBegin, std::uint32_t vgEnd, bool passB = false);
    std::uint32_t VirtualGroupCount() const { return numVirtualGroups_; }
    // A material hot reload may have rebuilt the PSOs the groups cached: rebuild the registry on
    // the next frame (the caster count alone would not notice).
    void InvalidateGroupMaterials() { rebuildPending_ = true; }

    // ---- Occlusion plan S5: the camera's two-pass HZB occlusion ---------------------------------
    // Mirrors CameraHzbCB (b2 of shadow_cull_cs.hlsl, b1 of cam_cull_post_cs.hlsl).
    struct CameraHzbParams
    {
        Math::mat4 prevViewProj;    // last frame's camera, no jitter
        Math::mat4 prevViewToClip;
        Math::mat4 viewProj;        // this frame's, jittered (the pyramid is the jittered depth)
        Math::mat4 viewToClip;
        std::int32_t viewRect[4];   // (0, 0, renderWidth, renderHeight)
        std::uint32_t hzbSize[2];   // mip 0
        std::uint32_t prevValid;    // last frame's pyramid: this camera's, same size, built last frame
        std::uint32_t on;
    };
    static_assert(sizeof(CameraHzbParams) == 288, "CameraHzbCB layout");
    // Once per frame from SceneRenderer::DecideFrame, before the graph is built: the matrices,
    // the validity of the previous pyramid and the two pyramids' resources (prev = the previous
    // frame slot's, current = this frame's, rebuilt after pass A by Main_HzbA).
    void SetCameraHzb(const CameraHzbParams& params, ID3D12Resource* prevHzb, D3D12_CPU_DESCRIPTOR_HANDLE prevHzbSrv,
                      ID3D12Resource* curHzb, D3D12_CPU_DESCRIPTOR_HANDLE curHzbSrv);
    // Decided by PrepareCullPass: the main cull deferred against the previous pyramid this frame,
    // so Main_HzbA / Main_CamCullPost / Main_GBufferB run.
    bool CamHzbThisFrame() const { return camHzbThisFrame_; }
    struct CamCullPostDecisions
    {
        bool active = false;
        bool stats = false;
        std::uint32_t base = 0;     // pyramid readable, the pair + deferred list UAV
        std::uint32_t consume = 0;  // args -> INDIRECT_ARGUMENT, list -> vertex stream, counters -> COPY_SOURCE
        std::uint32_t restore = 0;  // counters -> UAV
    };
    CamCullPostDecisions PrepareCamCullPostPass(RenderGraphPassContext& ctx);
    void RecordCamCullPost(Renderer* renderer, ID3D12GraphicsCommandList* cl, const CamCullPostDecisions& dec);
    // The readout: this frame's deferred (weighted) and pass-B entries of frame N - kFrameCount.
    std::uint32_t CamHzbDeferred() const { return hzbStats_[9]; }
    std::uint32_t CamHzbDrawnB() const { return hzbStats_[10]; }
    render::CascadeHzb& CascadeHzbRef() { return cascadeHzb_; }
    // Once per frame from Scene, after UpdateCascades and UpdateViewFrustums: this frame's
    // cascade light view-projections (forward-Z), whether the knob + mode want the test, and
    // the tile content size the pyramids are built over. Creates the pyramids on first use.
    void SetCascadeHzbViews(Renderer* renderer, const Math::mat4* lightViewProj, std::uint64_t frameNumber,
                            bool wantOn, UINT contentRes);
    // The counters of frame N - kFrameCount, mapped when its fence has passed: per cascade,
    // casters the main cull deferred and casters pass B drew (the cut = the difference).
    void PollHzbStats(Renderer* renderer);
    std::uint32_t HzbDeferred(unsigned c) const { return c < render::CascadeHzb::kCascades ? hzbStats_[c] : 0u; }
    std::uint32_t HzbDrawnB(unsigned c) const { return c < render::CascadeHzb::kCascades ? hzbStats_[render::CascadeHzb::kCascades + c] : 0u; }

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
    // S5b: `passB` draws the post cull's args/list (cascade rows only) instead of the main cull's.
    bool RecordIndirectShadowDraws(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                   std::uint32_t viewSlot, D3D12_GPU_VIRTUAL_ADDRESS viewCB,
                                   bool passB = false);

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

    // The shadow LOD curve the current caster tables were built with. Scene compares both values to
    // the live globals each frame and triggers a GPU-idle rebuild on a change.
    int BuiltShadowLod() const { return builtShadowLod_; }
    int BuiltShadowLodTierStride() const { return builtShadowLodTierStride_; }
    // Part of the same snapshot: it changes the per-view LOD table, so a change needs the same
    // GPU-idle rebuild. Without this the toggle is inert at runtime and reads as a no-op knob.
    bool BuiltShadowLodBiasNearTier() const { return builtShadowLodBiasNearTier_; }
    // Chunked-terrain LOD: per-group ABSOLUTE LOD override (-1 = none), refreshed EVERY FRAME in
    // UpdateForFrame from each chunked object's ChunkCameraLods() — the camera tier per chunk. No
    // rebuild involved: the mega buffer holds every LOD and gGroupLodMega carries every (group,lod)
    // range, so matching the caster to the drawn LOD is per-frame CB data. Consumed by the VSM
    // setup pass (as an SRV, region `frameIndex` — it is rewritten every frame); the Legacy
    // per-view loop reads the object array directly (RenderableObject::RenderShadow).
    D3D12_CPU_DESCRIPTOR_HANDLE GroupLodOverrideSrv(UINT frameIndex) const;
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
    // Per FRAME now: the dynamic bit is republished each frame so a caster that MOVED this frame
    // reads as dynamic, which is what lets the page cache dirty only ITS pages (Step 4 of
    // docs/vsm_page_caching_plan.md) instead of forcing the whole pool.
    D3D12_CPU_DESCRIPTOR_HANDLE CasterMetaSrv(UINT frameIndex) const;
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

    // Same shader as IndirectShadowMaterial but with the VSM POOL's DSV format (D32_FLOAT — the
    // pool is 32-bit so the receiver-plane bias can run with a ~zero constant; the Legacy atlases
    // stay D16). Used ONLY by the VSM per-page LOOP fallback, which draws into the pool with the
    // non-VSM_PAGE shader — one PSO cannot serve two depth formats.
    Material* IndirectShadowPoolMaterial() const;

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
    // Per-view LOD for the VSM setup CB (44 views, fixed) — see viewLod_.
    const std::vector<std::uint32_t>& ViewLod() const { return viewLod_; }
    // Per-(group,lod) mega ranges as an SRV (static, region 0). A CB array would cap the group
    // count at the CB's compile-time size; this is sized by numMeshGroups_ (see groupLodMega_).
    D3D12_CPU_DESCRIPTOR_HANDLE GroupLodMegaSrv() const;
    // Shadow LOD to bind a mesh's own index buffer at, for the per-page/per-view geometry FALLBACK
    // (mega off) and the Legacy per-view path. `cullView` indexes the cull-view layout; clamp per mesh
    // at the call site via Mesh::ClampExplicitLod. Returns 0 if out of range.
    std::uint32_t ViewLodAt(std::uint32_t cullView) const { return cullView < viewLod_.size() ? viewLod_[cullView] : 0u; }
    // S3.6: per VIRTUAL group (group * kMaxShadowLods + receiverLod). Per-frame: the .x base moves
    // as instances migrate between LODs. {base, indexCount, lodRelStartIndex, casterCount}.
    D3D12_CPU_DESCRIPTOR_HANDLE PerGroupVgSrv(UINT frameIndex) const;

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
    // S4: the record of caster slot `ordinal` (a submesh) with THAT submesh's material slot params
    // (GBufferRenderable::FillInstanceDataForSlot); other objects fall back to FillInstance.
    static void FillInstanceSlot(const RenderableObjectBase* obj, size_t ordinal, render::InstancePerObject& out);
    // The material slot a mesh's submesh `ordinal` draws with, clamped to the object's slot count
    // (the CPU per-submesh loop's rule).
    static std::uint32_t MaterialSlotOf(const Mesh* mesh, size_t ordinal, size_t slotCount);
    // The caster's world AABB, verbatim — deliberately NOT padded for the wind sway (see the
    // definition for the rationale and the measured cost of padding).
    static void FillBounds(const RenderableObjectBase* obj, render::CasterBounds& out);

public:
    // Per-caster shadow LOD (the caster==receiver contract, per INSTANCE): refresh casterLod_ (one
    // entry per caster slot; the VSM scatter buckets instances by it) and groupLodOverride_ (chunk
    // EXACT, brute-fallback only) from the camera. Scene calls it every frame AFTER PrepareViews
    // (whose SelectLods picked the receiver tiers) — earlier and the caster would lag the receiver
    // by one frame at every LOD transition. Takes the renderer because it UPLOADS: the tables land
    // in this frame's ring regions, and Scene calls this after UpdateForFrame, so there is no later
    // per-frame hook to defer to.
    // S3.6: per-frame virtual-group bucketing + visible-list bases (see the .cpp).
    void RefreshVirtualGroups(Renderer* renderer);
    // S4: `cameraOcclusionValid` = Scene::ApplyOcclusion ran this frame, so each object's
    // CameraOccluded() is this frame's verdict (false = the history is off; the flag is stale).
    // `frameNumber` = the stamp the camera prepare put on the objects its frustum kept.
    void RefreshCasterLods(Renderer* renderer,
                           const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
                           const Math::float3& cameraPos, bool cameraOcclusionValid,
                           std::uint64_t frameNumber);
    // TRUE exactly once after a frame in which any caster's LOD changed (receiver LOD moved, or
    // a chunk crossed a tier). The page cache must flush then: a cached page holds geometry at
    // the OLD LOD, and the scatter will bucket the caster into a different virtual group, so
    // the cached content and the new args disagree. Consumed by ComputePageRenderDecisions.
    // Camera position captured by RefreshCasterLods this frame — the VSM wind falloff's
    // origin (gWindFade). Lives here because this class already receives the camera each
    // frame and VirtualShadowMap does not.
    const DirectX::XMFLOAT3& LastCameraPos() const { return lastCameraPos_; }

    bool ConsumeCasterLodsChanged()
    {
        const bool c = casterLodsChanged_;
        casterLodsChanged_ = false;
        return c;
    }
    // Per-frame SRV of the per-caster LOD table (vsm_page_scatter_cs t5).
    D3D12_CPU_DESCRIPTOR_HANDLE CasterLodSrv(UINT frameIndex) const;

private:
    // Copy casterLod_ + groupLodOverride_ into this frame's ring regions (growing as needed).
    void UploadCasterLods(Renderer* renderer);


    void RebuildCullDescriptors(Renderer* renderer);        // per-region UAVs for args/visible/counts
    void RebuildUnifiedDescriptors(Renderer* renderer);     // per-region SRVs for the unified instance/bounds buffers
    void EnsureReadback(Renderer* renderer, size_t bytes);  // validation readback buffer (READBACK heap)

    Ring instances_;     // per-caster InstancePerObject
    Ring bounds_;        // per-caster CasterBounds
    Ring viewFrustums_;  // per-view ShadowViewFrustum
    Ring casterGroup_;   // per-caster mesh-group id (uint); static, region 0 only
    Ring casterMeta_;    // per-caster meta (uint: bit0=dynamic, bits1+=object slot count on its FIRST slot); static, region 0 only
    Ring perGroup_;      // per-group {base, indexCount, startIndex, 0} (uint4); static, region 0 only
    // S3.6: the caster==receiver LOD contract for the Legacy/Rung-0 path. A caster's LOD comes from
    // its RECEIVER (casterLod_), exactly as UE takes it from CurrentView.PrimitivesLODMask, so it is
    // the SAME for every shadow view — which is why the bucketing is done once on the CPU here
    // rather than per view in a scatter (VSM needs the latter only because it kept a per-level floor).
    Ring perGroupVg_;                        // per virtual group, PER FRAME
    std::uint32_t numVirtualGroups_ = 0;     // numMeshGroups_ * render::kMaxShadowLods
    std::vector<std::uint32_t> casterGroupCpu_; // caster -> REAL group id (Rebuild's mapping, kept)
    std::vector<std::uint32_t> vgCasterCount_;  // per virtual group, this frame (0 => skip the draw)
                         // static, region 0. Seeds the cull-clear args with each view's LOD (Legacy + Rung0).

    UavRing indirectArgs_;   // per (view, mesh-group) D3D12_DRAW_INDEXED_ARGUMENTS
    UavRing visibleList_;    // per (view, mesh-group) visible caster ids (uint32)
    UavRing indirectCounts_; // per view draw count (uint32)

    // Occlusion plan S5b (cascade light-space HZB cull). Per cascade: the casters the main cull
    // deferred (last frame's pyramid hid them), the 8 counters ([c] deferred, [4 + c] drawn by
    // pass B), and pass B's own args + visible list -- cascade rows only, the same per-row layout
    // as the main pair so RecordIndirectShadowDraws draws either.
    UavRing deferredList_;
    UavRing deferredCount_;
    UavRing indirectArgsB_;
    UavRing visibleListB_;
    render::CascadeHzb cascadeHzb_;
    std::shared_ptr<Material> cullPostMat_;      // shadow_cull_post_cs.hlsl

    // Occlusion plan S4 (the camera's indirect G-buffer). Per static group: the MaterialData
    // (textures + surface params) and the indirect PSO of the FIRST object that defined the group;
    // an object whose slots all match is eligible (casterEligible_, and stamped on the object).
    struct GroupGbuf
    {
        MaterialData* md = nullptr;
        Material* mat = nullptr;
    };
    std::vector<GroupGbuf> groupGbuf_;
    std::vector<std::uint8_t> casterEligible_;    // per caster id (static after Rebuild)
    std::vector<std::uint32_t> casterCamCpu_;     // per caster per frame: bit0 eligible, bit1 skip (occluded / chunk masked)
    std::vector<float> casterFadeCpu_;            // per caster per frame: crossfade weight (> 0: two buckets)
    std::vector<std::uint32_t> vgCamCount_;       // per virtual group: camera candidates this frame (fading counted twice)
    Ring casterCam_;                              // per frame uint, region f
    Ring casterFade_;                             // per frame float, region f
    Ring perGroupVgCam_;                          // per frame uint4 {base, indexCount, lodRelStart, count}
    UavRing camArgs_;                             // per virtual group D3D12_DRAW_INDEXED_ARGUMENTS
    UavRing camVisibleList_;                      // 2 x casters (a fading caster occupies two slots)
    // S5: the camera's deferred list (casters last frame's pyramid hid) and pass B's own pair.
    UavRing camDeferred_;
    UavRing camArgsB_;
    UavRing camVisibleListB_;
    CameraHzbParams camHzbParams_{};
    ID3D12Resource* camHzbPrev_ = nullptr;        // the previous frame slot's pyramid (NON_PIXEL at rest)
    ID3D12Resource* camHzbCur_ = nullptr;         // this frame slot's pyramid
    D3D12_CPU_DESCRIPTOR_HANDLE camHzbPrevSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE camHzbCurSrv_{};
    bool camHzbThisFrame_ = false;
    std::shared_ptr<Material> camCullPostMat_;    // cam_cull_post_cs.hlsl
    bool gbufferIndirectFrame_ = false;           // Scene's decision for this frame
    bool rebuildPending_ = false;                 // InvalidateGroupMaterials
    std::uint32_t eligibleCasterCount_ = 0;
    std::uint32_t camListCount_ = 0;              // this frame's camera candidates (sum of vgCamCount_)
    // Validation snapshot of the camera row (see PollValidation).
    std::vector<std::uint32_t> valCam_;
    std::vector<float> valFade_;
    bool valCamOn_ = false;
    std::uint32_t valCamGroups_ = 0;
    bool hzbCullThisFrame_ = false;              // PrepareCullPass's decision, for the S5b builders
    // The cull shaders' b1, filled by PrepareCullPass -- at BUILDER time, before the Main_CsmHzb
    // builder marks this frame's pyramids built. Filling it at record time read the mark of the
    // same frame and never saw a valid previous pyramid (measured: hzbDef 0 on every frame).
    render::CascadeHzb::GpuParams hzbParams_{};
    std::uint32_t hzbPrevValidMask_ = ~0u;       // last logged mask (a state-change event)
    std::uint32_t cullCastersThisFrame_ = 0;     // the visible/deferred lists' per-view stride this frame
    Microsoft::WRL::ComPtr<ID3D12Resource> hzbStatsReadback_; // READBACK, kFrameCount x 8 uints
    std::array<std::uint64_t, render::kFrameCount> hzbStatsFrame_{};
    // [0..4) cascade deferred, [4..8) cascade pass B, [8] camera deferred entries, [9] the same
    // weighted (a fading caster twice), [10] camera pass-B entries, [11] spare.
    std::array<std::uint32_t, 12> hzbStats_{};  // last polled counters
    static constexpr UINT kHzbStatsBytes = 12 * sizeof(std::uint32_t);
    std::array<std::uint32_t, 12> valCounters_{}; // the counters at the validation frame's readback
    bool valCamHzbOn_ = false;                    // S5: the validation frame deferred camera candidates
    void EnsureHzbStatsReadback(Renderer* renderer);
    bool HzbRingsValid() const
    {
        return deferredList_.Valid() && deferredCount_.Valid() && indirectArgsB_.Valid() && visibleListB_.Valid();
    }

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
    // [0..kFrameCount)=args (RAW), [kFrameCount..2k)=visibleList, [2k..3k)=counts,
    // S5b: [3k..4k)=deferredList, [4k..5k)=deferredCount, [5k..6k)=argsB (RAW), [6k..7k)=visibleListB,
    // S4: [7k..8k)=camArgs (RAW), [8k..9k)=camVisibleList,
    // S5: [9k..10k)=camDeferred, [10k..11k)=camArgsB (RAW), [11k..12k)=camVisibleListB.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cullUavHeap_;
    static constexpr std::size_t kCullUavSets = 12;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kCullUavSets * render::kFrameCount> cullUav_{};

    std::shared_ptr<Material> cullClearMat_;     // shadow_cull_clear_cs.hlsl
    std::shared_ptr<Material> cullMat_;          // shadow_cull_cs.hlsl
    std::shared_ptr<Material> indirectShadowMat_; // shadow_indirect_csm.hlsl (Step 5; used in Step 6)
    std::shared_ptr<Material> indirectShadowMaskedMat_; // C2: SHADOW_MASKED=1 variant (alpha-tested groups)
    // VSM single-draw page render: VSM_PAGE=1 permutations (page id unpacked from the caster id,
    // projection from an SRV, page borders via SV_ClipDistance). Optional — a failure just keeps the
    // per-page loop. Used ONLY by VirtualShadowMap::RecordPageRender, never by the Legacy path.
    std::shared_ptr<Material> indirectShadowPageMat_;
    std::shared_ptr<Material> indirectShadowPageMaskedMat_;
    // Pool-format (D32) twins of the plain/masked pair, for the VSM per-page LOOP fallback only.
    std::shared_ptr<Material> indirectShadowPoolMat_;
    std::shared_ptr<Material> indirectShadowPoolMaskedMat_;
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
    // Rebuild's meta, kept so UpdateForFrame can re-publish it with this frame's mover bits ORed in.
    std::vector<std::uint32_t> cpuCasterMeta_;
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
    // tier base LOD (tier / g_shadowLodTierStride) + g_shadowLodBias, UNclamped per mesh (the mega
    // table clamps per mesh). Consumed by the VSM setup CB + the Legacy per-view index-buffer bind.
    std::vector<std::uint32_t> viewLod_;
    // Per (group, lod) mega geometry, flat 4 uints/entry: {megaAbsStart, lodRelStart, indexCount,
    // baseVertex}, pre-clamped to the mesh's available LODs. numMeshGroups_ * kMaxShadowLods entries.
    std::vector<std::uint32_t> groupLodMega_;
    // Chunked-terrain LOD: per-group ABSOLUTE LOD override (-1 = none), refreshed every frame from
    // the chunked objects' camera tiers (see RefreshCasterLods). Consumed ONLY by the setup
    // shader's brute-force fallback since the per-instance table below took over the scatter path.
    std::vector<std::int32_t> groupLodOverride_;
    // Per caster SLOT: the receiver's LOD this frame (bit 7 = chunk EXACT; see LodSelect.h's
    // encoding contract). vsm_page_scatter_cs buckets every instance into a virtual draw group by
    // it — the per-instance caster==receiver contract.
    std::vector<std::uint32_t> casterLod_;
    std::vector<std::uint32_t> casterLodPrev_; // last frame's table (change detection)
    bool casterLodsChanged_ = false;           // see ConsumeCasterLodsChanged
    DirectX::XMFLOAT3 lastCameraPos_{};        // see LastCameraPos
    // GPU homes for the tables above. Mega is static (region 0, written at Rebuild); the override
    // and per-caster tables are per-frame (region f, rewritten by RefreshCasterLods).
    Ring groupLodMegaBuf_;
    Ring groupLodOverrideBuf_;
    Ring casterLodBuf_;
    // mesh -> its FIRST caster group (snapshot of Rebuild's meshToGroup), so the per-frame override
    // refresh can map a chunked mesh's slot ordinal to its group id without re-deriving the layout.
    std::unordered_map<const Mesh*, std::uint32_t> meshFirstGroup_;
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
    bool builtShadowLodBiasNearTier_ = false; // render::g_shadowLodBiasNearTier snapshot
    int builtShadowLodTierStride_ = 1; // normalized render::g_shadowLodTierStride snapshot
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
