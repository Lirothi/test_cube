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

// Step 6c: pick a LOD tier for a renderable from its world bounds + the camera position.
// Returns a DESIRED tier (0 = full detail); Mesh::SelectLod clamps to the LODs actually
// available, so objects/meshes without LODs always draw full detail. Metric is
// distance / object-radius — a resolution-independent screen-size proxy. Thresholds are
// deliberately conservative so near/large objects stay crisp (LOD's win is in the shadow
// cascades, which use a separate cascade-index floor, not this).
inline unsigned int SelectLodTier(const Math::float3& center, float radius, const Math::float3& camPos)
{
    // radius must be the size of ONE drawn instance, not an aggregate (cloud/run) bound — a
    // cloud's bound radius is huge, which would keep instanced objects at LOD 0 forever.
    if (radius <= 1e-4f) { return 0u; }
    const float ratio = (center - camPos).Length() / radius; // ~ inverse of projected screen size
    if (ratio < 15.0f) { return 0u; }
    if (ratio < 35.0f) { return 1u; }
    if (ratio < 70.0f) { return 2u; }
    return 3u;
}

inline unsigned int SelectLodTier(const AABB& worldBounds, const Math::float3& camPos)
{
    if (!worldBounds.IsValid()) { return 0u; }
    return SelectLodTier(worldBounds.GetCenter(), worldBounds.GetRadius(), camPos);
}
} // namespace render
