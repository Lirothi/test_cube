#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/ResourceDeclarations.h"

class Renderer;
class Material;
class Camera;
class ShadowGpuData;
struct RenderGraphPassContext;
namespace vfx { struct WindState; } // W5: global wind, folded into the per-page shadow view CB

// Rung 2 (VSM-lite) addressing constants. A virtual shadow map is a conceptually huge
// (kVirtualRes²) shadow surface split into kPageSize² pages; only the pages on-screen pixels
// need are made resident in a shared physical page pool. The page table maps a virtual page
// (per view/light-level) to a physical page in the pool.
namespace vsm
{
    inline constexpr std::uint32_t kPageSize = 128;                                  // texels per page edge

    // P16.16 -- the plane transform the receiver-plane depth bias needs, built the way Unreal build
    // `CalcTranslatedWorldToShadowUVNormalMatrix` (VirtualShadowMapArray.cpp:554): take
    // world -> shadow UVZ and return its TRANSPOSE INVERSE, which is how a plane transforms.
    //
    // ONE matrix serves every clipmap level. The shader only uses the ratio
    // `-planeUV.xy / planeUV.z`, and for our levels the UV scale goes as 1/extent while the depth
    // scale goes as 1/(6*extent), so the extent cancels out of the ratio. `ClipmapUvNormalMatrixIsUniform`
    // is the guard on that: it holds only while every level's depth range stays proportional to its
    // extent, which is what `Scene::UpdateClipmap` builds today (depthUp 5E + depthDown 1E).
    inline Math::mat4 CalcClipmapUvNormalMatrix(const Math::mat4& viewProj)
    {
        // Row-vector convention, matching the shaders: [x y z 1] * M. UE's ScaleAndBias is the same
        // half-scale with a flipped V.
        Math::mat4 uvScaleBias = Math::mat4::Identity();
        uvScaleBias.m._11 = 0.5f;
        uvScaleBias.m._22 = -0.5f;
        uvScaleBias.m._41 = 0.5f;  // row-vector translation
        uvScaleBias.m._42 = 0.5f;
        return Math::mat4::Inverse(Math::mat4::Transpose(viewProj * uvScaleBias));
    }

    // Step 19b: LOCAL-light virtual res (spot lights + point-light cube faces). The Step-18/19
    // first cut used kVirtualRes=8192 at a single (finest) level, which over-subscribed the pool
    // ~56× (~57k requested pages vs 1024). 2048² is ~4× the old spot/point maps' texel count and,
    // combined with the mip pyramid below (per-pixel level selection), keeps the request set
    // within the pool. Directional shadows stay on the existing CSM (Pass_CSM) until Step 24 gives
    // them a clipmap with its own resolution.
    inline constexpr std::uint32_t kVirtualRes = 2048;                               // finest-level virtual res per local view
    inline constexpr std::uint32_t kVirtualPagesL0Axis = kVirtualRes / kPageSize;    // 16 (level-0 pages per axis)

    // Mip pyramid: level L has (kVirtualRes>>L)/kPageSize pages per axis, down to 1×1. Each pixel
    // marks exactly ONE page, at the level whose shadow-texel density ≈ its screen-pixel density,
    // so far receivers request coarse (few) pages instead of finest-level ones — the 57k→pool fix.
    inline constexpr std::uint32_t kNumMipLevels = 5;                                // 16,8,4,2,1 for 2048²
    inline constexpr std::uint32_t kMaxMipLevel = kNumMipLevels - 1;                 // 4

    inline constexpr std::uint32_t LevelPagesPerAxis(std::uint32_t level) { return kVirtualPagesL0Axis >> level; }
    inline constexpr std::uint32_t LevelPageCount(std::uint32_t level) { return LevelPagesPerAxis(level) * LevelPagesPerAxis(level); }
    // Prefix sum of level page counts within a view: LevelPageOffset(L) = pages of levels 0..L-1.
    inline constexpr std::uint32_t LevelPageOffset(std::uint32_t level)
    {
        std::uint32_t sum = 0;
        for (std::uint32_t l = 0; l < level; ++l) { sum += LevelPageCount(l); }
        return sum;
    }
    inline constexpr std::uint32_t kPagesPerView = LevelPageOffset(kNumMipLevels);   // 341 = 256+64+16+4+1

    inline constexpr std::uint32_t kPoolTexels = 4096;                               // physical pool edge (~64 MB @ D32)
    inline constexpr std::uint32_t kPoolPagesPerAxis = kPoolTexels / kPageSize;      // 32
    inline constexpr std::uint32_t kPoolPageCount = kPoolPagesPerAxis * kPoolPagesPerAxis; // 1024

    // Single-draw page render: the per-instance visible-list entry carries the physical page index in
    // its high bits so ONE ExecuteIndirect can serve every page (the VS reads that page's projection
    // from pageProj_ instead of a per-page root CBV). Low kPageIdShift bits = caster slot id, high
    // bits = page. MUST match `kPageIdShift` in shadow_indirect_csm.hlsl's VSM_PAGE permutation, and
    // the setup/scatter CBs pass it as gPageIdShift (0 = "don't pack", the per-page loop path).
    inline constexpr std::uint32_t kPageIdShift = 22u;
    static_assert(kPoolPageCount <= (1u << (32u - kPageIdShift)), "page id must fit above kPageIdShift");

    // Step 19b: LOCAL lights only — spots + point-light cube faces, NO CSM cascades (directional
    // stays on Pass_CSM until Step 24). render::kMaxShadowViews (36) = 4 cascades + this set, so
    // subtract the cascades. Layout: [spots (kMaxShadowedSpotLights) | point faces
    // (kMaxShadowedPointLights*6)] = 8 + 24 = 32.
    inline constexpr std::uint32_t kNumCascades = 4;                                 // == SceneFrameData::kCascades
    inline constexpr std::uint32_t kNumLocalVirtualViews = 32;                        // spots (8) + point faces (24)

    // Step 24d/24e: directional clipmap — N nested camera-centered ortho levels, appended as VSM
    // views AFTER the 32 local ones (each a full view of kPagesPerView pages). Step 24e adds them to
    // the Rung-0 cull too, so the cull layout is [4 cascade | 32 local | N clipmap] and the setup's
    // rung0View = view + kNumCascades maps a clipmap VSM view straight to its cull slot. Hence
    // render::kMaxShadowViews == kNumCascades + kMaxVirtualViews. VSM_MAX_VIEWS / kMaxViews in the
    // shaders must equal kMaxVirtualViews.
    inline constexpr std::uint32_t kNumClipmapLevels = 8;
    inline constexpr std::uint32_t kMaxVirtualViews = kNumLocalVirtualViews + kNumClipmapLevels; // 40
    static_assert(kNumCascades + kMaxVirtualViews == render::kMaxShadowViews,
                  "cull view layout must be [cascades | local | clipmap]");
    inline constexpr std::uint32_t kPageTableEntries = kPagesPerView * kMaxVirtualViews; // 341*40 = 13640

    // Caster MESH-GROUPS the GPU-driven shadow path can address at once, across the whole level: one
    // per (mesh, submesh), so submesh splits — material slots, and terrain chunking's spatial tiles —
    // all spend from this one budget.
    //
    // NARROWED TWICE on 2026-08-24, and it now caps almost nothing. First the per-(group,lod) and
    // per-group-override tables left the setup CB for SRVs sized by the real group count (and the
    // mega gate went with them), so DIRECTIONAL shadows lost their limit. Then S5 put LOCAL views
    // into the spatial scatter too, so their pages stopped using the brute-force cull whose two
    // per-thread tables are the last thing sized by this number.
    //
    // What it still bounds: (a) the brute-force FALLBACK, reached only if the scatter PSO fails to
    // build — a group past it casts no local-light shadow there, deterministically; (b) the GI-fold
    // policy cap. Neither is a limit on ordinary rendering.
    //
    // Consumers: ShadowGpuData::Rebuild's GI-fold cap, the editor's mesh import dialog (which quotes
    // the budget while the user picks a chunk grid), and VSM_MAX_SETUP_GROUPS in
    // vsm_page_setup_cs.hlsl — the one copy that cannot include this header, so keep it in step by
    // hand. Removing the local-light cap too is S5 in docs/vsm_group_cap_removal_plan.md.
    inline constexpr std::uint32_t kMaxMeshGroups = 64;

    // Page-table entry packing (a single uint per virtual page). Unused until Step 20 fills it.
    //   bit 31      : resident (1 = mapped to a physical page)
    //   bits 0..15  : physical page index (0..kPoolPageCount-1)
    inline constexpr std::uint32_t kPageResidentBit = 0x80000000u;
    inline constexpr std::uint32_t kPagePhysicalMask = 0x0000FFFFu;

    // Step 19: the page-request set is a bitfield (1 bit per virtual page).
    inline constexpr std::uint32_t kRequestWords = (kPageTableEntries + 31u) / 32u; // 341 uints

    // Step 19b perf: run the request pass at 1/kRequestDownscale per axis. Lower = denser screen
    // sampling = better page coverage (fewer "missed" pages that show as a resident/lit
    // checkerboard when the sampler needs a page the sub-sampled request never marked).
    inline constexpr std::uint32_t kRequestDownscale = 4;

    // Step 19b level selection: level = clamp(log2(distCamera / kLodRefDist), 0, kMaxMipLevel).
    // Receivers within ~kLodRefDist of the camera use the finest level; each doubling of distance
    // steps one level coarser. SMALLER = coarser overall = FEWER resident pages (the old spot atlas
    // was 512² ≈ level 2; kVirtualRes=2048 level 0 is 16× finer per axis — wildly over-fine near,
    // which both inflates the per-page render CPU cost and causes the sub-sample checkerboard).
    inline constexpr float kLodRefDist = 5.0f;

    // Step 20 allocation: free a resident physical page that has not been requested for this many
    // frames (LRU). Small enough that pages the camera moved off release promptly; large enough
    // not to thrash pages that flicker in/out of the sub-sampled request set.
    inline constexpr std::uint32_t kLruFrameThreshold = 16;

    // --- Runtime-tunable mirrors (dev-window "VSM" tab). The passes read THESE each frame, so a
    // change applies live. They only feed per-frame CB values / the request dispatch size — NOT
    // buffer sizing (kVirtualRes / kNumMipLevels / kPoolPageCount stay compile-time), so changing
    // them needs no reallocation. Defaults = the constexpr values above. ---
    inline float         g_refDist = kLodRefDist;
    inline std::uint32_t g_requestDownscale = kRequestDownscale;
    // Step 24d: finest directional-clipmap level's world extent (level i covers g_clipmapBaseExtent
    // * 2^i). Tunable for the 24f visual sign-off; only feeds the per-frame view build (no realloc).
    inline float         g_clipmapBaseExtent = 12.0f;
    // Fraction of a directional clipmap level's half-extent used to cross-fade its shadow factor
    // into the next coarser level. 0 is a strict kill switch: no parent page requests and no second
    // PCF sample. 0.12 blends the outer 12% of each nested square and hides caster-LOD silhouette
    // changes without coarsening the finest level near the camera.
    inline bool          g_clipmapBlendEnabled = true;
    inline float         g_clipmapBlendWidth = 0.12f;
    inline float ClipmapBlendWidth()
    {
        return g_clipmapBlendEnabled ? std::clamp(g_clipmapBlendWidth, 0.0f, 0.5f) : 0.0f;
    }

    // Step 24f: directional-clipmap NDC depth bias (against shadow acne). Tunable at the visual gate.
    // A level's NDC range is 6x its extent, so a constant NDC value is a constant bias in TEXELS
    // (ndc * 6 * 2048 -- 0.0001 = 1.23 texels) whose WORLD size doubles per level.
    // DEFAULT 0 (2026-08-21): the pool is D32_FLOAT now, so there is no quantization floor to
    // cover -- the per-tap receiver-plane bias plus the normal offset carry everything, which is
    // UE's configuration (they run no constant either). Verified at the 2.8-degree-sun banding
    // camera and the palm-contact camera; raise only if a new scene shows residual acne.
    inline float         g_clipmapDepthBias = 0.0001f;
    // Per-level shaping of the constant bias: bias(L) = max(g_clipmapDepthBias * decay^L, floor).
    // decay 1 = constant-in-texels (world size doubles per level -- raising the base then detaches
    // thin far shadows); 0.5 = a constant WORLD-size bias (the near value everywhere). The floor is
    // authored in TEXELS of the landed level. Both are inert at the 0-bias default; they shape the
    // constant if one is ever brought back.
    inline float         g_clipmapDepthBiasDecay = 1.0f;
    inline float         g_clipmapDepthBiasFloorTexels = 0.0f;
    // Step 24f / P16.16: directional-clipmap normal offset. UNITS ARE UNREAL'S, so the number here
    // is directly comparable to `r.Shadow.Virtual.NormalBias` (their default 0.5): the shader
    // divides by 1000 and multiplies by distance-to-camera * tan(hFov/2), i.e. it is referred to the
    // world size of a SCREEN PIXEL, not to the shadow texel. The old form was "texels * dist/1024",
    // which at its shipped 2.0 worked out 3.9x this and ignored the field of view entirely.
    inline float         g_clipmapNormalBias = 1.0f;
    // Local-light (spot + point) VSM shadow bias, in units of one shadow texel at the receiver
    // (auto-sized per-pixel from the light's cone width, distance and mip level in the shaders).
    // Lateral = surface-normal offset (~1 texel keeps Peter-panning to a texel); depth push =
    // along-the-light-ray, slope-scaled, so grazing acne clears without moving flat lit ground.
    // Live-tunable in the Developer window's VSM section (fed to the spot/point CBs each frame).
    inline float         g_localLateralTexels   = 1.0f;
    inline float         g_localDepthPushTexels = 0.5f;
    inline std::uint32_t g_lruThreshold = kLruFrameThreshold;

    // Render only the pages a kFrameCount-old physOwner snapshot says are resident (skips the ~free
    // pool pages → far fewer CPU draw calls). DEFAULT ON: the CPU saving is worth the artifact here.
    // That artifact is the snapshot latency — a page that just became resident is skipped for
    // ~kFrameCount frames and, with the pool cleared each frame, reads as UNSHADOWED, so shadows
    // blink at the edges whenever the resident set changes (camera motion / light churn). OFF =
    // iterate the whole pool every frame (correct, ~4x the render CPU, all of it on a graph worker).
    // g_pageDrawSingle below removes the same artifact WITHOUT paying that CPU: one draw over every
    // page has nothing to skip, so this flag simply stops applying (it still governs the loop path).
    inline bool g_residentIterOnly = true;

    // Single-draw page render: ONE ExecuteIndirect over all (page, group) args instead of the
    // kPoolPageCount-iteration CPU loop (per-page viewport -> VS clip-space remap + SV_ClipDistance
    // page borders). Requires the mega buffer (geometry bound once). Default ON; OFF restores the
    // per-page loop for A/B and for per-page inspection in PIX.
    inline bool g_pageDrawSingle = true;

    // Compacted draw args (single-draw only): the setup CS appends ONLY non-empty (page, group)
    // records via an atomic and the draw reads that counter as ExecuteIndirect's count buffer,
    // instead of walking all kPoolPageCount*groups fixed-layout records. Requires the page id in the
    // instance stream (record order stops mattering), which is why it cannot precede the flip.
    // DEFAULT OFF — measured a small net LOSS on this scene (2026-08-01, interleaved A/B/A/B on
    // matched clocks, ~20.4k frames each): Pass_VsmPageRender GPU 0.401/0.412 ms without vs
    // 0.425/0.422 with, i.e. ~+0.017 ms (~4% of the pass); CPU identical (0.067 both). The reason is
    // that the thing it removes was already free — walking 6144 zero-instance records costs nothing
    // (see g_residentIterOnly's ~2% measurement) — while the atomic it adds lands in the setup CS,
    // the pass's most expensive sub-scope. Kept because the record count is pages x mesh-groups: a
    // group-heavy scene (64 groups = 65k records) is where the walk could start to matter. Re-measure
    // there before turning it on; do not enable it on faith.
    //
    // RE-MEASURED 2026-08-24 at 66 GROUPS (wind_test, island chunked 7x7 = 67.6k records) — the
    // scene the note above asked for, and the verdict INVERTS: `Pass_VsmPageRender` 0.794/0.809 OFF
    // vs 0.757/0.756 ON (-0.046 ms), `VsmPageRender.Setup` 0.050 -> 0.022 (-56%), `Pass_Compose`
    // flat at 0.029. Interleaved x2 on matched clocks.
    //
    // Why it flipped, and it is not just the walk: the `continue` for an empty group now skips the
    // group's GroupLodOverride + GroupLodMega SRV loads too (they moved out of the CB when the group
    // cap was removed). So compaction skips the loop BODY, not just a 20-byte store, and most of
    // what it recovers is that transport cost.
    //
    // DEFAULT FLIPPED TO ON 2026-08-24, after the light-scene re-measure the note above demanded.
    // demo.json (6 groups, 9 spots + 8 points): 0.367/0.360 OFF vs 0.360/0.358 ON — the 2026-08-01
    // loss is GONE. It went away because the setup CS now splits its per-group loop across the 8
    // thread lanes it used to throw away, so the atomic this adds no longer lands in a serial loop.
    // Neutral on a light scene, -0.057 ms on a group-heavy one: ON is the right default at both ends.
    // Boot flags: --vsm-page-nocompact forces it off (the A/B control), --vsm-page-compact on.
    inline bool g_pageDrawCompact = true;

    // S5: scatter LOCAL (spot/point) views too, instead of leaving them to the setup CS's
    // brute-force per-page cull. ON removes the last mesh-group cap (that cull's two per-thread
    // tables are the only thing still sized by kMaxMeshGroups) and makes the per-page cost stop
    // scaling with the caster count. OFF restores the brute-force path.
    //
    // Kept as a live toggle because the two paths must be A/B-able IN ONE BINARY: local-light
    // shadow correctness is judged by comparing them, and demo.json — the only level with local
    // lights — has a screenshot noise floor of |mean| 1.07, so a cross-build comparison there
    // cannot resolve a real difference from run-to-run drift.
    inline bool g_scatterLocalViews = true;

    // Page cache (Rung 1): skip re-rendering pages whose content didn't change (cached depth kept;
    // only new / dynamic-caster-overlapping / forced pages re-render). DEFAULT OFF: measured a net
    // loss on the current scene — VsmPageRender is dominated by the per-page CULL (which caching
    // can't skip), the teapots are dynamic (uncacheable), and the gated depth-clear is slower than a
    // hardware full-pool clear on spike frames (max regressed). ON draws only dirty pages + uses the
    // gated clear; keep for a future coarse-skip (skip the cull for pages far from any mover).
    inline bool g_pageCaching = false;

    // Periodically log the VSM page stats to DBWIN ("[VSM] request ... | resident=..."). DEFAULT OFF
    // — it spams a captured stress/dev run. The on-screen dev-window "VSM" tab always shows the live
    // stats; flip this on only when you want them in the debug output too.
    inline bool g_logPageStats = false;

    // Per-shadow-view data the request pass projects screen pixels through (mirrors the HLSL
    // cbuffer in vsm_page_request_cs.hlsl). params.x = valid (0/1), .y = zNear, .z = zFar.
    struct alignas(16) ViewProjEntry
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4   params;
    };
    struct alignas(16) PageRequestConstants
    {
        DirectX::XMFLOAT4X4 invView;
        DirectX::XMFLOAT4X4 invProj;
        DirectX::XMFLOAT4   camPosWS;   // xyz
        DirectX::XMFLOAT4   screen;     // x=w, y=h, z=1/w, w=1/h
        DirectX::XMFLOAT4   lodParams;  // x=refDist, y=maxLevel, z=downscale, w=clipmap blend width
        std::uint32_t       numViews;
        std::uint32_t       _pad[3];
        ViewProjEntry       views[kMaxVirtualViews];
    };
}

// Rung 2 / Step 18: owns the PERSISTENT (cross-frame, NOT per-frame-tripled — the pool IS the
// cache) physical page pool + page table. Allocated once; still unused (no pass reads/writes
// them until Steps 19-22). Later steps add the screen-space page-request pass, allocation,
// virtual->physical sampling, and per-page caster rendering (reusing Rung 0's cull + indirect).
class VirtualShadowMap
{
public:
    // Allocate the pool + page table once (idempotent; persistent across level switches).
    void EnsureResources(Renderer* renderer);

    // Barrier plan step 4: create everything the Record* functions below used to create
    // lazily, at a point where the GPU is not mid-frame and nothing is recording. Call
    // once per frame BEFORE the render graph runs.
    //
    // Two reasons, in order of importance: a pass's Prepare callback can only register a
    // resource that already exists, and a lazy grow inside a recording body is what freed
    // the spot/point light buffers mid-render and produced the DXGI_DEVICE_HUNG that the
    // --scene-stress harness was built to catch. Idempotent and cheap when nothing changed.
    void EnsureFrameResources(Renderer* renderer, ShadowGpuData* shadowGpu);

    // Barrier plan step 5: VSM's own contribution to the page-request pass's Prepare. The
    // resources the Record* functions transition are private to VSM, so VSM registers them
    // rather than exposing every buffer just so SceneRenderer can list them. The context type
    // is forward-declared — the definition lives in the .cpp, which keeps RenderGraph.h out of
    // this header (it already includes Renderer.h, so including it here would be circular).
    //
    // pass-flow S3c: the request/alloc pass's barrier-point indices + the one-shot readback
    // decision, captured by PrepareRequestPass as it declares and carried into the record lambda
    // by the AddPass2 builder — the record bodies emit EmitPoint markers and name no states.
    struct PageRequestPoints {
        std::uint32_t base = 0;            // camera depth SRV + request buffer UAV
        std::uint32_t alloc = 0;           // the six alloc buffers -> UAV
        bool readback = false;             // this frame runs the one-shot debug readback
        std::uint32_t readbackCopy = 0;    // request/counters/physOwner -> COPY_SOURCE
        std::uint32_t readbackRestore = 0; // ...and back to canonical after the copies
    };
    // Declares this frame's exact uses and returns the point indices. The caller (the AddPass2
    // builder) gates on VsmActive/IsAllocated BEFORE declaring anything, including the depth
    // read it registers itself.
    PageRequestPoints PrepareRequestPass(RenderGraphPassContext& ctx) const;
    // Barrier plan D1.1 → pass-flow S3: the page-render path's runtime decisions, taken ONCE per
    // frame by PrepareRenderPass and RETURNED to the caller — SceneRenderer's AddPass2 builder
    // captures them into the record lambda, so Prepare and Record read the SAME values by
    // construction (they used to travel through a class member, which is a bridge two code paths
    // can still disagree over; a by-value capture cannot).
    struct PageRenderDecisions {
        bool caching = false;
        bool useMega = false;
        bool singleDraw = false;
        bool compactArgs = false;
        bool scatterActive = false;
        // S5: local views are in the scatter this frame (scatterActive AND g_scatterLocalViews).
        bool scatterLocals = false;
        std::uint32_t forceAll = 1u;
        // pass-flow S3b: the ABSOLUTE declaration indices of the pass's barrier points, captured
        // by PrepareRenderPass as it declares them. RecordPageRender emits each with an EmitPoint
        // marker — the record body names no resource and no state. Scatter/cache indices are only
        // meaningful when their decision above is true.
        std::uint32_t pointBase = 0;
        std::uint32_t pointScatterWrite = 0;
        std::uint32_t pointScatterRead = 0;
        std::uint32_t pointCacheCopy = 0; // physOwnerPrev -> COPY_DEST, BEFORE the snapshot copy
        std::uint32_t pointCacheRead = 0; // physOwnerPrev/perPageDirty -> NPS, AFTER the copy
        std::uint32_t pointConsume = 0;
    };

    // Registers this frame's exact resource uses AND returns the decisions they were derived
    // from. Call only on frames the pass will record (the AddPass2 builder gates both at once).
    PageRenderDecisions PrepareRenderPass(RenderGraphPassContext& ctx, ShadowGpuData* shadowGpu,
                                          const vfx::WindState* wind);
    // Step 24b: free every VSM GPU allocation (Legacy mode / teardown). MUST be at GPU idle.
    void ReleaseResources();
    bool IsAllocated() const { return pagePool_ != nullptr && pageTable_ != nullptr; }

    // Resources + views for the future VSM passes; null/{0} until allocated.
    ID3D12Resource* PagePool() const { return pagePool_.Get(); }
    ID3D12Resource* PageTable() const { return pageTable_.Get(); }
    // Level switch: drop every page mapping — the next RecordPageAllocate re-runs the one-shot
    // init (page table zeroed, all physical pages freed). Cross-level pool content is garbage:
    // the new level's views reuse the same view slots, so its virtual page ids collide with
    // resident entries whose depths were rendered under the OLD level's viewProj/casters.
    void InvalidateAllPages() { allocInitialized_ = false; }

    D3D12_CPU_DESCRIPTOR_HANDLE PagePoolDsv() const { return poolDsv_; }     // render pages (Step 22)
    D3D12_CPU_DESCRIPTOR_HANDLE PagePoolSrv() const { return poolSrv_; }     // sample (Step 21)
    D3D12_CPU_DESCRIPTOR_HANDLE PageTableSrv() const { return pageTableSrv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE PageTableUav() const { return pageTableUav_; } // written by Step 20

    // Step 19: run the screen-space page-request pass into `cl` (an open command list). Clears
    // the request bitfield, then for each visible pixel projects into every active shadow view
    // and marks the virtual page it needs. `constants` carries the camera + per-view viewProj
    // (filled by the caller from the frame's shadow views); `depthSrv` is the camera depth.
    // Produces the request set; NOTHING consumes it yet (Step 20 allocates from it).
    void RecordPageRequest(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                           const vsm::PageRequestConstants& constants,
                           D3D12_CPU_DESCRIPTOR_HANDLE depthSrv, UINT screenW, UINT screenH);

    // Step 20: consume the request bitfield produced by RecordPageRequest (same command list,
    // right after it) and turn it into physical-page assignments with cross-frame caching: touch
    // resident+requested pages (LRU keep-alive), free LRU-stale pages back to a free list, then
    // allocate a physical page for each requested-but-not-resident page and append it to the
    // needs-render list (Step 22 input). Persists the page table + free-list/LRU state across
    // frames. Nothing samples/renders the pages yet (add-dormant). Also records the debug readback.
    void RecordPageAllocate(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                            const PageRequestPoints& pts);

    // Step 22: render shadow casters into the resident physical pages. A setup compute builds a
    // per-page off-center projection + per-page indirect args (copied from Rung 0's per-view cull),
    // then the whole pool is cleared and each pool page is drawn via ExecuteIndirect (reusing
    // ShadowGpuData's instance buffer + visible list + indirect depth VS) with its page-cell
    // viewport. `views` are the VSM local shadow views (spots then point faces) in slot order.
    // Same-frame, race-free (no readback). Correctness-first: re-renders every resident page each
    // frame (Step 23 will gate to changed pages only).
    // W5: `wind` (may be null) is copied into every page's PerView slot so the shadow VS sways
    // foliage casters exactly like the gbuffer does.
    void RecordPageRender(Renderer* renderer, ID3D12GraphicsCommandList* cl, ShadowGpuData* shadowGpu,
                          const vsm::ViewProjEntry* views, std::uint32_t viewCount,
                          const vfx::WindState* wind, const PageRenderDecisions& dec);

    // Step 19/20 (temporary): a few frames in, read back the request bitfield + allocation
    // counters and log the requested-page mip histogram + resident/newly-allocated/failed counts,
    // so caching is verifiable (resident stabilizes, newly-allocated ~0 when the scene is stable).
    // Re-armed periodically. Call once per frame (main thread).
    void PollPageRequestDebug(Renderer* renderer);

    // --- Live debug stats for the dev-window "VSM" tab. Filled by PollPageRequestDebug from the
    // deferred readback (a kFrameCount-old snapshot, refreshed a few times/sec). They reflect the
    // last ACTIVE frame — values freeze while VSM is off or the camera is still (request/alloc/render
    // are skipped then), which is expected. ---
    struct DebugStats
    {
        bool          valid = false;                     // a sample has landed at least once
        std::uint64_t sampleFrame = 0;                   // total-frame the sampled data was produced
        std::uint32_t requested = 0;                     // total requested virtual pages (this sample)
        std::uint32_t resident = 0;                      // physical pages currently mapped (of kPoolPageCount)
        std::uint32_t newAlloc = 0;                      // pages newly allocated on the sampled frame
        std::uint32_t fail = 0;                          // requests that couldn't allocate (pool full)
        std::uint32_t perLevel[vsm::kNumMipLevels] = {}; // requested pages per mip level (LOD histogram)
    };
    const DebugStats& Stats() const { return stats_; }

    // Physical-page ownership snapshot for the dev-window pool grid: kPoolPageCount entries, each
    // 0xFFFFFFFF (free) or the owning virtual-page id (view = id / vsm::kPagesPerView). Same
    // kFrameCount-old snapshot as Stats(); empty until the first sample.
    const std::vector<std::uint32_t>& PhysOwnerSnapshot() const { return physOwnerSnapshot_; }

private:
    GpuResource pagePool_;   // R32_TYPELESS depth atlas (kPoolTexels²) — D32, see EnsureResources
    GpuResource pageTable_;  // StructuredBuffer<uint>[kPageTableEntries]
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;    // pool DSV
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvUavHeap_; // pool SRV + page-table SRV/UAV

    D3D12_CPU_DESCRIPTOR_HANDLE poolDsv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE poolSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageTableSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageTableUav_{};

    // Step 19: page-request bitfield (1 bit / virtual page) + its UAV. DEFAULT-heap; written +
    // consumed in-frame (single buffer, cleared each frame).
    GpuResource requestBuffer_;
    D3D12_CPU_DESCRIPTOR_HANDLE           requestUav_{};

    // Step 20: persistent (cross-frame — this IS the cache) page-allocation state. All DEFAULT-heap
    // RWStructuredBuffer<uint>, kept in UNORDERED_ACCESS. physOwner/physLastFrame/free-list/LRU
    // survive level switches with the pool + page table.
    GpuResource physOwner_;     // [kPoolPageCount] physical -> virtual owner / INVALID
    GpuResource physLastFrame_; // [kPoolPageCount] physical -> last requested frame
    GpuResource freeList_;      // [kPoolPageCount] free physical indices (rebuilt per frame)
    GpuResource needsRender_;   // [kPoolPageCount] pages allocated this frame (Step 22 input)
    GpuResource allocCounters_; // [4] free / needs / fail / resident
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> allocUavHeap_; // the 5 alloc-state UAVs
    D3D12_CPU_DESCRIPTOR_HANDLE physOwnerUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE physLastFrameUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE freeListUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE needsRenderUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE allocCountersUav_{};
    bool allocInitialized_ = false; // one-shot page-table/owner init done

    void EnsureShaderResources(Renderer* renderer); // lazily create all VSM compute PSOs
    std::shared_ptr<Material> pageRequestClearMat_;  // vsm_page_request_clear_cs.hlsl
    std::shared_ptr<Material> pageRequestMat_;       // vsm_page_request_cs.hlsl
    std::shared_ptr<Material> allocInitMat_;         // vsm_page_alloc_init_cs.hlsl
    std::shared_ptr<Material> allocTouchMat_;        // vsm_page_alloc_touch_cs.hlsl
    std::shared_ptr<Material> allocFreeMat_;         // vsm_page_alloc_freelist_cs.hlsl
    std::shared_ptr<Material> allocMapMat_;          // vsm_page_alloc_map_cs.hlsl
    std::shared_ptr<Material> pageSetupMat_;         // vsm_page_setup_cs.hlsl (Step 22)
    std::shared_ptr<Material> pageScatterMat_;       // vsm_page_scatter_cs.hlsl (spatial scatter cull)
    std::shared_ptr<Material> pageScatterClearMat_;  // vsm_page_scatter_clear_cs.hlsl (zero the counts)
    std::shared_ptr<Material> pageClearMat_;         // vsm_page_clear.hlsl (page cache: gated depth clear)
    bool shaderResourcesTried_ = false;

    // Step 22: per-page render state. pageProj_ = off-center viewProj per physical page (256-byte
    // stride for root-CBV alignment); pageDrawArgs_ = per (page, mesh-group) indirect args copied
    // from Rung 0's per-view cull. DEFAULT-heap, persistent.
    GpuResource pageProj_;
    GpuResource pageDrawArgs_;
    // Per-page-cull plan (Step 1): per (page, caster-slot) visible-list. The setup shader culls the
    // caster set to each page's frustum and writes that page's compacted, mesh-group-grouped caster
    // ids into its [p*casters, (p+1)*casters) slice; the page draw binds this as the slot-1 stream.
    GpuResource pageVisibleList_;
    // Page-caching plan (Rung 1): physOwnerPrev_ = last frame's physOwner (new-page detection);
    // perPageDirty_ = per-page dirty bit the setup computes + the gated depth-clear reads.
    GpuResource physOwnerPrev_;
    GpuResource perPageDirty_;
    // Spatial scatter cull (directional clipmap only): instead of every page testing every caster
    // (O(pages x casters)), each caster projects its AABB into each clipmap level's page grid and
    // writes itself into the few pages it actually covers. pageGroupCount_ = per (page, mesh-group)
    // instance count, doubling as the append cursor (a caster's rank within its group's run);
    // pageScatterDyn_ = per-page "a dynamic caster landed here" flag for the page cache.
    GpuResource pageGroupCount_;
    GpuResource pageScatterDyn_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> renderHeap_; // physOwnerSrv, rung0ArgsSrv, pageDrawArgsUav, pageProjUav, pageVisibleListUav, physOwnerPrevSrv, perPageDirtyUav, perPageDirtySrv
    D3D12_CPU_DESCRIPTOR_HANDLE physOwnerSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE rung0ArgsSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageDrawArgsUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageProjUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageVisibleListUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE physOwnerPrevSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE perPageDirtyUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE perPageDirtySrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageGroupCountUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageGroupCountSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageScatterDynUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageScatterDynSrv_{};
    // Single-draw page render: pageProj_ as a StructuredBuffer<float4> for the VSM_PAGE vertex
    // shader (16 elements per page's 256-byte slot). The loop path binds the same buffer as a
    // per-page root CBV instead, which is why this is an SRV and not a replacement.
    D3D12_CPU_DESCRIPTOR_HANDLE pageProjSrv_{};
    // Compacted draw args: single uint the setup CS atomically bumps per appended (page, group)
    // record; consumed as ExecuteIndirect's count buffer. 4 uints so the allocation is comfortably
    // aligned — only element 0 is used.
    GpuResource pageArgCount_;
    D3D12_CPU_DESCRIPTOR_HANDLE pageArgCountUav_{};
    std::uint32_t   renderGroups_ = 0;           // mesh-group count pageDrawArgs_ is sized for
    std::uint32_t   scatterGroups_ = 0;          // mesh-group count pageGroupCount_ is sized for
    std::uint32_t   renderCasters_ = 0;          // caster count pageVisibleList_ is sized for
    bool            cacheWarmup_ = true;         // force one full render until physOwnerPrev_ is valid (else garbage)
    // Single-draw page render: last logged reason the path was unavailable while g_pageDrawSingle
    // was on (0 = available / nothing logged). Dedupes the DBWIN line to one per distinct cause —
    // a silent fallback to the per-page loop is the expensive thing to diagnose later.
    int             singleDrawFallbackLogged_ = 0;
    ID3D12Resource* cachedRung0Args_ = nullptr;  // detect a ShadowGpuData rebuild (re-create the SRV)
    void EnsureRenderResources(Renderer* renderer, ShadowGpuData* shadowGpu);

    // Step 22 CPU: the render loop only draws RESIDENT pool pages, not all kPoolPageCount. A per-
    // frame readback ring of physOwner (kFrameCount-latency) tells the CPU which physical pages are
    // resident so it skips the ~free ones (which otherwise cost ~15k no-op draw bindings/frame). The
    // page content (pageProj/args) is always current, so the only effect of the latency is a
    // newly-resident page showing unshadowed for a few frames (edge pop-in while moving).
    Microsoft::WRL::ComPtr<ID3D12Resource> residentReadback_[render::kFrameCount];
    const std::uint32_t* residentReadbackPtr_[render::kFrameCount] = {};
    bool residentReadbackValid_[render::kFrameCount] = {};

    // Step 19/20 validation: deferred readback of the request bitfield + alloc counters, re-armed
    // periodically so a live/stress run samples several times. The single readback buffer holds
    // [request words | alloc counters].
    static constexpr std::uint64_t kRequestReadbackPeriod = 30; // frames between samples (dev-window live-ish)
    static constexpr std::uint64_t kDbwinLogPeriod = 180;       // throttle the OutputDebugStringA log separately
    Microsoft::WRL::ComPtr<ID3D12Resource> debugReadback_;
    std::uint64_t debugReadbackFrame_ = 0;     // frame the copy was scheduled
    std::uint64_t debugReadbackDoneFrame_ = 0; // frame the last sample logged
    std::uint64_t debugLoggedFrame_ = 0;       // frame the last DBWIN line was emitted (log throttle)
    int           debugReadbackState_ = 0;     // 0 = not started, 1 = pending, 2 = done

    // Dev-window readout: filled from the readback each sample (see PollPageRequestDebug).
    DebugStats                 stats_;
    std::vector<std::uint32_t> physOwnerSnapshot_; // kPoolPageCount, physical -> owning virtual page / INVALID

    void RecordDebugReadback(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                             const PageRequestPoints& pts); // Step 20: copy request+counters
    // Step 7: the gate RecordDebugReadback uses, shared with PrepareRequestPass so the two cannot
    // drift — a Prepare that registers what its Record skips is fatal once barriers are compiled.
    bool WillRecordDebugReadback(Renderer* renderer) const;
    // D1.1 → pass-flow S3: pure — returns the frame's decisions, stores nothing.
    PageRenderDecisions ComputePageRenderDecisions(ShadowGpuData* shadowGpu,
                                                   const vfx::WindState* wind) const;
    void EnsureAllocResources(Renderer* renderer); // create the alloc buffers + descriptors
};
