#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "core/math/AABB.h"
#include "core/math/Math.h"

namespace render
{
// Runtime kill-switch for mesh LOD (default on). When off, Mesh::SelectLod ignores the
// requested tier and draws full detail — useful for A/B debugging and before/after measurement.
inline bool g_lodEnabled = true;

// Debug override: when >= 0, forces this LOD level on EVERY mesh draw (clamped to available),
// ignoring per-object screen-size / cascade selection. -1 = automatic selection. Lets you
// inspect each level. (No effect when g_lodEnabled is false — that forces full detail.)
inline int g_forcedLod = -1;

// Maximum distinct shadow LODs the GPU-driven caster path plumbs per mesh (mega slices +
// per-(group,lod) table depth). Meshes with fewer LODs clamp to their coarsest. The shaders get it as
// TC_MAX_SHADOW_LODS (Material.cpp EngineShaderDefines, plan S10a).
inline constexpr unsigned int kMaxShadowLods = 4u;

// Shadow LOD bias: an ADDITIVE offset on top of the PER-VIEW base LOD the GPU-driven shadow casters
// rasterize at. Each shadow view (CSM cascade i / VSM clipmap level i / local light) gets a base LOD
// from its tier (near = fine, far = coarse); this bias shifts the whole curve. Shadow maps don't
// resolve fine geometry, so coarser caster LODs cut VsmPageRender / cascade triangles for free.
// Default 1 (the near tier already coarsens to LOD 1 — measured visually free); positive = coarser
// everywhere, negative = sharper. Changing it needs a GPU-idle caster rebuild (Scene polls
// BuiltShadowLod() vs this each frame).
inline int g_shadowLodBias = 1;

// Number of consecutive shadow-view tiers that share one caster mesh LOD. A tier is a CSM
// cascade or a VSM clipmap level. 1 preserves the original aggressive 1:1 mapping; 2 produces
// 0,0,1,1,2,2,... and avoids changing caster geometry at every clipmap boundary. The additive
// g_shadowLodBias is applied after this curve. Changing either knob rebuilds the caster tables.
// Whether g_shadowLodBias also applies to the NEAREST shadow tier (clipmap level 0, and local
// lights, which are pinned there). Default OFF: the bias is a shift of the TIER curve, and at
// tier 0 there is no distance to hide a coarser caster behind, so it lands as self-shadow blobs
// on thin shells. See the note at the assignment in ShadowGpuData.cpp.
// --set=vsm.shadowLodBiasNearTier
inline bool g_shadowLodBiasNearTier = false;

inline int g_shadowLodTierStride = 2;

inline int ShadowLodTierStride()
{
    return std::clamp(g_shadowLodTierStride, 1, 8);
}

// --- Chunked-terrain LOD (mesh.json "chunkGrid": spatial submesh tiles of one surface) ----------
//
// A chunked mesh is LODed PER CHUNK, from the camera, and the SAME per-chunk tier drives BOTH the
// raster draw and the shadow casters (the VSM setup CB carries a per-group override; the Legacy
// per-view loop reads the same array). Caster == receiver geometry BY CONSTRUCTION, which is what
// kills the terrain self-shadow family for good: any caster LOD above the drawn one multiplies its
// height error by 1/sin(sunElevation) along the light — at a 2.8-degree sun a 10-30 cm LOD
// deviation became 2-6 METRES of depth error (phantom shadow blobs on empty beach, verified
// against a Legacy ground truth 2026-08-21). The per-view shadow LOD curve above still applies to
// everything non-chunked (palms).
//
// The knobs: a chunk closer than g_chunkLodDist0 (metres, closest point of its world AABB) draws
// LOD0; each g_chunkLodDistFactor further steps one LOD coarser. +/-15% hysteresis on the tier a
// chunk already has, so a boundary chunk does not flip while the camera breathes.
inline float g_chunkLodDist0 = 24.0f;
inline float g_chunkLodDistFactor = 2.0f;

inline unsigned int SelectChunkLodTier(float distMeters, unsigned int currentTier)
{
    const float d0 = g_chunkLodDist0 > 1.0f ? g_chunkLodDist0 : 1.0f;
    const float f = g_chunkLodDistFactor > 1.01f ? g_chunkLodDistFactor : 1.01f;
    constexpr float kHyst = 0.15f;
    constexpr unsigned int cap = kMaxShadowLods - 1u;
    unsigned int t = currentTier > cap ? cap : currentTier;
    auto bound = [&](unsigned int tier) { float b = d0; for (unsigned int i = 0; i < tier; ++i) { b *= f; } return b; };
    while (t < cap && distMeters > bound(t) * (1.0f + kHyst)) { ++t; }          // go coarser
    while (t > 0u && distMeters < bound(t - 1u) * (1.0f - kHyst)) { --t; }      // go finer
    return t;
}

// --- Caster-vs-receiver LOD contract -------------------------------------------------------------
//
// Unreal's rule, from ShadowSetup.cpp FProjectedShadowInfo::CalcAndUpdateLODToRender: the shadow
// depth pass STARTS from `CurrentView.PrimitivesLODMask[PrimitiveId]` -- the LOD the main view
// already chose for that primitive -- and only recomputes when a LOD is forced or
// r.Shadow.LODDistanceFactor is set. Its two deliberate divergences (preshadows take FMath::Max over
// the LOD indices, far cascades add GFarShadowStaticMeshLODBias) both go COARSER. A caster is never
// FINER than its receiver.
//
// This engine broke that for non-chunked meshes: ShadowGpuData picks the caster LOD purely per SHADOW
// VIEW (ShadowTierBaseLod(tier) + g_shadowLodBias), knowing nothing about the camera LOD. A sphere
// drawn at LOD2 while its caster rasterizes LOD1 self-shadows: LOD geometry is a vertex SUBSET, so
// the coarse receiver sits inside the finer caster hull and the depth test puts it behind its own
// shadow. That is the "black squares appear while I walk toward it, and raising the shadow LOD bias
// makes them go away" report -- raising the bias is a manual, global version of this rule.
//
// The transport is PER INSTANCE, like Unreal's: one byte per caster SLOT in
// ShadowGpuData::RefreshCasterLods' ring, consumed by vsm_page_scatter_cs, which buckets each
// instance into a VIRTUAL draw group (group * kMaxShadowLods + lod). A per-GROUP value was tried
// first (a FLOOR = "no finer than the coarsest receiver") and was wrong by construction: instances
// of one mesh sit at different receiver LODs in the same frame (demo.json's spheres span two LODs
// from any one camera), so any single per-group answer mismatches part of them — and BOTH mismatch
// directions self-shadow. A finer caster is a vertex SUPERSET whose hull the coarse receiver sits
// inside; a coarser caster pokes through a thin receiver (the tent). Only per-instance is exact.
//
//   bits 0..3  the LOD this caster's RECEIVER draws this frame
//   bit  7     EXACT (chunked terrain): that LOD on EVERY view, no per-view coarsening — a caster
//              coarser than the drawn ground multiplies its height error by 1/sin(sunElevation)
//              along the light (metres of depth error at a low sun; the phantom-shadow family)
//
// Without the EXACT bit the scatter takes max(receiverLod, view LOD): never finer than the
// receiver, coarser only where the view's tier curve already coarsens (which distance hides).
// Keep in sync with the decode in vsm_page_scatter_cs.hlsl.
inline constexpr unsigned int kCasterLodExactBit = 0x80u;
// Local-light shadow plan S7: set on the UPLOADED CasterLod only (ShadowGpuData::UploadCasterLods) -- the caster is
// movable for the local shadow cache. Every reader masks the LOD with kCasterLodMask; shadow_cull_cs.hlsl reads this.
inline constexpr unsigned int kCasterLocalDynamicBit = 0x100u;
// ...and this one when it is movable ONLY because it sways in the wind: static for a local view whose light is past
// render::g_localShadowWindRange (that view draws it rigid), movable for the others. Never set with the bit above.
inline constexpr unsigned int kCasterLocalWindBit = 0x200u;
inline constexpr unsigned int kCasterLodMask = 0x0Fu;

// The LOD a draw of `selectedTier` will ACTUALLY use once the two global debug overrides are
// applied. This is the mirror of Mesh::ResolveRuntimeLod, and the caster floor has to be computed
// from it rather than from the raw selected tier -- otherwise "Force LOD level" moves the receiver
// and leaves the caster where it was, which is the exact mismatch the floor exists to prevent, now
// produced by the debug control meant to inspect it.
inline unsigned int EffectiveDrawLod(unsigned int selectedTier)
{
    if (!g_lodEnabled) { return 0u; }
    if (g_forcedLod >= 0) { return static_cast<unsigned int>(g_forcedLod); }
    return selectedTier;
}

// Per-view BASE shadow LOD from a view's tier. g_shadowLodTierStride controls how many consecutive
// tiers share one caster LOD (1 = the original 1:1 curve, 2 = 0,0,1,1,...).
// `tier` is the CSM cascade index or the VSM clipmap level (0 = finest/near); locals pass a small
// fixed tier. The final LOD adds g_shadowLodBias (default 1, which is where the whole curve's
// coarsening lives) and is clamped per mesh to its available LODs by the caster tables.
inline int ShadowTierBaseLod(unsigned int tier)
{
    const int lod = static_cast<int>(tier) / ShadowLodTierStride();
    const int cap = static_cast<int>(kMaxShadowLods) - 1;
    return lod < cap ? lod : cap;
}

// Step 6 boundaries, now TUNABLE (dev window "LOD" tab; --set=lod.bound0/1/2) -- the DISTANCE/RADIUS
// method, used when g_lodUeScreenSize (below) is off. The unit is
// distance / instance RADIUS — object-size-relative on purpose, so a palm switches later than a
// pebble at the same distance. In UE's terms (ComputeBoundsScreenSize: the bounding sphere's diameter
// over the screen height) a ratio r is a screen size of aspect / (r * tan(hfov/2)) = 1.778 / r at the
// reference 90 degrees on 16:9. 2026-10-23 (owner: "a bit more aggressive"): 10/20/40 -> 8/16/32,
// i.e. screen size 0.18/0.09/0.044 -> 0.22/0.11/0.056 -- the same as UE's r.StaticMeshLODDistanceScale
// 1.25, and still on the detailed side of UE's usual hand-authored 0.5/0.25/0.125 (ratio 3.6/7/14).
// Measured on wind_test (level camera, interleaved x3): frame -0.03 ms.
inline float g_lodBound0 = 8.0f;  // ratio where LOD0 -> 1
inline float g_lodBound1 = 16.0f; // ratio where LOD1 -> 2
inline float g_lodBound2 = 32.0f; // ratio where LOD2 -> 3

// --- FOV normalization ---------------------------------------------------------------------------
//
// The bounds above are a distance/radius ratio, which is a screen size ONLY at a fixed field of
// view: zoom in and every object doubles on screen while its ratio does not move, so the LOD stays
// where a much smaller object would have wanted it. Unreal folds the projection into the metric
// (SceneManagement.cpp ComputeBoundsScreenSize: ScreenMultiple = max(0.5*P[0][0], 0.5*P[1][1]),
// ScreenRadius = ScreenMultiple * SphereRadius / max(1, Dist)).
//
// Taken NORMALIZED to a reference FOV on purpose: the factor is refMultiple/currentMultiple, and
// since both share the aspect ratio that reduces exactly to tan(hfov/2) / tan(refHfov/2). So the
// numbers stay tuned at the reference FOV (the factor is exactly 1 there), a window resize does not
// move any LOD, and only an actual FOV change does. Absolute screen sizes would have re-tuned every
// bound the moment this shipped.
inline float g_lodRefHFovDeg = 90.0f;  // the camera's own default hfov; see Camera::view_.hfov
inline float g_lodFovRatioScale = 1.0f; // refreshed once per frame from the camera, before selection

inline void UpdateLodFovScale(float hfovRadians)
{
    const float ref = g_lodRefHFovDeg * 3.14159265f / 180.0f;
    const float a = std::tan(std::clamp(hfovRadians, 0.01f, 3.0f) * 0.5f);
    const float b = std::tan(std::clamp(ref, 0.01f, 3.0f) * 0.5f);
    g_lodFovRatioScale = (b > 1e-6f) ? (a / b) : 1.0f;
}

// --- UE's static-mesh LOD: selection AND estimation (2026-10-23, owner: "do it as UE does") --------
//
// SELECTION, ComputeLODForMeshes / ComputeStaticMeshLOD (D:\zalupa\Engine\Source\Runtime\Engine\
// Private\SceneManagement.cpp:1028-1050, 1138-1161). The object's screen size is its bounding sphere's
// projected DIAMETER,
//     SS = 2 * max(0.5 * P[0][0], 0.5 * P[1][1]) * R / max(1, D)        (ComputeBoundsScreenSize, :966)
// and the LOD drawn is the COARSEST one whose ScreenSize[i] * LODScale is still >= SS, walking down from
// the last LOD; LOD0 when none is. LODScale = r.StaticMeshLODDistanceScale * View.LODDistanceFactor
// (Renderer/Private/SceneVisibility.cpp:1250) -- above 1 switches EARLIER. UE has no hysteresis; its
// transition is the temporal dither below, which both methods use.
//
// ESTIMATION, FStaticMeshRenderData::ResolveSectionInfo with bAutoComputeLODScreenSize
// (Engine/Private/StaticMesh.cpp:3105-3142). LOD0 = 2.0; LOD i = the screen size at which that level's
// MaxDeviation projects to PixelError pixels on a 1920x1080 screen at 90 degrees:
//     ViewDistance = MaxDeviation * 960 / PixelError                     (CalculateViewDistance, :3064)
//     ScreenSize   = ComputeBoundsScreenSize(0, R, view at ViewDistance + R, 90 deg, 1920 x 1080)
//                  = 1.778 * R / (ViewDistance + R)
// A level with no measured deviation gets 0.75^i, and no level may ask for a LARGER screen size than the
// one before it (it then takes half of that). PixelError defaults to 8 (FMeshReductionSettings,
// MeshReductionSettings.cpp:13). Mesh::GetLodScreenSizes is this.
//
// The deviation is the bake's (Mesh::GetLodError: meshopt's error in object units, the worst range). UE's
// MaxDeviation is `sqrt(quadric error) / 8` of its OWN simplifier (Developer/MeshSimplifier/Private/
// QuadricMeshReduction.cpp:355) -- a calibration of that metric to a distance; meshopt's already is one,
// so it goes in as it is.
//
// Per asset, as in UE: mesh.json "lodScreenSize" (the authored ScreenSize of each LOD, LOD0 first, i.e.
// bAutoComputeLODScreenSize off) and "lodPixelError" (the reduction's PixelError); "lodDistanceScale"
// still multiplies the selection radius (UE's per-component FactorScale).
//
// History worth keeping: an error-derived per-asset DISTANCE scale was tried in August and left off,
// because measured over the bake it holds palms (deviation/radius at LOD1 0.0535: a lot of error, spread
// over noisy fronds) finer for longer than spheres (0.0285, a silhouette the eye tracks) and rocks /
// tent / teapot (0.003-0.005) the coarsest. UE's rule has exactly that shape -- it is what UE ships and
// what the owner asked for; an asset the eye disagrees with takes an authored "lodScreenSize".
// FOLIAGE is not estimated at all (RenderableObject::LodSelectionBounds): UE's Foliage LOD group
// generates no LODs, and our foliage chain removes whole parts no recorded deviation sees.
//
// Both methods feed the SAME selector below as per-object distance/radius BOUNDS:
//     SS <= T  <=>  D / R >= 2 * M / T,   M = max(0.5 P[0][0], 0.5 P[1][1]).
//
// THE SCREEN IS THE ONE BEING LOOKED AT (2026-10-24). UE fixes the projection of that estimate at 1920x1080 / 90
// degrees (the 960 above) and PixelError at 8: eight pixels of a 1080p screen, a visible pop by design, and calibrated
// against UE's own deviation metric, which reads small. Ours is meshopt's distance, and at 8 the_bathroom_free's props
// switched in arm's reach -- LOD1 from a median 1.6 m, 36 of its 100 meshes inside 1 m, the candles at 0.37 m, where
// their 0.9 mm is 3 pixels of a 1440p screen (owner: "the LODs in the room still switch very close"). So the
// deviation is projected through THIS frame's camera onto THIS screen -- pixels per unit at depth 1 =
// 0.5 * output width * P[0][0] (g_lodPixelsPerUnit: 1280 at 2560 wide and 90 degrees, UE's 960 at 1920) -- the
// screen size is taken with this frame's multiple too, so the switch lands at exactly ViewDistance + R whatever
// the field of view, and lod.pixelError is in those pixels, default 1: a LOD comes in where its worst deviation
// spans one pixel of the screen it is seen on. The OUTPUT width, not DLSS's input: a pop is seen on the output.
inline bool g_lodUeScreenSize = true;       // false = the global distance/radius bounds g_lodBound0..2
inline float g_lodPixelError = 1.0f;        // pixels of THIS screen (UE's PixelError: 1080p pixels, 8)
inline float g_lodDistanceScale = 1.0f;     // UE r.StaticMeshLODDistanceScale
inline float g_lodScreenMultiple = 0.8889f; // max(0.5 P[0][0], 0.5 P[1][1]) of this frame's camera
inline float g_lodPixelsPerUnit = 960.0f;   // 0.5 * output width * P[0][0] of this frame (UE's fixed 960)
inline constexpr unsigned int kLodScreenSizes = 4u; // LOD0..3, the most any mesh bakes

// Once per frame from the camera's projection (unjittered), before selection -- UpdateLodFovScale's
// sibling.
inline float g_lodOutputWidthPx = 1920.0f; // the output (display) width, Scene::PrepareViews each frame
inline void UpdateLodScreenMultiple(float p00, float p11)
{
    const float m = 0.5f * std::max(std::abs(p00), std::abs(p11));
    g_lodScreenMultiple = m > 1e-6f ? m : 0.8889f;
    const float ppu = 0.5f * g_lodOutputWidthPx * std::abs(p00);
    g_lodPixelsPerUnit = ppu > 1.0f ? ppu : 960.0f;
}

// The distance / radius at which tier t steps to t+1, for the selector below. A level that is never
// reached keeps its bound at "infinity".
struct LodBounds
{
    float b[3] = { 1e30f, 1e30f, 1e30f };
};

// The distance/radius method: the global bounds, with the FOV scale divided out of the bound instead
// of multiplied into the ratio (the same test).
inline LodBounds RatioLodBounds()
{
    const float s = g_lodFovRatioScale > 1e-6f ? g_lodFovRatioScale : 1.0f;
    LodBounds out;
    out.b[0] = g_lodBound0 / s;
    out.b[1] = g_lodBound1 / s;
    out.b[2] = g_lodBound2 / s;
    return out;
}

// UE's walk as bounds: at least LOD k+1 once SS <= ScreenSize[k+1] * LODScale, i.e. once
// D / R >= 2M / (ScreenSize[k+1] * LODScale). A level the mesh lacks, or one whose screen size is 0,
// is never reached.
inline LodBounds ScreenSizeLodBounds(const float (&screenSize)[kLodScreenSizes], unsigned int lodCount)
{
    LodBounds out;
    const float scale = g_lodDistanceScale > 1e-3f ? g_lodDistanceScale : 1e-3f;
    for (unsigned int k = 0; k < 3u; ++k)
    {
        const float t = screenSize[k + 1u] * scale;
        if (k + 1u < lodCount && t > 0.0f) { out.b[k] = 2.0f * g_lodScreenMultiple / t; }
    }
    return out;
}

// UE's auto screen sizes for one mesh: `deviation[i]` the level's MaxDeviation in the units of
// `sphereRadius` (0 = not measured; deviation[0] is ignored), `count` levels. See the block above.
inline void AutoLodScreenSizes(const float* deviation, unsigned int count, float sphereRadius, float pixelError,
                               float (&out)[kLodScreenSizes])
{
    constexpr float kLod0ScreenSize = 2.0f;   // UE::StaticMesh::Private::Constants::LOD0ScreenSize
    constexpr float kPowerBase = 0.75f;       // ...::AutoComputeLODPowerBase
    const float pe = pixelError > 1e-3f ? pixelError : 1e-3f;
    for (unsigned int i = 0; i < kLodScreenSizes; ++i)
    {
        float ss = 0.0f;
        if (i == 0u) { ss = kLod0ScreenSize; }
        else if (i < count)
        {
            if (deviation[i] <= 0.0f || sphereRadius <= 1e-6f) { ss = std::pow(kPowerBase, static_cast<float>(i)); }
            else
            {
                // This screen's pixels and this frame's multiple (see the block above): switches at viewDistance + R.
                const float viewDistance = deviation[i] * g_lodPixelsPerUnit / pe;
                ss = 2.0f * g_lodScreenMultiple * sphereRadius / std::max(viewDistance + sphereRadius, 1e-2f);
            }
            if (ss > out[i - 1u]) { ss = out[i - 1u] * 0.5f; } // screen-size coherence between LODs
        }
        out[i] = ss;
    }
}

// --- TEMPORAL LOD dither: UE's FTemporalLODState (SceneManagement.cpp:93-138), 2026-10-23 --------
//
// The owner: "the dither between LODs should be temporal, like UE's, not spatial". It used to be a
// SPATIAL band -- every object within +/-10 % of a boundary's distance drew both tiers, permanently,
// with the mask weight a function of distance. UE does it in TIME: the view keeps two camera origins
// lod.TemporalLag (0.5 s) apart, selects the LOD from each, and when they disagree draws BOTH with
// complementary screen-door masks while the weight runs 0 -> 1 over the lag:
//     every lag, origin[0] = origin[1] (with its time) and origin[1] = the camera now;
//     alpha = clamp((now - lag - time[0]) / (time[1] - time[0]), 0, 1).
// So a LOD change DISSOLVES over ~0.5 s and a standing camera dithers nothing at all; selection is
// stateless (no hysteresis -- UE has none; the lagged origins are what keep a boundary still). A
// camera cut (the camera's history revision) or a stall restarts the state on the current origin,
// UE's bOk = false. lag 0 = hard switches with the old +/-15 % hysteresis.
//
// The masks are the same 4x4 Bayer screen door (LodFadeClip, gbuffer_common.hlsli) the band used,
// resolved by DLSS/TAA. Measured with the band 2026-08-25 and still true for any dither: the shadow
// map holds ONE of the two surfaces, so under VSM's local-light shadows the other can self-shadow
// into the mask's dots -- the band made that permanent on every object near a boundary; in time it
// lasts the half second of the dissolve.
inline float g_lodTemporalLag = 0.5f; // UE lod.TemporalLag, seconds; 0 = no dither (hard switches)

struct TemporalLodState
{
    Math::float3 origin[2]{};
    double time[2] = { 0.0, 0.0 };
    float lag = 0.0f;
    float alpha = 0.0f;            // this frame's handover from origin[0]'s LOD to origin[1]'s
    std::uint64_t revision = ~0ull; // the camera history the samples belong to (a cut restarts them)
    Math::float3 current{};        // the camera origin the state was last updated with
};
inline TemporalLodState g_lodTemporal;

// Once per frame from the main camera, before selection (SceneRenderQueue::SelectLods). Calling it
// again within the same frame changes nothing.
inline void UpdateTemporalLod(const Math::float3& viewOrigin, double nowSeconds, std::uint64_t cameraRevision)
{
    TemporalLodState& s = g_lodTemporal;
    bool ok = g_lodTemporalLag > 0.0f && cameraRevision == s.revision;
    if (ok)
    {
        s.lag = g_lodTemporalLag;
        if (s.time[1] < nowSeconds - s.lag)
        {
            if (s.time[0] < s.time[1])
            {
                s.origin[0] = s.origin[1];
                s.time[0] = s.time[1];
            }
            s.origin[1] = viewOrigin;
            s.time[1] = nowSeconds;
            if (s.time[1] <= s.time[0]) { ok = false; } // paused, or no good sample
        }
    }
    if (!ok)
    {
        s.origin[0] = s.origin[1] = viewOrigin;
        s.time[0] = s.time[1] = nowSeconds;
        s.lag = 0.0f;
    }
    s.revision = cameraRevision;
    s.current = viewOrigin;
    const double span = s.time[1] - s.time[0];
    s.alpha = (s.lag > 0.0f && span > 1e-6)
        ? static_cast<float>(std::clamp((nowSeconds - s.lag - s.time[0]) / span, 0.0, 1.0))
        : 0.0f;
}

// Tier + crossfade state for one object. fade == 0: draw `tier` solid. fade in (0,1): `tier`
// is fading OUT (weight 1-fade) and `tier+1` is fading IN (weight fade) — the caller issues
// both draws, clamping tier+1 to the mesh's available LODs. With the temporal dither `fade` is the
// weight of the COARSER of the pair whichever way the handover runs.
struct LodTierFade
{
    unsigned int tier = 0u;
    float fade = 0.0f;
};

// Step 6: pick a LOD tier (0 = full) from screen size (distance / instance radius against the
// object's bounds -- RatioLodBounds or ScreenSizeLodBounds, which carry the field of view), with
// HYSTERESIS off the current tier so it doesn't flip back and forth near a boundary. Called
// once per frame per object in Scene::PrepareViews (NOT during recording); the result is
// stored and read at draw time. `radius` must be a single drawn instance's size, not an
// aggregate (cloud/run) bound. Mesh::SelectLod clamps to the LODs actually available.
inline unsigned int SelectLodTier(const Math::float3& center, float radius,
                                  const Math::float3& camPos, unsigned int currentTier, const LodBounds& lb)
{
    if (radius <= 1e-4f) { return 0u; }
    const float ratio = (center - camPos).Length() / radius;

    // Monotonic enforcement (each bound at least 5% past the previous) keeps the hysteresis bands
    // from overlapping however the three sliders are dragged (or a mesh's screen sizes authored).
    float bound[3];
    bound[0] = lb.b[0] > 0.5f ? lb.b[0] : 0.5f;
    bound[1] = lb.b[1] > bound[0] * 1.05f ? lb.b[1] : bound[0] * 1.05f;
    bound[2] = lb.b[2] > bound[1] * 1.05f ? lb.b[2] : bound[1] * 1.05f;
    constexpr float kHyst = 0.15f;                       // +/-15% dead band around each boundary
    unsigned int t = currentTier > 3u ? 3u : currentTier;
    while (t < 3u && ratio > bound[t] * (1.0f + kHyst)) { ++t; }           // go coarser
    while (t > 0u && ratio < bound[t - 1u] * (1.0f - kHyst)) { --t; }      // go finer
    return t;
}

// The tier at one origin with no hysteresis -- UE's walk, used for each temporal sample.
inline unsigned int SelectLodTierAt(const Math::float3& center, float radius,
                                    const Math::float3& origin, const LodBounds& lb)
{
    if (radius <= 1e-4f) { return 0u; }
    const float ratio = (center - origin).Length() / radius;
    float bound[3];
    bound[0] = lb.b[0] > 0.5f ? lb.b[0] : 0.5f;
    bound[1] = lb.b[1] > bound[0] * 1.05f ? lb.b[1] : bound[0] * 1.05f;
    bound[2] = lb.b[2] > bound[1] * 1.05f ? lb.b[2] : bound[1] * 1.05f;
    unsigned int t = 0u;
    while (t < 3u && ratio > bound[t]) { ++t; }
    return t;
}

// The camera pass's selection: UE's temporal LOD. The two lagged origins belong to the main camera;
// any other camera (a preview, a thumbnail) selects at its own position with no dither. With the
// lag at 0 this is SelectLodTier and its hysteresis.
inline LodTierFade SelectLodTierTemporal(const Math::float3& center, float radius,
                                         const Math::float3& camPos, unsigned int currentTier, const LodBounds& lb)
{
    if (g_lodTemporalLag <= 0.0f)
    {
        return { SelectLodTier(center, radius, camPos, currentTier, lb), 0.0f };
    }
    const TemporalLodState& s = g_lodTemporal;
    const Math::float3 d = camPos - s.current;
    if (d.x * d.x + d.y * d.y + d.z * d.z > 1e-6f)
    {
        return { SelectLodTierAt(center, radius, camPos, lb), 0.0f };
    }
    const unsigned int a = SelectLodTierAt(center, radius, s.origin[0], lb); // outgoing
    const unsigned int b = SelectLodTierAt(center, radius, s.origin[1], lb); // incoming
    if (a == b || s.alpha >= 1.0f) { return { b, 0.0f }; }
    if (s.alpha <= 0.0f) { return { a, 0.0f }; }
    if (a + 1u == b) { return { a, s.alpha }; }        // going coarser: b (= a + 1) fades in
    if (b + 1u == a) { return { b, 1.0f - s.alpha }; } // going finer: a (= b + 1) fades out
    // Two tiers apart (the camera moved a whole tier's worth within one lag): the masks pair a tier
    // with the next one only, so take the nearer end of the handover instead of dithering.
    return { s.alpha < 0.5f ? a : b, 0.0f };
}
} // namespace render
