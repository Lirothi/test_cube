#ifndef WIND_HLSLI
#define WIND_HLSLI

// The single shared wind-sway function. The gbuffer VS (BaseVS) and the depth-only shadow VS both
// include this and call it IDENTICALLY, so the shadow tracks the tree and every submesh stays in
// lockstep. Any divergence (a different seed/constant, or gbuffer-vs-shadow math drift) detaches the
// shadow or tears a tree apart at a submesh seam. Treat this as the single source of the sway math.
//
// TWO INVARIANTS THAT MAKE THE SYNC FREE — do not break them:
//  1. The per-tree phase comes from the tree's WORLD ORIGIN (worldOrigin.xz), which every submesh
//     and the shadow instance share.
//  2. Everything else is a function of OBJECT-SPACE POSITION ONLY. In particular the leaf mask is
//     derived from objPos, NOT from the vertex normal — the depth-only shadow VS has no normal in
//     its input layout (PosOnly_InstCasterId / PosUV_InstCasterId), so any normal-driven term would
//     be impossible to reproduce there and the shadow would drift away from the tree.
//
// Structure follows the standard two-level vegetation model (GPU Gems 3 ch.16, "Vegetation
// Procedural Animation and Shading in Crysis"): a MAIN BEND that leans the whole plant downwind,
// plus a small DETAIL flutter confined to the foliage.

// Where the canopy attaches, as a fraction of mesh height. Foliage motion grows with distance FROM
// this point, so a frond that hangs DOWN along the trunk gets just as much give as one sweeping up —
// a purely radial mask left the hanging ones pinned to the trunk. 0.7 matches the measured frond
// attachments on the staged palm (Y 2.46..5.91 of a 7.34 m mesh).
static const float kWindCrownHeightFrac = 0.70;
// Frond reach from the attachment, also as a fraction of mesh height (measured 1.64..2.27 m / 7.34).
static const float kWindFrondSpanFrac = 0.30;

// How far foliage streams downwind on top of the trunk's bend. This is what makes leaves FOLLOW the
// wind instead of flapping symmetrically in place.
static const float kWindFoliageStream = 0.9;

// Detail-flutter size relative to the main sway amplitude, and its rate relative to the sway rate.
// Small now that the streaming term carries the bulk of the foliage motion.
static const float kWindFlutterAmp  = 0.15;
static const float kWindFlutterRate = 2.7;

// Steady (DC) share of the bend vs the oscillating share. The oscillation must not overpower the
// steady term, or the tree leans upwind half the time and the wind direction stops reading.
// osc is in [-1.5, 1.5], so bend stays in [1 - 1.5*kWindBendOsc, 1 + 1.5*kWindBendOsc] > 0.
static const float kWindBendSteady = 1.0;
static const float kWindBendOsc    = 0.35;

// Peak of the bend envelope, i.e. max of (kWindBendSteady + kWindBendOsc * osc) — mirrored by
// WindState::MaxSwayExtentMeters on the CPU for the shadow caster-bounds padding.
static const float kWindBendPeak = kWindBendSteady + 1.5 * kWindBendOsc; // 1.525

// Normalised height profile of the bend. Straight from GPU Gems 3 ch.16: (1+u)^4 - (1+u)^2, scaled
// so that f(1) == 1 (the raw polynomial evaluates to 12 at u = 1). It is a smooth polynomial with NO
// clamp anywhere, which is the whole point: the previous saturate(y/k)^2 profile flattened to a
// constant partway up the trunk, and the resulting derivative discontinuity showed up as a visible
// KINK at that height, with the entire crown above it rigidly translating as one block.
float WindBendProfile(float u)
{
    float f = 1.0 + max(u, 0.0);
    f *= f;              // (1+u)^2
    f = f * f - f;       // (1+u)^4 - (1+u)^2
    return f * (1.0 / 12.0);
}

// Returns a WORLD-space offset to add to the world position.
//   objPos       object-space vertex position; the bend pivots about the object origin (assets are
//                authored base-at-0), so y is height up the tree and xz is distance from its axis.
//   invHeight    1 / mesh height (per-object, from the mesh bounds). Normalising by the ACTUAL mesh
//                means the profile spans the whole tree instead of saturating at some hardcoded
//                height that has nothing to do with the asset.
//   foliage      PER-SLOT 0..1: 0 = woody trunk, 1 = leaves. Authored per asset (mesh.json
//                "windFoliage"), defaulting to the slot's alpha-mask flag. Geometry alone cannot
//                tell a hanging frond from the trunk it hangs against, which is why this is data
//                rather than a heuristic.
//   trunkStiff   PER-OBJECT: divides the main bend. >1 = stiffer trunk, <1 = whippier.
// windStrength 0 => exactly float3(0,0,0), so non-foliage objects are byte-identical to the no-wind path.
//   posWS        the vertex's UN-swayed world position; the arc is built in WORLD space about the
//                object's world origin, so the object's scale and rotation are handled exactly and
//                swayAmp keeps its meaning (metres of world displacement at the canopy). Doing the
//                arc in object space instead silently divides the visible lean by the object scale.
float3 WindOffset(float3 objPos, float3 posWS, float3 worldOrigin, float windStrength,
                  float invHeight, float foliage, float trunkStiff,
                  float2 windDirXZ, float swayAmp, float swayFreq, float gustMul, float t)
{
    if (windStrength <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float amp = swayAmp * windStrength;

    // Per-tree phase from the world origin (the sync anchor: identical for all submeshes + the
    // shadow instance). Layered sines (not one) read less robotic.
    const float p = t * swayFreq + dot(worldOrigin.xz, float2(0.13, 0.17));
    const float osc = sin(p) + 0.5 * sin(p * 2.3 + 1.7); // [-1.5, 1.5], zero-mean

    // The oscillation rides on a STEADY downwind lean. A zero-mean bend (what this used to be) makes
    // the tree spend as much time leaning upwind as downwind, and the wind direction becomes
    // unreadable — it just looks like the tree is wobbling in place.
    const float bend = (kWindBendSteady + kWindBendOsc * osc) * gustMul;

    // --- Main bend: displace along the wind, then restore the original distance from the pivot. ---
    // The rescale is what turns a shear into a bend: the tip travels along an arc and DROPS as it
    // leans out, instead of the trunk stretching. Without it the whole crown just slides sideways.
    // The height/leaf weights stay in OBJECT space (scale-invariant fractions of the mesh), but the
    // arc itself is built in WORLD space about the object's base.
    const float f = WindBendProfile(objPos.y * invHeight) / max(trunkStiff, 0.05);

    // Foliage weight: the authored per-slot flag times how far this vertex reaches from the crown
    // attachment. Distance (not radius) is the point — it treats a frond hanging down the trunk the
    // same as one sweeping outward. objPos-only, so the depth-only shadow VS reproduces it exactly.
    const float3 crownPos = float3(0.0, kWindCrownHeightFrac / max(invHeight, 1.0e-6), 0.0);
    float leaf = saturate(length(objPos - crownPos) * invHeight * (1.0 / kWindFrondSpanFrac));
    leaf = foliage * leaf * leaf; // soft near the attachment, full at the frond tip

    // Leaves ride the trunk's bend AND stream further downwind on their own. Folding the extra push
    // into the same arc keeps the whole thing length-preserving.
    const float3 rel = posWS - worldOrigin; // pivot -> vertex, world space
    float3 q = rel;
    q.xz += windDirXZ * (amp * bend * (f + leaf * kWindFoliageStream));

    const float len = length(rel);
    float3 offset = (len > 1.0e-4) ? (normalize(q) * len - rel) : float3(0.0, 0.0, 0.0);

    // --- Detail flutter: foliage only, on top of the streamed pose. ---
    if (leaf > 0.0)
    {
        // Per-frond decorrelation without any baked leaf id: fronds splay out in all directions, so
        // their xz already differ strongly. Normalising by the height keeps this scale-independent.
        const float lp = p * kWindFlutterRate +
                         dot(objPos.xz * invHeight, float2(5.1, 3.3));
        const float fa = amp * gustMul * leaf * kWindFlutterAmp;

        const float2 perp = float2(-windDirXZ.y, windDirXZ.x);
        offset.y += sin(lp) * fa;
        offset.xz += perp * (sin(lp * 1.3 + 0.7) * fa * 0.5);
    }

    return offset;
}

#endif // WIND_HLSLI
