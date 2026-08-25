#pragma once
#include <algorithm>
#include <cmath>
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
// per-(group,lod) table depth). Meshes with fewer LODs clamp to their coarsest. Keep in sync with
// KMAX_SHADOW_LODS in vsm_page_setup_cs.hlsl.
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

// Step 6 boundaries, now TUNABLE (dev window "LOD" tab; --set=lod.bound0/1/2). The unit is
// distance / instance RADIUS — object-size-relative on purpose, so a palm switches later than a
// pebble at the same distance. Defaults = the original deliberately conservative curve.
inline float g_lodBound0 = 10.0f; // ratio where LOD0 -> 1
inline float g_lodBound1 = 20.0f; // ratio where LOD1 -> 2
inline float g_lodBound2 = 40.0f; // ratio where LOD2 -> 3

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
// Reference deviation/radius that maps to an auto scale of 1.0 (Mesh::GetLodAutoDistanceScale).
// The median over this project baked meshes.
inline constexpr float kLodAutoErrorRef = 0.00422f;
//
// DEFAULT OFF, and the measurement is why. Deriving a switch distance from the baked geometric
// error is an appealing idea -- a deviation projects to pixels, and pixels have an obviously
// right threshold -- but measured over the v2 bake it ORDERS THE ASSETS WRONG. deviation/radius
// at LOD1: date_palm 0.0535, curly_palm 0.0314, sphere 0.0285, coconut_palm 0.0099, rocks and
// teapot and tent 0.003-0.005, atoll_island 0.00016. So the palms deform MORE for their size
// than the sphere does, and this rule would hold palms fine longer than spheres -- the opposite
// of what the eye asks for.
//
// Geometric error is not perceptual salience. A sphere errs by little but STRUCTURALLY: a smooth
// silhouette becomes a polygon and the eye tracks that instantly. A palm errs by a lot, spread
// over noisy fronds where nothing is trackable. This is exactly why Unreal AUTHORS ScreenSize per
// LOD per mesh instead of deriving it -- a deliberate choice, not an omission.
//
// The baked errors are kept and surfaced (Mesh::GetLodError, the Mesh Editor, the LOD debug
// probe) so per-asset lodDistanceScale is set from evidence. Turn this on only with a rule that
// survives its own measurement. --set=lod.autoScale
inline bool g_lodAutoScaleFromError = false;

inline float g_lodRefHFovDeg = 90.0f;  // the camera's own default hfov; see Camera::view_.hfov
inline float g_lodFovRatioScale = 1.0f; // refreshed once per frame from the camera, before selection

inline void UpdateLodFovScale(float hfovRadians)
{
    const float ref = g_lodRefHFovDeg * 3.14159265f / 180.0f;
    const float a = std::tan(std::clamp(hfovRadians, 0.01f, 3.0f) * 0.5f);
    const float b = std::tan(std::clamp(ref, 0.01f, 3.0f) * 0.5f);
    g_lodFovRatioScale = (b > 1e-6f) ? (a / b) : 1.0f;
}

// Dithered LOD crossfade: half-width of the transition band around each boundary, as a
// fraction of the boundary ratio (0 = off -> hard switches with the classic hysteresis).
// Inside the band BOTH tiers draw with complementary screen-door masks (see LodFadeClip in
// gbuffer_common.hlsli), so the switch is a gradual pixel handover instead of a pop; DLSS/TAA
// resolves the 4x4 Bayer pattern into a smooth blend. Selection inside the band is STATELESS
// (fade is a pure function of distance), which is also why the band needs no hysteresis: the
// blend is continuous in distance, so there is nothing to flip-flop.
// Default OFF (user decision 2026-08-25): the band costs a SECOND draw of every object crossing it,
// and the pop it hides stopped being the thing you notice once the LOD normal drift was fixed
// (see MeshLoadOptions::lodNormalWeight). Raise it to bring the dithered handover back.
//
// KNOWN INTERACTION if you do, measured on demo.json 2026-08-25: the band draws the object TWICE,
// as two geometrically DIFFERENT surfaces, but the shadow map holds only one of them (this is a
// camera-pass-only effect). Under VSM's local spot/point shadows -- whose bias is a tight,
// texel-sized world push -- the tier the shadow map never saw lands on the wrong side of that bias
// and self-shadows, so the screen-door mask itself becomes visible as squares of dark dots on the
// lit side of every fading object. Legacy CSM shows the same scene clean: it shadows the same local
// lights, but its softer/coarser lookup keeps the tier displacement inside the bias. Proven by
// same-binary toggles: lod.fadeBand:0 removes it, lod.enabled:0 removes it, and
// vsm.shadowLodBias:0 does NOT (it makes it slightly worse) -- so this is the crossfade, not a
// caster-vs-receiver LOD mismatch.
inline float g_lodFadeBand = 0.10f;

// Tier + crossfade state for one object. fade == 0: draw `tier` solid. fade in (0,1): `tier`
// is fading OUT (weight 1-fade) and `tier+1` is fading IN (weight fade) — the caller issues
// both draws, clamping tier+1 to the mesh's available LODs.
struct LodTierFade
{
    unsigned int tier = 0u;
    float fade = 0.0f;
};

// Step 6: pick a LOD tier (0 = full) from screen size (distance / instance radius), with
// HYSTERESIS off the current tier so it doesn't flip back and forth near a boundary. Called
// once per frame per object in Scene::PrepareViews (NOT during recording); the result is
// stored and read at draw time. `radius` must be a single drawn instance's size, not an
// aggregate (cloud/run) bound. Mesh::SelectLod clamps to the LODs actually available.
inline unsigned int SelectLodTier(const Math::float3& center, float radius,
                                  const Math::float3& camPos, unsigned int currentTier)
{
    if (radius <= 1e-4f) { return 0u; }
    // g_lodFovRatioScale folds the field of view in (see UpdateLodFovScale): 1 at the reference
    // FOV, below 1 when zoomed in, which pulls every boundary nearer in ratio terms = finer LOD.
    const float ratio = (center - camPos).Length() / radius * g_lodFovRatioScale;

    // Monotonic enforcement (each bound at least 5% past the previous) keeps the hysteresis bands
    // from overlapping however the three sliders are dragged.
    float bound[3];
    bound[0] = g_lodBound0 > 0.5f ? g_lodBound0 : 0.5f;
    bound[1] = g_lodBound1 > bound[0] * 1.05f ? g_lodBound1 : bound[0] * 1.05f;
    bound[2] = g_lodBound2 > bound[1] * 1.05f ? g_lodBound2 : bound[1] * 1.05f;
    constexpr float kHyst = 0.15f;                       // +/-15% dead band around each boundary
    unsigned int t = currentTier > 3u ? 3u : currentTier;
    while (t < 3u && ratio > bound[t] * (1.0f + kHyst)) { ++t; }           // go coarser
    while (t > 0u && ratio < bound[t - 1u] * (1.0f - kHyst)) { --t; }      // go finer
    return t;
}

// Crossfade-aware tier selection. With the band off this is exactly SelectLodTier; with it on
// the result is STATELESS (currentTier unused): outside every band the tier the ratio falls
// in, inside boundary t's band tier t with fade = position across the band (0 at the near
// edge, 1 at the far edge). Same monotonic bound enforcement as SelectLodTier.
inline LodTierFade SelectLodTierFade(const Math::float3& center, float radius,
                                     const Math::float3& camPos, unsigned int currentTier)
{
    if (g_lodFadeBand <= 0.0f)
    {
        return { SelectLodTier(center, radius, camPos, currentTier), 0.0f };
    }
    if (radius <= 1e-4f) { return { 0u, 0.0f }; }
    const float ratio = (center - camPos).Length() / radius * g_lodFovRatioScale;

    float bound[3];
    bound[0] = g_lodBound0 > 0.5f ? g_lodBound0 : 0.5f;
    bound[1] = g_lodBound1 > bound[0] * 1.05f ? g_lodBound1 : bound[0] * 1.05f;
    bound[2] = g_lodBound2 > bound[1] * 1.05f ? g_lodBound2 : bound[1] * 1.05f;
    // Clamp the half-width so adjacent bands can never overlap (bounds are >= 5% apart).
    const float w = std::min(g_lodFadeBand, 0.35f);

    unsigned int t = 0u;
    while (t < 3u && ratio > bound[t] * (1.0f + w)) { ++t; }
    if (t < 3u && ratio > bound[t] * (1.0f - w))
    {
        const float lo = bound[t] * (1.0f - w);
        const float span = bound[t] * (2.0f * w);
        return { t, span > 1e-6f ? (ratio - lo) / span : 1.0f };
    }
    return { t, 0.0f };
}
} // namespace render
