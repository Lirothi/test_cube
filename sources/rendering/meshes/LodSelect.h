#pragma once
#include "core/math/AABB.h"
#include "core/math/Math.h"

namespace render
{
// Runtime kill-switch for mesh LOD (default on). When off, Mesh::SelectLod ignores the
// requested tier and draws full detail — useful for A/B debugging and before/after measurement.
inline bool g_lodEnabled = true;

// Step 6c: pick a LOD tier for a renderable from its world bounds + the camera position.
// Returns a DESIRED tier (0 = full detail); Mesh::SelectLod clamps to the LODs actually
// available, so objects/meshes without LODs always draw full detail. Metric is
// distance / object-radius — a resolution-independent screen-size proxy. Thresholds are
// deliberately conservative so near/large objects stay crisp (LOD's win is in the shadow
// cascades, which use a separate cascade-index floor, not this).
inline unsigned int SelectLodTier(const AABB& worldBounds, const Math::float3& camPos)
{
    if (!worldBounds.IsValid()) { return 0u; }
    const float radius = worldBounds.GetRadius();
    if (radius <= 1e-4f) { return 0u; }

    const Math::float3 delta = worldBounds.GetCenter() - camPos;
    const float ratio = delta.Length() / radius; // ~ inverse of projected screen size
    if (ratio < 15.0f) { return 0u; }
    if (ratio < 35.0f) { return 1u; }
    if (ratio < 70.0f) { return 2u; }
    return 3u;
}
} // namespace render
