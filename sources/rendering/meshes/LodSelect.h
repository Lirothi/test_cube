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

// Step 6 boundaries, now TUNABLE (dev window "LOD" tab; --set=lod.bound0/1/2). The unit is
// distance / instance RADIUS — object-size-relative on purpose, so a palm switches later than a
// pebble at the same distance. Defaults = the original deliberately conservative curve.
inline float g_lodBound0 = 5.0f; // ratio where LOD0 -> 1
inline float g_lodBound1 = 10.0f; // ratio where LOD1 -> 2
inline float g_lodBound2 = 20.0f; // ratio where LOD2 -> 3

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
} // namespace render
