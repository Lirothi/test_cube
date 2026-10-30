#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/ResourceDeclarations.h"

#include "rendering/shadows/ShadowSettings.h"
class Renderer;
class Material;
class Camera;
class ShadowGpuData;
struct RenderGraphPassContext;
struct RenderContext;
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
    // stays on Pass_CSM until Step 24): the cull layout (render::MaxShadowViews) minus its 4 cascades.
    // Local-light plan S10a: ONE source -- the cull layout's counts (RenderConstants.h); the shaders get the same numbers
    // as defines at compile time (Material.cpp EngineShaderDefines: TC_SHADOW_CASCADE_VIEWS, TC_VSM_PREFIX_SPOTS,
    // TC_VSM_CLIPMAP_FIRST, TC_VSM_CLIPMAP_LEVELS, TC_SHADOW_VIEW_CEILING), never as literals of their own.
    inline constexpr std::uint32_t kNumCascades = render::kShadowViewCascades;      // == SceneFrameData::kCascades
    // Plan S10b: VSM's views ARE the cull layout minus its cascades -- [8 spots | 24 point faces | clipmap | the other
    // spots | the other faces] -- so every VSM view's cull slot is view + kNumCascades (the setup's rung0View) and VSM
    // shadows every spot / point the atlas modes do. The view of a light is SpotView / PointFaceView, never a count.
    inline constexpr std::uint32_t kClipmapFirstView = render::kShadowViewVsmSpots + 6u * render::kShadowViewVsmPoints; // 32
    // Plan S9e: as many local views as the shadow capacity holds (render::ShadowSpotCapacity / ShadowPointCapacity, which
    // grow at Scene::ReconcileShadowMode's GPU-idle point and re-create VSM with them) -- 128 at the initial 32 / 16.
    inline std::uint32_t MaxLocalVirtualViews() { return render::ShadowSpotCapacity() + 6u * render::ShadowPointCapacity(); }

    // Step 24d/24e: directional clipmap — N nested camera-centered ortho levels, appended as VSM
    // views AFTER the 32 local ones (each a full view of kPagesPerView pages). Step 24e adds them to
    // the Rung-0 cull too, so the cull layout is [4 cascade | 32 local | N clipmap] and the setup's
    // rung0View = view + kNumCascades maps a clipmap VSM view straight to its cull slot. Hence
    // render::MaxShadowViews() == kNumCascades + MaxVirtualViews().
    // 10 levels (was 8, 2026-08-28): the coarsest level's half-extent is g_clipmapBaseExtent*2^9
    // = ~6.1 km at the default 12 m base, so directional shadows reach 4x farther for +682 page
    // table entries and two more cull/scatter views; the physical pool is untouched (far levels
    // are coarse and resident pages stay demand-driven).
    inline constexpr std::uint32_t kNumClipmapLevels = render::kShadowViewClipmapLevels; // 10

    // The square each clipmap level covers in the FIXED light frame Scene::UpdateClipmap snaps
    // every level in (its `right` / `trueUp` axes are shared by all levels, which is the whole
    // reason a level-to-level page mapping is 2D arithmetic instead of a matrix inverse).
    // Consumed by the LOD fallback chain, shaders/vsm_page_propagate_cs.hlsl -- MIRRORS its
    // `gLevelXform`, so the layout is float4 per level.
    struct ClipmapLevelSquare
    {
        float centreX = 0.0f;   // world centre projected on the light frame's right axis
        float centreY = 0.0f;   // ... and on its up axis
        float extent  = 0.0f;   // full edge length; 0 marks a level with no valid sun
        float _pad    = 0.0f;
    };
    struct ClipmapSquares
    {
        std::uint32_t count = 0;
        std::array<ClipmapLevelSquare, kNumClipmapLevels> level{};
    };
    // Plan S9e: every VSM view at the current capacity (138 at 32 / 16); VirtualShadowMap sizes its page table, request
    // bits and per-view records by it at EnsureResources and keeps that count (ViewCount) until it is re-created.
    inline std::uint32_t MaxVirtualViews() { return MaxLocalVirtualViews() + kNumClipmapLevels; }
    // The literal every shader loop over views is bounded by (VSM_VIEW_CEILING, vsm_addressing.hlsli).
    inline constexpr std::uint32_t kVirtualViewCeiling = render::kShadowViewCeiling - kNumCascades;
    // Plan S10b: the views of a spot slot, a point face (slot * 6 + face), a clipmap level; the i-th local view.
    inline std::uint32_t SpotView(std::uint32_t slot) { return render::ShadowViewSpotSlot(slot) - kNumCascades; }
    inline std::uint32_t PointFaceView(std::uint32_t face) { return render::ShadowViewPointFaceSlot(face) - kNumCascades; }
    constexpr std::uint32_t ClipmapView(std::uint32_t level) { return kClipmapFirstView + level; }
    constexpr bool IsClipmapView(std::uint32_t view) { return view >= kClipmapFirstView && view < kClipmapFirstView + kNumClipmapLevels; }
    constexpr std::uint32_t LocalView(std::uint32_t i) { return i < kClipmapFirstView ? i : i + kNumClipmapLevels; }
    static_assert(ClipmapView(0u) + kNumCascades == render::ShadowViewClipmapSlot(0u) &&
                  LocalView(kClipmapFirstView) == ClipmapView(kNumClipmapLevels),
                  "VSM view = cull slot - cascades: [prefix spots | prefix faces | clipmap | more spots | more faces]");
    // The page table / request bits of `views` views (341 x 138 = 47058 entries, 188 KB, at the initial capacity).
    constexpr std::uint32_t PageTableEntries(std::uint32_t views) { return kPagesPerView * views; }
    constexpr std::uint32_t RequestWords(std::uint32_t views) { return (PageTableEntries(views) + 31u) / 32u; }

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

    // Step 19: the page-request set is a bitfield (1 bit per virtual page) -- RequestWords above (1471 uints at 138 views).

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
    // frames (UE r.Shadow.Virtual.Cache.MaxPageAgeSinceLastRequest, 1000). Until then a page nobody
    // asked for keeps its content and is taken back only when the pool runs out of empty pages,
    // least recently requested first (vsm_page_alloc_freelist_cs.hlsl) -- it was 16 frames and no
    // eviction under pressure, so the pages of a light looking away left long before UE's would.
    inline constexpr std::uint32_t kLruFrameThreshold = 1000;
    inline constexpr std::uint32_t kMaxLruFrames = 1000; // the settings' / --set's / the slider's range

    // --- Runtime-tunable mirrors (dev-window "VSM" tab). The passes read THESE each frame, so a
    // change applies live. They only feed per-frame CB values / the request dispatch size — NOT
    // buffer sizing (kVirtualRes / kNumMipLevels / kPoolPageCount stay compile-time), so changing
    // them needs no reallocation. Defaults = the constexpr values above. ---
    inline float         g_refDist = kLodRefDist;
    // Local-light plan S10e: UE's dynamic VSM resolution (VirtualShadowMapCacheManager.cpp:155-187, 683-722). When the
    // page pool's load (1 - free / pool, above 1 when requests were refused) passes MaxPagePoolLoadFactor, the local
    // lights' mips are biased coarser -- target = last bias + log2(load / factor), approached by 0.5 a frame while over,
    // by 0.1 once 10 frames have passed under it, clamped to [0, MaxResolutionLodBias]. A refused page reads as LIT
    // (no resident mip), so without it a pool too small for its lights loses their shadows. The bias enters the mip
    // select as a scaled reference distance (LocalRefDist: floor(log2(d / (ref * 2^-b))) = level + b), the request pass
    // and every sampler alike. `--set vsm.dynamicResMaxLodBias:<b>` (UE r.Shadow.Virtual.DynamicRes.MaxResolutionLodBias
    // 2.0, 0 = off), `vsm.dynamicResMaxPoolLoad:<f>` (UE ...MaxPagePoolLoadFactor 0.85, 0 = off).
    inline float         g_dynamicResMaxLodBias = 2.0f;
    inline float         g_dynamicResMaxPoolLoad = 0.85f;
    inline float         g_dynamicLodBias = 0.0f;  // this frame's (VirtualShadowMap::UpdateDynamicResolution)
    inline float LocalRefDist() { return g_refDist * std::exp2(-g_dynamicLodBias); }
    // Plan S10e: the local lights' mip by the camera pixel's footprint in the light's texels (UE's: the light's distance
    // and cone width count, and the render resolution -- a cheaper DLSS mode asks for coarser pages), instead of the
    // camera distance over g_refDist. `--set vsm.localMipFromPixel:0|1`; UE r.Shadow.Virtual.ResolutionLodBiasLocal
    // (0; -1 doubles the resolution) = `vsm.resolutionLodBiasLocal`. The dynamic-resolution bias adds to it. Glass and
    // the fog keep the distance select (their mip falls back coarser when the request's is not resident).
    inline bool  g_localMipFromPixel = true;
    // Plan S10e: the page request tests a receiver against each local light's reach first, and projects a point light's
    // receiver into the one cube face its direction falls in (UE marks a pixel only for its light-grid cell's lights).
    // `--set vsm.requestLightCull:0|1`.
    inline bool  g_requestLightCull = true;
    // Plan S10c: a light whose shadow slot changes takes its pages to the new view (UE's per-light cache) instead of
    // re-rendering them. `--set vsm.lightPageRemap:0|1`.
    inline bool  g_lightPageRemap = true;
    // ...and a light that LEFT the shadowed set keeps its pages this many frames on a view nobody holds; back inside it
    // in time, they move to its new view (UE r.Shadow.Virtual.Cache.MaxLightAgeSinceLastRequest, 10: the unreferenced
    // light keeps its cache entry and gets an unreferenced VSM id). 0 = drop at once. `--set vsm.maxLightAge:<frames>`.
    inline std::uint32_t g_maxLightAgeFrames = 10;
    // Plan S10d (UE distant lights: r.Shadow.Virtual.DistantLightMode 1, r.Shadow.Virtual.MaxDistantUpdatePerFrame 1,
    // ShadowSceneRenderer.cpp:33-46, 210-260, 329-430). A local light whose FINEST possible mip -- the camera pixel at the
    // near side of its sphere, over its whole radius in light depth (GetConservativeMipLevelLocal) -- is the last one has
    // its map in one page a view. Its pages then ignore wind and moving casters (UE skips the invalidations of a single-page
    // map) and draw their casters rigid, and they redraw in a round robin: this many lights a frame, the one waiting
    // longest first (< 0 = all). Turning distant redraws it at once (UE: the flag is part of the light's cache key).
    // `--set vsm.distantLightMode:0|1`, `vsm.maxDistantUpdatePerFrame:<n>`.
    inline int   g_distantLightMode = 1;
    inline int   g_maxDistantUpdatesPerFrame = 1;
    inline float g_resolutionLodBiasLocal = 0.0f;
    inline float LocalMipBias() { return g_resolutionLodBiasLocal + g_dynamicLodBias; }
    // Plan S10e: UE r.Shadow.Virtual.ResolutionLodBiasLocalMoving (1: a moving light's pages one mip coarser) and
    // r.Shadow.Scene.LightActiveFrameCount (10: frames the bias takes to fade back once it stops). A light's bias is
    // lerp(ResolutionLodBiasLocal, max(it, Moving), mobility) (FVirtualShadowMapArray::InterpolateResolutionBias,
    // ShadowSceneRenderer.cpp:275-281); this is what it adds to LocalMipBias(), per light (the request's view entry,
    // the light's GPU data). A moving light re-renders every page it has each frame, so this is a cost cap.
    // `--set vsm.resolutionLodBiasLocalMoving`, `vsm.lightActiveFrameCount`.
    inline float g_resolutionLodBiasLocalMoving = 1.0f;
    inline std::uint32_t g_lightActiveFrameCount = 10;
    inline float LocalMovingMipBias(float mobility)
    {
        const float still = g_resolutionLodBiasLocal;
        return std::clamp(mobility, 0.0f, 1.0f) * (std::max(still, g_resolutionLodBiasLocalMoving) - still);
    }
    // vsm_addressing.hlsli VsmLocalMipFromPixel, term for term (plan S10d classifies lights with it on the CPU).
    inline std::uint32_t LocalMipFromPixel(float pixelRadiusWorld, float lightXScale, float lightDepth, float lodBias)
    {
        const float footprint = std::abs(pixelRadiusWorld * lightXScale / std::max(lightDepth, 1e-4f)) *
                                (2.0f * static_cast<float>(kVirtualRes));
        const float mip = std::log2(std::max(footprint, 1e-8f)) + lodBias;
        return static_cast<std::uint32_t>(std::min(std::max(std::floor(mip), 0.0f), static_cast<float>(kMaxMipLevel)));
    }
    // The camera pixel's radius per unit of view depth (UE RadiusXY = 1 / (ViewSize * ViewToClip._m00_m11), the smaller
    // axis) from the INVERSE projection at the render resolution: 1 / P00 = invProj._11. And the CB's float4 for it.
    inline float PixelRadiusPerDepth(const DirectX::XMFLOAT4X4& invProj, float renderWidth, float renderHeight)
    {
        const float rx = std::abs(invProj._11) / std::max(renderWidth, 1.0f);
        const float ry = std::abs(invProj._22) / std::max(renderHeight, 1.0f);
        return std::min(rx, ry);
    }
    inline DirectX::XMFLOAT4 LocalMipParams(const DirectX::XMFLOAT4X4& invProj, float renderWidth, float renderHeight)
    {
        return DirectX::XMFLOAT4(g_localMipFromPixel ? 1.0f : 0.0f, LocalMipBias(),
                                 PixelRadiusPerDepth(invProj, renderWidth, renderHeight), 0.0f);
    }
    inline std::uint32_t g_requestDownscale = kRequestDownscale;
    // Step 24d: finest directional-clipmap level's world extent (level i covers g_clipmapBaseExtent
    // * 2^i). Tunable for the 24f visual sign-off; only feeds the per-frame view build (no realloc).
    inline float         g_clipmapBaseExtent = 8.0f;
    // [r.Shadow.Virtual.Clipmap.ZRangeScale, UE default 1000] A clipmap level's reach UP-SUN as a
    // multiple of its RADIUS -- same shape as UE's, so the number is directly comparable to theirs.
    //
    // This was `extent * 5`, i.e. radius * 10, which is exactly the MINIMUM UE's own cvar help calls
    // survivable ("should generally be at least 10 or it will result in excessive cache
    // invalidations"). At the finest level that is 40 m of reach: a caster taller than that has its
    // top CLIPPED by the level's near plane, and the piece of shadow that top casts simply vanishes.
    // Measured on a 100-unit pole -- the shadow stopped dead partway down the frame under VSM while
    // CSM drew it in full. Raising g_clipmapBaseExtent "fixed" it only by making every near shadow
    // proportionally coarser.
    //
    // Kept PROPORTIONAL rather than floored at an absolute distance: the per-level NDC depth bias
    // relies on the range scaling with the level (see UpdateClipmap), and an absolute floor would
    // silently change what that bias means at fine levels. UE's own warning bounds the other end --
    // "values that are too large cause depth imprecisions and shadow flickering" -- so this is a
    // measured value, not their 1000.
    // 50 is the MEASURED minimum that restores full shadow reach on a 100-unit caster; below it the
    // shadow visibly truncates (25 -> half length, 10 -> a third). It is not larger because the
    // per-level NDC depth bias is a constant, so a longer range is a proportionally larger WORLD
    // bias -- i.e. peter-panning grows linearly with this number. Measured against scale 10 on
    // ordinary content (noise floor: shadow lift +0.011): 50 -> +0.532, 75 -> +0.731, 100 -> +0.963,
    // and UE's own 1000 -> +8.083, which is unusable here. No flicker at any of them (run-to-run
    // spread 0.06 throughout), so UE's stated failure mode is not what bounds us -- the bias is.
    // Taking UE's 1000 would first require deriving the depth bias FROM the range instead of
    // holding it constant; then the range would be free and this could stop being a compromise.
    // STEP 4 of docs/vsm_smrt_plan.md: raised 50 -> 1000, which is UE's own default.
    //
    // It sat at 50 because the constant NDC depth bias is expressed as a fraction of this range, so
    // stretching the range stretched the bias with it and detached shadows. That coupling is gone:
    // the SMRT march derives its tolerance from its own depth step, and the single-tap arm rescales
    // its constant by kClipmapRangeMultipleRef/ClipmapRangeMultiple() so the bias keeps its meaning.
    //
    // MEASURED on wind_test at the shadow camera, one binary, wind frozen, noise floor 0.008 % of
    // pixels: 50 -> 1000 moves 0.022 % with SMRT and 0.030 % on the shipping single-tap path, with
    // shadow coverage identical to three decimals on both. The control that makes those numbers
    // mean something is ZRangeScale 3, where the range DOES bind: coverage falls to 53.632 % and
    // 3.197 % of pixels change, i.e. the tall caster's shadow is being cut off.
    inline float         g_clipmapZRangeScale = 1000.0f;

    // A clipmap level's depth range as a MULTIPLE OF ITS EXTENT:
    //     depthUp = radius * ZRangeScale = extent * ZRangeScale/2,  depthDown = extent * 1
    // Every NDC quantity that is really a WORLD distance has to be divided by this, or changing the
    // range silently rescales it. That is exactly what went wrong when ZRangeScale went 10 -> 50: a
    // hardcoded `6` (the old 5E + 1E) stayed behind in the depth-bias conversion, so the same NDC
    // bias became 4.3x more world distance and shadows visibly detached from their casters.
    // At the historical ZRangeScale = 10 this returns 6, so the old numbers are preserved exactly.
    inline float ClipmapRangeMultiple() { return g_clipmapZRangeScale * 0.5f + 1.0f; }
    inline constexpr float kClipmapRangeMultipleRef = 6.0f; // what the authored biases were tuned at
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
    // NOT zeroed by Step 4, and the measurement is the reason: with SMRT on this value is already
    // dead (VsmClipmapShadow returns from the march before it is even computed), but the SINGLE-TAP
    // arm still needs it -- zeroing it there moves shadow coverage 54.958 % -> 56.922 %, and that
    // 2-point gain is acne, not shadow. The plan's "set it to 0" assumed SMRT would be the only
    // path; while both ship, the constant belongs to the one that has no march to derive it from.
    inline float         g_clipmapDepthBias = 0.0002f;
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

    // ---- SMRT (shadow-map ray tracing), docs/vsm_smrt_plan.md -------------------------------
    // Marches a ray from the receiver toward the light through the clipmap instead of taking one
    // biased comparison. The march's own step size IS the tolerance, which is what lets the
    // constant depth bias above go to zero -- and with it the ceiling on the clipmap depth range.
    //
    // DORMANT BY DEFAULT (rayCount 0 = today's single-tap SampleCmp path, untouched). Step 1 of
    // the plan ships the machinery, not the behaviour: the single-tap path stays as both the
    // fallback and the A/B arm, exactly as csm.filterMode:0 did for the CSM tent kernels.
    // `--set=vsm.smrtRayCount:1` to turn it on.
    // MUST MATCH VSM_SMRT_MAX_RAYS / VSM_SMRT_MAX_SAMPLES_PER_RAY in shaders/vsm_smrt.hlsli. The
    // shader is bounded by its own literals and cannot be talked past these by any CB value -- that
    // is the actual safety. These exist so a setting is REJECTED at the knob instead of being
    // silently truncated on the GPU, which would make the knob lie about what it is doing.
    inline constexpr std::uint32_t kSmrtMaxRays = 16u;
    inline constexpr std::uint32_t kSmrtMaxSamplesPerRay = 32u;
    inline std::uint32_t g_smrtRayCount = 7u;
    // UE's r.Shadow.Virtual.SMRT.SamplesPerRayDirectional (their default 8).
    inline std::uint32_t g_smrtSamplesPerRay = 8u;
    // UE's r.Shadow.Virtual.SMRT.RayLengthScaleDirectional (their default 1.5), scaled by distance
    // to camera. Dimensionless, so it transfers from their centimetres unchanged. Their own
    // warning, and it is the knob that replaces the depth bias as the thing to tune: "Too high
    // values will cause shadows to detach from their contact points (unless more samples are
    // used). Too low values will greatly restrict how large penumbras can be."
    inline float         g_smrtRayLengthScale = 1.5f;
    // UE's r.Shadow.Virtual.SMRT.ExtrapolateMaxSlopeDirectional. THEIR DEFAULT 5.0 IS IN
    // CENTIMETRES -- this engine's world unit is the metre, so the same quantity is 0.05 here.
    // Copying the 5.0 would have been a hundredfold over-clamp, i.e. no clamp at all.
    //
    // What it does (step 2): once the march passes BEHIND an occluder it stops comparing against
    // the occluder's surface and starts extrapolating that surface along its own measured slope,
    // so a penumbra keeps widening past the silhouette instead of ending at it. The clamp is what
    // stops that extrapolation from leaking light behind a SECOND occluder. UE's own note: "Higher
    // values allow softer penumbra edges but can introduce light leaks behind second occluders.
    // Setting to 0 will disable slope extrapolation."
    //
    // 0 is still a valid setting and is exactly their SMRT_EXTRAPOLATE_SLOPE=0 path -- with the
    // clamp at 0 the extrapolated depth collapses to the last recorded one.
    inline float         g_smrtExtrapolateMaxSlope = 0.05f;
    // Step 3 -- the light's angular SIZE, which is what a penumbra is made of. UE's directional
    // default SourceAngle is 0.5357 degrees (the real sun's disc); the shader wants sin of the
    // RADIUS, so half of it. With 0 here every ray collapses onto the light axis and rayCount is
    // pure cost -- which is exactly the state Steps 1 and 2 shipped in.
    inline float         g_smrtSourceAngleDeg = 0.5357f;
    // r.Shadow.Virtual.SMRT.TexelDitherScaleDirectional (UE default 2.0), in texels.
    inline float         g_smrtTexelDitherScale = 2.0f;
    // Fraction of a clipmap level's square within which a receiver is accepted, SMRT only.
    // 1.0 = the finest level that contains it at all, which is the single-tap rule and keeps the
    // sharpest level available. 0.5 mirrors UE, whose level covers twice the radius its selection
    // tests (VirtualShadowMapClipmap.cpp:349) -- but a margin is bought with A LEVEL OF SHADOW
    // RESOLUTION, and on fine geometry like palm fronds that reads as blocky slabs where there
    // were leaflets. Default 1.0: the theory the margin was added for (rays running out of level)
    // was disproven by instrumenting the march, so it has to earn its place on looks alone.
    inline float         g_smrtLevelMargin = 1.0f;
    // TEMPORAL DITHER. Rotates the whole per-pixel sample set once per frame, the way UE feed
    // View.StateFrameIndex into their blue-noise lookup. On its own it makes a still frame NOISIER;
    // it pays off through the temporal pass, which sees a different set each frame and averages
    // them. Default ON because this engine ships with DLSS on -- turn it off to judge a single
    // frame, or when running with --dlss=off.
    inline bool          g_smrtTemporalDither = true;
    // r.Shadow.Virtual.SMRT.AdaptiveRayCount (UE default 1): after this many rays, a wave whose
    // lanes ALL agree stops early. Fully lit and fully umbral regions are most of the screen and
    // need one or two rays; only penumbrae need the full count. 0 disables it.
    inline std::uint32_t g_smrtAdaptiveRayCount = 1u;

    // SCREEN-SPACE RAY (r.Shadow.Virtual.ScreenRayLength, UE default 0.015). A short march through
    // the CAMERA depth buffer away from the receiver, whose result becomes the SMRT ray's start
    // offset. UE call it "smart shadow bias": it buys contact detail below the shadow map's texel
    // scale AND skips the near band where every bias argument lives.
    //
    // The value is a MULTIPLE OF VIEW DEPTH, not a distance -- so it is dimensionless and their
    // 0.015 transfers from centimetres unchanged. 0 disables the trace.
    // DEFAULT 0 -- OFF, against UE's 0.015, and the measurement is why.
    //
    // Its job is to skip the ambiguous band next to the receiver so the SMRT ray does not start
    // inside its own surface. UE need that because they run with no constant depth bias at all.
    // WE DO NOT HAVE THAT PROBLEM: SMRT here already measures 0.044 % acne with the constant bias
    // not applied, so there is nothing for the trace to rescue.
    //
    // And it is not free of consequence. Starting the ray further out moves shadow coverage
    // MONOTONICALLY DOWN -- 54.948 % (off) -> 54.902 % (0.015) -> 54.795 % (0.05) -- which is the
    // peter-panning direction: shadows lifting off their contact points. That is precisely what
    // contact shadows (render::contact) exist to tighten, so paying four depth samples to work
    // against them is a bad trade here.
    //
    // Kept, not deleted: the code is a faithful transcription and a scene with real acne would
    // want it. Turn it on with `--set=vsm.smrtScreenRayLength:0.015`.
    inline float         g_smrtScreenRayLength = 0.0f;
    inline std::uint32_t g_smrtScreenRaySamples = 4u; // UE's SCREEN_RAY_SAMPLES
    // Local-light (spot + point) VSM shadow bias, in units of one shadow texel at the receiver
    // (auto-sized per-pixel from the light's cone width, distance and mip level in the shaders).
    // Lateral = surface-normal offset (~1 texel keeps Peter-panning to a texel); depth push =
    // along-the-light-ray, slope-scaled, so grazing acne clears without moving flat lit ground.
    // Live-tunable in the Developer window's VSM section (fed to the spot/point CBs each frame).
    // Plan S10b: lateral 1.5 = the 3x3 PCF's reach (one texel per tap plus the bilinear compare's half) -- plan S9's rule
    // that the push must cover the filter. Measured with the texel bias on (10 ring spots at night, against the atlas):
    // pixels darker than the atlas by >= 3 % -- 12.6 % with the old bias, 0.51 % at 1.0, 0.07 % at 1.5, 0.04 at 2.0;
    // the shadows' feet do not move (pixels lighter than the atlas stay 1.72 -> 1.75 %).
    inline float         g_localLateralTexels   = 1.5f;
    inline float         g_localDepthPushTexels = 0.5f;
    // Local-light plan S10b: the texel is that of the mip the sampler actually READS (a page that fell back to a coarser
    // level has bigger texels -- sized for the requested one, the whole page went to acne: hatched quads with bright
    // seams), and the push is along the GEOMETRIC normal rebuilt from depth (a normal map's ripple left a ripple of acne
    // at any push -- plan S9). 0 = the old bias (requested level, shading normal). `--set vsm.localTexelBias:0|1`,
    // `vsm.localLateralTexels`, `vsm.localDepthPushTexels`.
    inline bool          g_localTexelBias = true;
    inline std::uint32_t g_lruThreshold = kLruFrameThreshold;
    // A page nobody requested for this many frames that the last render still found dirty goes at the next allocation
    // instead of being redrawn again (vsm_page_alloc_freelist_cs.hlsl). The setup redraws every dirty RESIDENT page --
    // the sub-sampled request misses pages pixels still read -- so with a 1000-frame page age a view the camera left
    // kept being redrawn under wind: VsmPageRender 1.8 -> 3.0 ms on wind_test. 16 = the old page age, the window the
    // sub-sampling needs. 0 = off. `--set vsm.redrawUnrequestedFrames`.
    inline std::uint32_t g_redrawUnrequestedFrames = 16;

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
    // A/B lever for the per-instance caster LOD (the caster==receiver contract): 1 = bucket every
// FLOOR caster by its own receiver LOD (correct; the default), 0 = the old per-view LOD for
// every instance of a group (reproduces the pre-contract record layout inside one binary, for
// cost attribution). Chunk EXACT is unaffected. --set=vsm.perInstanceCasterLod
inline bool g_perInstanceCasterLod = true;
inline bool g_scatterLocalViews = true;

    // Occlusion plan S5b.2: light-space two-pass HZB occlusion of the clipmap pages (Unreal's
    // r.Shadow.Virtual.NonNanite.UseHZB). A (caster, page) pair that last frame's pool pyramid
    // hid is deferred; pass A draws the rest; the pyramid is rebuilt from the pages pass A drew;
    // the deferred pairs are retested against it and pass B draws the survivors into the same
    // pages. Zero latency and no holes by construction. Needs the scatter and the single-draw
    // page render. `--set=vsm.hzbCull:0|1` is the A/B.
    // DEFAULT OFF BY MEASUREMENT (2026-09-04, Release, one binary): on the wall level under a
    // camera drift (728 pairs hidden from the sun per frame) Pass_VsmPageRender is 0.586 vs
    // 0.594 ms -- the raster it saves pays exactly for the pyramid (0.034), the retest (0.028)
    // and the scatter's prev test (+0.03); in the palm grove with wind (19 pairs hidden) it is
    // pure cost, 0.19 -> 0.31 ms, +0.11 ms on a 2.0 ms frame. Same verdict as the S5b.1 cascade
    // cull: the mechanism is correct (parity at the floor, GBV CLEAN) and waits for content whose
    // hidden casters are expensive -- a town, walls with interiors -- not palms under a high sun.
    inline bool g_hzbCull = false;

    // Page cache (Rung 1): skip re-rendering pages whose content didn't change (cached depth kept;
    // only new / dynamic-caster-overlapping / forced pages re-render). DEFAULT OFF: measured a net
    // loss on the current scene — VsmPageRender is dominated by the per-page CULL (which caching
    // can't skip), the teapots are dynamic (uncacheable), and the gated depth-clear is slower than a
    // hardware full-pool clear on spike frames (max regressed). ON draws only dirty pages + uses the
    // gated clear; keep for a future coarse-skip (skip the cull for pages far from any mover).
    inline bool g_pageCaching = true;
// Wind page-cache range (Unreal's per-level WPO disable, transcribed): clipmap levels BELOW
// this animate wind (and re-render every frame while it blows); levels at/above render their
// casters RIGID and can cache. Locals always animate. Only matters with g_pageCaching on —
// without caching every page redraws anyway. --set=vsm.windMaxLevel; >= level count = all.
inline std::uint32_t g_windAnimateMaxLevel = 4;

    // Periodically log the VSM page stats to DBWIN ("[VSM] request ... | resident=..."). DEFAULT OFF
    // — it spams a captured stress/dev run. The on-screen dev-window "VSM" tab always shows the live
    // stats; flip this on only when you want them in the debug output too.
    inline bool g_logPageStats = false;

    // Per-shadow-view data (BuildVsmViews' list): what the request pass projects screen pixels through, and the view
    // matrix the page render draws with. params.x = valid (0/1), .y = a point face's cube face (0..5; -1 = a spot or
    // a clipmap level), .z = zFar, .w = the projection's x scale (plan S10e: the pixel-footprint mip). cull = the light's
    // position + reach (w < 0 = none; plan S10e: the request skips receivers it does not reach).
    struct alignas(16) ViewProjEntry
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4   params;
        DirectX::XMFLOAT4   cull{ 0.0f, 0.0f, 0.0f, -1.0f };
        // plan S10e: x = the light's mip bias on top of the frame's; y = 1: past render::g_localShadowWindRange (rigid)
        DirectX::XMFLOAT4   mip{ 0.0f, 0.0f, 0.0f, 0.0f };
    };
    // Plan S9e: the GPU's per-view record (VsmViewData in vsm_addressing.hlsli, 192 bytes), uploaded into a StructuredBuffer
    // twice a frame -- the request pass's slot (the entry above + bits.w = the page remap) and the render pass's (the
    // matrices + bits.x flags + bits.y the view's shadow LOD) -- where CB arrays could hold at most 64 KB of views.
    inline constexpr std::uint32_t kViewDirty = 1u;          // VSM_VIEW_DIRTY: the matrix changed (the page cache)
    inline constexpr std::uint32_t kViewDistant = 2u;        // VSM_VIEW_DISTANT (plan S10d)
    inline constexpr std::uint32_t kViewDistantUpdate = 4u;  // VSM_VIEW_DISTANT_UPDATE
    // VSM_VIEW_WIND_RIGID: a local light past render::g_localShadowWindRange (ViewProjEntry.mip.y != 0) -- its pages draw
    // the casters rigid and take no wind dirt.
    inline constexpr std::uint32_t kViewWindRigid = 8u;
    inline constexpr std::uint32_t kRemapKeep = 0xFFFFFFFFu; // vsm_page_alloc_remap_cs.hlsl kRemapKeep / kRemapDrop
    inline constexpr std::uint32_t kRemapDrop = 0xFFFFFFFEu;
    struct alignas(16) ViewGpuData
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4X4 prevViewProj;
        DirectX::XMFLOAT4   params;
        DirectX::XMFLOAT4   cull;
        DirectX::XMFLOAT4   mip;
        DirectX::XMUINT4    bits;
    };
    static_assert(sizeof(ViewGpuData) == 192, "ViewGpuData mirrors VsmViewData (vsm_addressing.hlsli)");
    struct alignas(16) PageRequestConstants
    {
        DirectX::XMFLOAT4X4 invView;
        DirectX::XMFLOAT4X4 invProj;
        DirectX::XMFLOAT4   camPosWS;   // xyz
        DirectX::XMFLOAT4   screen;     // x=w, y=h, z=1/w, w=1/h
        DirectX::XMFLOAT4   lodParams;  // x=refDist, y=maxLevel, z=downscale, w=clipmap blend width
        DirectX::XMFLOAT4   localMip;   // plan S10e: x = pixel-footprint mip on, y = lod bias, z = pixel radius / depth
        std::uint32_t       numViews;
        // Plan S9e (cost without a limit): the request walks LIGHTS, not views -- the shadowed spots (their views
        // SpotView(0..)) and points (six faces each from PointFaceView(slot * 6), the extra lights' faces from
        // pointExtraView = PointFaceView(6 x the prefix's points)), a point's sphere once for its six faces.
        std::uint32_t       spotLights = 0;
        std::uint32_t       pointLights = 0;
        std::uint32_t       pointExtraView = 0;
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
        // S5b.2: the page table as it stood during last frame's render is copied aside BEFORE
        // this frame's allocation rewrites it -- the scatter finds last frame's physical page
        // of a virtual page there (Unreal's PrevBuffers.PageTable).
        bool hzbSnapshot = false;
        std::uint32_t snapshotCopy = 0;    // pageTable -> COPY_SOURCE, prevPageTable -> COPY_DEST
        std::uint32_t alloc = 0;           // the six alloc buffers -> UAV
        // Plan S10e: the alloc counters into this frame's readback slot, every frame (dynamic resolution).
        bool remap = false;                // S10c: this frame moves pages between views (PlanViewRemap)
        std::vector<std::uint32_t> viewRemap; // ...per OLD view: keep / drop / new view (filled only when `remap`)
        bool counters = false;
        std::uint32_t countersCopy = 0;    // counters -> COPY_SOURCE
        std::uint32_t countersRestore = 0; // ...and back to UAV
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
        // Clipmap levels below this wind-animate and wind-dirty; 0 = calm. See gWindDirtyMaxLevel.
        std::uint32_t windDirtyMaxLevel = 0u;
        // Plan S10d: the views of distant lights, and of those the round robin redraws this frame (bit per view, 32 per
        // word; empty when there are none).
        std::vector<std::uint32_t> distantMask;
        std::vector<std::uint32_t> distantUpdate;
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
        // S5b.2: the two-pass light occlusion runs this frame (scatter + single draw + PSOs +
        // buffers); `hzbPrevValid` = last frame's pyramid may be tested against (a render with a
        // build happened last frame, same allocation epoch); `hzbFull` = rebuild every resident
        // page and zero the free ones (first build / after a level switch).
        bool hzb = false;
        bool hzbPrevValid = false;
        bool hzbFull = false;
        std::uint32_t pointHzbBuild = 0;   // pool -> NPS, pyramid -> UAV, dirty bits -> NPS
        std::uint32_t pointHzbPost = 0;    // pyramid -> NPS, list/countsB/counters -> UAV, pairs -> NPS
        std::uint32_t pointSetupB = 0;     // countsB -> NPS, argsB/argCountB/dirty -> UAV
        std::uint32_t pointDrawB = 0;      // pool -> DEPTH_WRITE, argsB -> INDIRECT, list -> vertex, counters -> COPY_SOURCE
        std::uint32_t pointHzbRestore = 0; // counters -> UAV
        // Tessy N3: this frame the setup hands the buckets of groups with clusters to the cluster cull and the clusters
        // are drawn (tessy::g_vsm, the scatter + single draw, the tables, both PSOs). `tessyStats` = the counters exist
        // (cleared, counted and read back every frame, whatever `tessy` says: they are N0's pair counter too).
        bool tessy = false;
        bool tessyStats = false;
        const char* tessyOffReason = ""; // why tessy.vsm is on and `tessy` is not ("" otherwise)
        // Tessy N6: the clusters go through the mesh shader (tessy::g_vsmMeshShader, a mesh shader tier, its PSO, the
        // args pass, the DispatchMesh signature); false with `tessy` = the VS path. `tessyMsOffReason` says why not.
        bool tessyMs = false;
        const char* tessyMsOffReason = "";
        std::uint32_t tessyMsCull = 0; // tessy::g_vsmMsCull's bits (tessy_common.hlsli kTessyMsCull*), 0 off the MS path
        // Tessy N7c: the runs of groups with a cluster DAG on a clipmap page take the DAG's cut (tessy::g_lod).
        bool tessyLod = false;
    };

    // Tessy N0/N3 counters of frame N - kFrameCount (tessy_vsm_cull_cs.hlsl names the bytes). Pairs = (page, caster)
    // instances; "whole" = drawn as the whole mesh, "tessy" = handed to the cluster cull (`tessyWholeTris` = what they
    // would have cost drawn whole).
    struct TessyStats
    {
        std::uint32_t clusters = 0;        // drawn
        std::uint32_t records = 0;         // the draw's instances / mesh shader groups: clusters on the VS path, meshlets on the MS's
        std::uint32_t runs = 0;            // (page x bucket) runs the setup appended
        std::uint32_t wholePairs = 0;
        std::uint32_t wholeTris = 0;
        std::uint32_t tessyPairs = 0;
        std::uint32_t tessyWholeTris = 0;
        std::uint32_t clustersTested = 0;
        std::uint32_t clusterTris = 0;     // the drawn clusters' triangles
        std::uint32_t clustersLost = 0;    // past the visible-cluster buffer (a hole: must stay 0)
        std::uint32_t runsLost = 0;        // past the run list (drawn whole instead: no hole)
        std::uint32_t padMisses = 0;       // tessy.validate: clusters whose swayed vertices left the padded box
        std::uint32_t padMissMaxUm = 0;    // ...and by how far at most, in micrometres
        std::uint32_t padNeedCm = 0;       // tessy.validate: the boxes' growth the sway needed (six faces, summed, cm)
        std::uint32_t padGotCm = 0;        // ...and the growth the pad gave them: the bound's slack is the ratio
        std::uint32_t padMaxOffCm = 0;     // tessy.validate: sum of each cluster's largest true vertex displacement
        std::uint32_t padDCm = 0;          // ...of its shear bound D
        std::uint32_t padFCm = 0;          // ...and of its flutter bound F
        std::uint32_t rectCulled = 0;      // tessy.clusterCull: clusters that held no texel centre of their page
        bool tessyOn = false;              // that frame drew clusters
        bool tessyMs = false;              // ...through the mesh shader (N6)
        bool valid = false;
    };

    // Registers this frame's exact resource uses AND returns the decisions they were derived
    // from. Call only on frames the pass will record (the AddPass2 builder gates both at once).
    PageRenderDecisions PrepareRenderPass(RenderGraphPassContext& ctx, ShadowGpuData* shadowGpu,
                                          const vfx::WindState* wind);
    // Step 24b: free every VSM GPU allocation (Legacy mode / teardown). MUST be at GPU idle.
    void ReleaseResources();
    bool IsAllocated() const { return pagePool_ != nullptr && pageTable_ != nullptr; }
    // Plan S9e: the views the page table, the request bits and the per-view records were made for (vsm::MaxVirtualViews
    // at EnsureResources); 0 while unallocated. A capacity change re-creates VSM (Scene::ReconcileShadowCapacity).
    std::uint32_t ViewCount() const { return views_; }

    // Resources + views for the future VSM passes; null/{0} until allocated.
    ID3D12Resource* PagePool() const { return pagePool_.Get(); }
    ID3D12Resource* PageTable() const { return pageTable_.Get(); }
    // Level switch: drop every page mapping — the next RecordPageAllocate re-runs the one-shot
    // init (page table zeroed, all physical pages freed). Cross-level pool content is garbage:
    // the new level's views reuse the same view slots, so its virtual page ids collide with
    // resident entries whose depths were rendered under the OLD level's viewProj/casters.
    void InvalidateAllPages() { allocInitialized_ = false; hzbInvalidate_ = true; }

    // S5b.2: the counters of frame N - kFrameCount -- [0] (caster, page) pairs the scatter
    // deferred, [1] pairs pass B drew, [2] pairs past the deferred-list capacity (drawn in A).
    const std::array<std::uint32_t, 4>& HzbStats() const { return hzbStats_; }
    void PollHzbStats(Renderer* renderer); // once per frame, main thread (the Tessy counters too)
    bool HzbCullThisFrame() const { return hzbThisFrame_; }
    const TessyStats& GetTessyStats() const { return tessyStatsLast_; }
    // Why tessy.vsm is not drawing clusters this frame (empty when it is, or when the knob is off).
    const char* TessyOffReason() const { return tessyOffReason_; }
    // N6: why tessy.raster 1 draws through the VS this frame (empty when the mesh shader draws, or the knob is 0).
    const char* TessyMsOffReason() const { return tessyMsOffReason_; }

    D3D12_CPU_DESCRIPTOR_HANDLE PagePoolDsv() const { return poolDsv_; }     // render pages (Step 22)
    D3D12_CPU_DESCRIPTOR_HANDLE PagePoolSrv() const { return poolSrv_; }     // sample (Step 21)
    D3D12_CPU_DESCRIPTOR_HANDLE PageTableSrv() const { return pageTableSrv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE PageTableUav() const { return pageTableUav_; } // written by Step 20

    // Step 19: run the screen-space page-request pass into `cl` (an open command list). Clears
    // the request bitfield, then for each visible pixel projects into every active shadow view
    // and marks the virtual page it needs. `constants` carries the camera, `views` the frame's shadow views (plan S9e:
    // uploaded with `pts`' page remap as this frame's request slot, which RecordPageAllocate's remap reads too);
    // `depthSrv` is the camera depth.
    void RecordPageRequest(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                           const vsm::PageRequestConstants& constants,
                           const vsm::ViewProjEntry* views, std::uint32_t viewCount, const PageRequestPoints& pts,
                           D3D12_CPU_DESCRIPTOR_HANDLE depthSrv, UINT screenW, UINT screenH);

    // Step 20: consume the request bitfield produced by RecordPageRequest (same command list,
    // right after it) and turn it into physical-page assignments with cross-frame caching: touch
    // resident+requested pages (LRU keep-alive), free LRU-stale pages back to a free list, then
    // allocate a physical page for each requested-but-not-resident page and append it to the
    // needs-render list (Step 22 input). Persists the page table + free-list/LRU state across
    // frames. Nothing samples/renders the pages yet (add-dormant). Also records the debug readback.
    // `squares` feeds the clipmap LOD fallback chain (pass 4); null or count 0 skips it, which is
    // the correct behaviour when the level has no sun.
    void RecordPageAllocate(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                            const PageRequestPoints& pts,
                            const vsm::ClipmapSquares* squares);

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

    // Local-light plan S10e: UE's dynamic resolution -- reads this frame slot's alloc counters (the frame that last
    // used the slot, kFrameCount ago) and moves vsm::g_dynamicLodBias. Once per frame on the main thread, after
    // BeginFrame waited on the slot and before any pass records (they read vsm::LocalRefDist()).
    void UpdateDynamicResolution(Renderer* renderer);
    struct DynamicResStats { float poolLoad = 0.0f; float lodBias = 0.0f; std::int32_t refused = 0; bool valid = false;
                             bool settling = false; };
    const DynamicResStats& DynamicRes() const { return dynamicRes_; }
    // S10e: the pool has not settled -- requests were refused, or the bias is still moving. The skip-when-still update
    // must not freeze it then: the controller learns only from frames that allocate, and the pages the old bias asked
    // for give way only to pages a frame requests (the allocate pass's LRU eviction).
    bool DynamicResSettling() const { return dynamicRes_.valid && dynamicRes_.settling; }

    // Local-light plan S10c (UE's per-light VSM cache): which LIGHT each VSM view holds. Scene hands this frame's
    // (SetViewOwners, from the shadow slots); the request pass's builder compares it with what the pool's pages were
    // made for (PlanViewRemap) and, where a light changed view, moves its pages along instead of re-rendering them
    // (vsm_page_alloc_remap_cs.hlsl) -- and the prev-matrix table the page cache's dirty test reads. A light that left
    // the set PARKS its pages for vsm::g_maxLightAgeFrames (UE's unreferenced cache entry): on its own view while nobody
    // takes it, else moved to a free one; back in time, they move to its new view. Nothing renders a parked view (its
    // light has no cull view to draw from), so a frame that could change its pages unseen -- one that dirties pages
    // wholesale (forceAll, wind on the locals) or moves a caster -- frees them instead (parkedStale_).
    static constexpr std::uint32_t kNoViewOwner = 0xFFFFFFFFu;
    // Plan S9e: one per view (ViewCount; SetViewOwners takes any length and keeps the first ViewCount, the rest none).
    using ViewOwners = std::vector<std::uint32_t>;
    static constexpr std::uint32_t SpotViewOwner(std::uint32_t lightIndex) { return lightIndex & 0x7FFFFFFFu; }
    static constexpr std::uint32_t PointFaceViewOwner(std::uint32_t lightIndex, std::uint32_t face)
    {
        return 0x80000000u | ((lightIndex & 0x0FFFFFFFu) << 3u) | (face & 7u);
    }
    static ViewOwners NoViewOwners(std::uint32_t views) { return ViewOwners(views, kNoViewOwner); }
    void SetViewOwners(const ViewOwners& owners)
    {
        nextViewOwners_.assign(views_, kNoViewOwner);
        std::copy_n(owners.begin(), std::min<size_t>(owners.size(), views_), nextViewOwners_.begin());
    }
    bool ViewOwnersPending() const { return nextViewOwners_ != appliedViewOwners_; }
    // Builder (main thread), before PrepareRequestPass, only on a frame the pool updates. `frame` = the renderer's.
    void PlanViewRemap(std::uint64_t frame);
    // moved = views moved (to a light's new view, a return or a parking spot); dropped = views freed for an arrival or
    // because their parked light did not come back in time (expired) or could not vouch for them (stale).
    struct RemapStats
    {
        std::uint32_t moved = 0, dropped = 0, parked = 0, returned = 0, expired = 0, stale = 0;
        std::uint64_t frames = 0;
    };
    const RemapStats& Remaps() const { return remapStats_; }
    bool IsViewParked(std::uint32_t view) const
    {
        return view < parkedOwners_.size() && parkedOwners_[view] != kNoViewOwner;
    }

    // Plan S10d (vsm::g_distantLightMode): which views belong to distant lights this frame and which of those lights the
    // round robin redraws. Builder of the page-render pass, once a frame, after PlanViewRemap (the view owners name the
    // lights) and before PrepareRenderPass (which hands the result to the setup). `views` = BuildVsmViews' list (cull =
    // the light's sphere), the camera = the main view, `pixelRadiusPerDepth` = vsm::PixelRadiusPerDepth at render res.
    void PlanDistantLights(const vsm::ViewProjEntry* views, std::uint32_t viewCount, const DirectX::XMFLOAT3& camPos,
                           const DirectX::XMFLOAT3& camForward, float pixelRadiusPerDepth, std::uint64_t frame);
    // lights = distant this frame; updates = redraws the round robin scheduled; turned = lights that turned distant.
    struct DistantStats { std::uint32_t lights = 0; std::uint64_t updates = 0, turned = 0; };
    const DistantStats& Distant() const { return distantStats_; }

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
    GpuResource pageTable_;  // StructuredBuffer<uint>[PageTableEntries(views_)]
    // Plan S9e: the view count everything below was sized for (see ViewCount), and its derived sizes.
    std::uint32_t views_ = 0;
    std::uint32_t PageTableEntries() const { return vsm::PageTableEntries(views_); }
    std::uint32_t RequestWords() const { return vsm::RequestWords(views_); }
    // Plan S9e: the per-view records (vsm::ViewGpuData), an UPLOAD ring of kFrameCount x 2 slots of views_ records -- slot
    // f * 2 the request pass's, f * 2 + 1 the render pass's -- persistently mapped, one SRV per slot. The CPU writes a slot
    // only in the frame that owns it (BeginFrame waited on its fence); viewDataHigh_ = how many records a slot's last write
    // left non-default (the rest stay zero / kRemapKeep from creation, so a write touches no more than it must).
    static constexpr std::uint32_t kViewDataSlots = render::kFrameCount * 2u;
    Microsoft::WRL::ComPtr<ID3D12Resource> viewData_;
    std::uint8_t* viewDataPtr_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> viewDataHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE viewDataSrv_[kViewDataSlots]{};
    std::uint32_t viewDataHigh_[kViewDataSlots]{};
    void EnsureViewData(Renderer* renderer);
    // Writes records [0, count) of slot `slot` from `fill(i, record)` and resets the ones the slot's last write left past
    // that; returns the slot's SRV ({0} when the ring is missing).
    template <typename Fill>
    D3D12_CPU_DESCRIPTOR_HANDLE WriteViewData(std::uint32_t slot, std::uint32_t count, Fill&& fill);
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
    std::shared_ptr<Material> allocPropagateMat_;    // vsm_page_propagate_cs.hlsl (clipmap LOD fallback)
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
    D3D12_CPU_DESCRIPTOR_HANDLE physOwnerPrevUav_{}; // S10c: the page remap (vsm_page_alloc_remap_cs.hlsl)
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
    // ---- Tessy N3: the cluster cull's runs, its survivors and the counters (+ the draw's args) ----
    // Fixed capacities, allocated with the render resources whatever tessy.vsm says (a switch allocates nothing). A
    // run past kTessyRunCap is drawn whole by the setup (never lost); a cluster past kTessyVisibleCap is lost and
    // counted (TessyStats::clustersLost -- a warning, it must stay 0).
    static constexpr std::uint32_t kTessyRunCap = 65536u;      // 1 MB
    static constexpr std::uint32_t kTessyVisibleCap = 524288u; // 8 MB
    static constexpr std::uint32_t kTessyStatsUints = 32u;     // tessy_vsm_cull_cs.hlsl names the bytes
    static constexpr std::uint32_t kTessyCullGroups = 1024u;   // thread groups of the cull; each walks runs g, g + 1024, ...
    GpuResource tessyRuns_;      // uint4 {page, first list entry, casters, virtual group}
    GpuResource tessyVisible_;   // uint4 {list entry, cluster index, base vertex, 0} (tessy_common.hlsli)
    GpuResource tessyStats_;     // [0..3] D3D12_DRAW_ARGUMENTS, the counters (TessyStats), [20..22] DISPATCH_MESH args
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> tessyHeap_; // runs UAV, visible UAV, visible SRV, stats raw UAV
    D3D12_CPU_DESCRIPTOR_HANDLE tessyRunsUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE tessyVisibleUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE tessyVisibleSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE tessyStatsUav_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> tessyReadback_[render::kFrameCount];
    const std::uint32_t* tessyReadbackPtr_[render::kFrameCount] = {};
    std::uint64_t tessyReadbackFrame_[render::kFrameCount] = {};
    bool tessyReadbackOn_[render::kFrameCount] = {}; // that frame drew clusters
    bool tessyReadbackMs_[render::kFrameCount] = {}; // ...through the mesh shader
    TessyStats tessyStatsLast_{};
    std::shared_ptr<Material> tessyCullMat_;          // tessy_vsm_cull_cs.hlsl
    std::shared_ptr<Material> tessyMsArgsMat_;        // tessy_ms_args_cs.hlsl (N6)
    bool tessyLast_ = false;                          // last frame's decision: a switch re-renders every page once
    bool tessyMsLast_ = false;                        // ...and the rasterizer's (N6), the same rule
    const char* tessyOffReason_ = "";
    const char* tessyMsOffReason_ = "";
    float tessyLodKeyLast_ = 0.0f; // N7c: the DAG cut as last frame took it (its texels, 0 = off; a change flushes the cache)
    const char* tessyLoggedReason_ = nullptr;         // the reason last logged (an event per change)
    const char* tessyMsLoggedReason_ = nullptr;
    void EnsureTessyResources(Renderer* renderer);
    // ---- Occlusion plan S5b.2: the light-space two-pass HZB occlusion of the clipmap pages ----
    // The pool pyramid: R32_FLOAT at half the pool, 7 mips (2048 -> 32), i.e. a self-contained
    // 64 -> 1 chain per 128-texel page, storing 1 - z with a min reduction (vsm_hzb_build_cs.hlsl).
    // Persistent like the pool: a cached page's region still describes its cached depth.
    static constexpr std::uint32_t kHzbMips = 7;
    GpuResource vsmHzb_;
    GpuResource prevPageTable_;   // the page table during last frame's render (snapshot before alloc)
    GpuResource deferredPairs_;   // uint2[deferredCap_]: (leading caster slot, virtual page id)
    GpuResource hzbCounters_;     // uint[4]: deferred, drawn in B, overflow, pad
    GpuResource pageGroupCountB_; // pass B's per (page, virtual group) counts (shape of pageGroupCount_)
    GpuResource pageDrawArgsB_;   // pass B's args (shape of pageDrawArgs_)
    GpuResource pageArgCountB_;   // pass B's compaction counter
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> hzbHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE hzbSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE hzbMipUav_[kHzbMips]{};
    D3D12_CPU_DESCRIPTOR_HANDLE prevPageTableSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE deferredPairsUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE deferredPairsSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE hzbCountersUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageGroupCountBUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageGroupCountBSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageDrawArgsBUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageArgCountBUav_{};
    std::uint32_t deferredCap_ = 0;         // entries deferredPairs_ holds
    std::uint32_t scatterGroupsB_ = 0;      // group count pageGroupCountB_/pageDrawArgsB_ are sized for
    std::shared_ptr<Material> hzbBuildMat_; // vsm_hzb_build_cs.hlsl
    std::shared_ptr<Material> hzbPostMat_;  // vsm_hzb_post_cs.hlsl
    // Cross-frame validity (the builder's): the pyramid describes the pool iff every render since
    // its build also built it; a level switch (InvalidateAllPages) voids it and asks for a full build.
    std::uint64_t lastRenderFrame_ = 0;
    std::uint64_t hzbLastBuildFrame_ = 0;
    bool hzbInvalidate_ = true;
    bool hzbThisFrame_ = false;
    int hzbLogged_ = 0x7fffffff;   // last logged (on, prevValid | off-reason) state
    Microsoft::WRL::ComPtr<ID3D12Resource> hzbReadback_[render::kFrameCount];
    const std::uint32_t* hzbReadbackPtr_[render::kFrameCount] = {};
    std::uint64_t hzbReadbackFrame_[render::kFrameCount] = {};
    std::array<std::uint32_t, 4> hzbStats_{};
    void EnsureHzbResources(Renderer* renderer, std::uint32_t groups, std::uint32_t casters);
    bool groupsBSized_(ShadowGpuData* shadowGpu) const;
    // The pass-B chain after pass A's single draw (see the definition). `writeSetupCB` fills the
    // setup CB exactly as pass A did, with the pass-B flag.
    void RecordHzbPassB(Renderer* renderer, ID3D12GraphicsCommandList* cl, ShadowGpuData* shadowGpu,
                        D3D12_CPU_DESCRIPTOR_HANDLE viewsSrv, const PageRenderDecisions& dec,
                        ID3D12CommandSignature* sig, Material* pageMat, RenderContext& ctx, UINT setupCbSize,
                        const std::function<void(std::uint8_t*, std::uint32_t)>& writeSetupCB);
    std::uint32_t   renderGroups_ = 0;           // mesh-group count pageDrawArgs_ is sized for
    std::uint32_t   scatterGroups_ = 0;          // mesh-group count pageGroupCount_ is sized for
    std::uint32_t   renderCasters_ = 0;          // caster count pageVisibleList_ is sized for
    // Last frame's per-view matrices (page cache: a moved view re-renders wholesale; see
    // gViewDirtyMask in vsm_page_setup_cs.hlsl).
    std::vector<DirectX::XMFLOAT4X4> prevViewVp_; // views_ (plan S9e)
    // render::g_localShadowWindRange: each view's rigid flag last frame -- a change redraws the view once, so no page keeps a
    // swayed pose under a rigid light (or the reverse). views_, follows the remap like prevViewVp_.
    std::vector<std::uint8_t> prevWindRigid_;
    bool prevViewVpValid_ = false;
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
    // Plan S10e: the alloc counters of each frame slot (16 bytes, persistently mapped), the LOD bias that frame used
    // (UE's LastGlobalResolutionLodBias comes back with its counters the same way), and the controller's state.
    Microsoft::WRL::ComPtr<ID3D12Resource> countersReadback_[render::kFrameCount];
    const std::uint32_t* countersReadbackPtr_[render::kFrameCount] = {};
    bool countersReadbackValid_[render::kFrameCount] = {};
    float countersLodBias_[render::kFrameCount] = {};
    std::uint64_t lastFrameOverBudget_ = 0;
    bool poolOverflowLogged_ = false;
    DynamicResStats dynamicRes_{};
    // Plan S10c: the lights the pages are for (applied) and this frame's (next); per OLD view the remap the GPU pass
    // applies (keep / drop / new view); whether the alloc pass runs it this frame, and whether the dirty test's
    // prevViewVp_ still has to follow it (the render pass's builder applies that).
    // Plan S9e: every per-view vector here is views_ long (EnsureResources sizes them, ReleaseResources empties them).
    ViewOwners appliedViewOwners_;
    ViewOwners nextViewOwners_;
    std::vector<std::uint32_t> viewRemap_;
    bool remapPending_ = false;
    bool prevVpRemapPending_ = false;
    // Per view: the light whose pages it parks (kNoViewOwner = none) and since which frame; parkedStale_ = a render since
    // could have changed them unseen (PrepareRenderPass), so the next plan frees every parked view.
    ViewOwners parkedOwners_;
    std::vector<std::uint64_t> parkedSince_;
    std::uint32_t parkedCount_ = 0;
    bool parkedStale_ = false;
    RemapStats remapStats_{};
    // Plan S10d: this frame's distant views / scheduled views (PlanDistantLights -> PrepareRenderPass), and per distant
    // light (spot owner, or a point's owner without its face bits) the frame it last redrew.
    std::vector<std::uint32_t> distantMask_;   // bit per view, (views_ + 31) / 32 words; empty = none this frame
    std::vector<std::uint32_t> distantUpdate_;
    struct DistantLight { std::uint32_t key = 0; std::uint64_t lastUpdate = 0; bool scheduled = false; };
    std::vector<DistantLight> distantLights_;
    std::vector<DistantLight> distantScratch_;
    DistantStats distantStats_{};
    std::shared_ptr<Material> allocRemapMat_;        // vsm_page_alloc_remap_cs.hlsl (S10c)

    // Step 19/20 validation: deferred readback of the request bitfield + alloc counters, re-armed
    // periodically so a live/stress run samples several times. The single readback buffer holds
    // [request words | alloc counters].
    static constexpr std::uint64_t kRequestReadbackPeriod = 30; // frames between samples (dev-window live-ish)
    static constexpr std::uint64_t kDbwinLogPeriod = 180;       // throttle the session-log mirror (vsm::g_logPageStats) separately
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
