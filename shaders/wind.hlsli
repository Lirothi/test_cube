#ifndef WIND_HLSLI
#define WIND_HLSLI

// W4 — the single shared wind-sway function. The gbuffer VS (BaseVS) and the shadow VS (W5) both
// include this and call it IDENTICALLY, so the shadow tracks the tree and every submesh stays in
// lockstep. Any divergence (a different seed/constant, or gbuffer-vs-shadow math drift) detaches the
// shadow or tears a tree apart at a submesh seam. Treat this as the single source of the sway math.
//
// The phase is derived from the tree's WORLD ORIGIN (worldOrigin.xz) — shared by every submesh/slot
// AND by the shadow instance — so submesh-sync and shadow-match are automatic and free. Per-tree
// variation (trees don't sway in unison) comes from that spatial term. A height weight plants the
// base and lets the canopy sway.

// Object-space height (metres) over which the sway ramps from 0 (base) to full (canopy). Palms are
// authored base-at-0 (A2). Exposed as a constant for now; a per-object value can replace it later.
// `saturate` clamps meshes taller than this to full sway above the threshold.
static const float kWindInvTreeHeight = 1.0 / 3.0;

// Returns a WORLD-space offset to add to the world position. windStrength 0 => exactly float3(0,0,0)
// so non-foliage objects are byte-identical to the no-wind path.
float3 WindOffset(float3 objPos, float3 worldOrigin, float windStrength,
                  float2 windDirXZ, float swayAmp, float swayFreq, float gustMul, float t)
{
    if (windStrength <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    // Height weight: 0 at the base, ->1 at the canopy, squared so the trunk base is stiff and the
    // top is floppy. Object-space Y is scale-invariant (fractional height up the mesh).
    float h = saturate(objPos.y * kWindInvTreeHeight);
    h = h * h;

    const float amp = swayAmp * gustMul * windStrength * h;

    // Per-tree phase from the world origin (the sync anchor: identical for all submeshes + the
    // shadow instance). Layered sines (not one) read less robotic.
    const float p = t * swayFreq + dot(worldOrigin.xz, float2(0.13, 0.17));
    const float sway = (sin(p) + 0.5 * sin(p * 2.3 + 1.7)) * amp;

    // A small perpendicular, higher-frequency flutter for the leaves.
    const float2 perp = float2(-windDirXZ.y, windDirXZ.x);
    const float flutter = sin(p * 3.7 + dot(worldOrigin.xz, float2(0.7, 0.3))) * 0.15 * amp;

    float3 offset;
    offset.x = windDirXZ.x * sway + perp.x * flutter;
    offset.y = 0.0; // horizontal bend only; a small vertical droop could be layered in later
    offset.z = windDirXZ.y * sway + perp.y * flutter;
    return offset;
}

#endif // WIND_HLSLI
