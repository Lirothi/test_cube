#pragma once
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

// Additional shadow-LOD bias applied ONLY to the caster groups of a chunked mesh (mesh.json
// "chunkGrid" — terrain split into spatial submesh chunks). Terrain is the receiver the camera
// rasterizes at LOD0, so ANY caster LOD above 0 makes the simplified surface shadow the detailed
// one. DEFAULT -3 = PIN TERRAIN CASTERS TO LOD0 AT EVERY LEVEL (with radius 0 below), and it is
// not a taste call: at a grazing sun the mismatch multiplies by 1/sin(elevation) along the light —
// at the 2.8-degree low sun a ~10-30 cm LOD3 height deviation became 2-6 METRES of depth error,
// which rendered as huge phantom shadow blobs on empty beach (2026-08-21, verified against a
// Legacy-CSM ground truth: LOD0-everywhere sits at the noise floor from it, no depth bias short of
// metres could mask it). Measured cost of LOD0-everywhere over the old "-1 within 12 m":
// Pass_VsmPageRender 0.716 -> 0.816 ms — chunking is what makes it this cheap (a page draws only
// its overlapping chunks). The per-view LOD curve still applies to everything non-chunked (palms).
// 0 disables (the A/B control; no-op while no chunked mesh exists). Same rebuild rule as
// g_shadowLodBias above: Scene polls BuiltChunkLodBias() and re-Rebuilds on a change.
inline int g_chunkShadowLodBias = -3;

// How far from the camera `g_chunkShadowLodBias` is allowed to act, in METRES. Directional shadow
// views are distance-ordered (CSM cascade index / VSM clipmap level), so this is a cutoff on that
// order: a view whose reach exceeds the radius keeps the plain per-view LOD.
//
// Without it the bias applies at EVERY distance, and the far end is where it is least worth paying:
// the per-view LOD saturates at the coarsest level, so a -1 there does not cancel the +1 default —
// it un-saturates a level the clamp had already collapsed, making the terrain finer out to the
// horizon for no visible gain. Local lights never take the bias at all: "distance from the camera"
// is not a property their views have.
//
// <= 0 means "no cutoff" — WHICH IS THE DEFAULT NOW: the phantom-blob finding above showed the
// far levels are exactly where the terrain LOD mismatch does the most damage at a low sun (the
// grazing multiplier grows with distance-level coarseness), so the bias must reach every level.
// The radius stays as the cost-recovery knob if a scene ever needs it. The mapping to views is
// vsm::ClipmapLevelsWithinRadius, and the RESULTING LEVEL COUNT is what the caster rebuild is keyed
// on — nudging the radius inside one level's bucket costs nothing.
inline float g_chunkShadowLodRadius = 0.0f;

// Per-view BASE shadow LOD from a view's tier: the tier index itself (near = fine, far = coarse).
// `tier` is the CSM cascade index or the VSM clipmap level (0 = finest/near); locals pass a small
// fixed tier. The final LOD adds g_shadowLodBias (default 1, which is where the whole curve's
// coarsening lives) and is clamped per mesh to its available LODs by the caster tables.
inline int ShadowTierBaseLod(unsigned int tier)
{
    // tier -> LOD 1:1, capped: level 0->0, 1->1, 2->2, 3+ -> kMaxShadowLods-1.
    const int lod = static_cast<int>(tier);
    const int cap = static_cast<int>(kMaxShadowLods) - 1;
    return lod < cap ? lod : cap;
}

// Step 6: pick a LOD tier (0 = full) from screen size (distance / instance radius), with
// HYSTERESIS off the current tier so it doesn't flip back and forth near a boundary. Called
// once per frame per object in Scene::PrepareViews (NOT during recording); the result is
// stored and read at draw time. `radius` must be a single drawn instance's size, not an
// aggregate (cloud/run) bound. Mesh::SelectLod clamps to the LODs actually available.
inline unsigned int SelectLodTier(const Math::float3& center, float radius,
                                  const Math::float3& camPos, unsigned int currentTier)
{
    if (radius <= 1e-4f) { return 0u; }
    const float ratio = (center - camPos).Length() / radius; // ~ inverse of projected screen size

    constexpr float kBound[3] = { 15.0f, 35.0f, 70.0f }; // tier boundaries (deliberately conservative)
    constexpr float kHyst = 0.15f;                       // +/-15% dead band around each boundary
    unsigned int t = currentTier > 3u ? 3u : currentTier;
    while (t < 3u && ratio > kBound[t] * (1.0f + kHyst)) { ++t; }          // go coarser
    while (t > 0u && ratio < kBound[t - 1u] * (1.0f - kHyst)) { --t; }     // go finer
    return t;
}
} // namespace render
