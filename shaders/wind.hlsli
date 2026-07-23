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
//  2. Everything else is a function of the VERTEX's own baked weights and object-space position —
//     never of the vertex NORMAL. The depth-only shadow VS has no normal in its input layout
//     (PosOnly_InstCasterId / PosUV_InstCasterId), so a normal-driven term would be impossible to
//     reproduce there and the shadow would drift away from the tree.
//
// W7.3: every geometric heuristic is GONE. The old version guessed where the canopy was
// (kWindCrownHeightFrac / kWindFrondSpanFrac + a radial mask) and guessed stiffness from
// objPos.y / meshHeight. Both broke on anything that is not a single upright trunk — on a two-trunk
// palm 36 % of frond vertices saturated at full weight, so a third of the canopy had no
// base-to-tip gradient at all and simply streamed at maximum. The weights now come from the
// importer bake (W7.2, MeshManager::BakeWindWeightsCpu), which walks the actual mesh:
//
//   windWeights.r  geodesic distance along the surface from the plant's ground contact, 0..1.
//                  Trunk base 0 -> trunk top -> frond base -> frond tip 1. Replaces BOTH the old
//                  height profile and the old crown-distance mask. Multi-trunk is free (each trunk
//                  seeds from its own contact patch) and a frond hanging DOWN the trunk measures
//                  "up the trunk, then out along the frond" instead of reading as trunk.
//   windWeights.g  per-limb id (hashed connected component) -> per-frond phase decorrelation.
//   windWeights.b  geodesic distance from the WOOD SURFACE (the slots whose windFoliage is 0),
//                  normalised. 0 exactly where a leaf meets wood, 1 at the furthest leaf tip.
//                  This is what the foliage term must use — driving it with .r instead gives a frond
//                  BASE most of the push (r is 0.62..1.00 on a coconut frond), so the whole leaf
//                  translates off the crown and the canopy tears into streaks.
//                  Being a distance FIELD (not a per-mesh-component ramp) is what makes `foliage * b`
//                  continuous in space: it cannot restart mid-leaf on a frond the modeller split into
//                  a petiole and a blade, and it is pinned at 0 on both sides of a wood/leaf seam, so
//                  no per-slot weight the artist picks can tear or kink the mesh. The per-component
//                  ramp is only the fallback for a mesh baked without the wood classification.
//   windWeights.a  255 => the weights are baked. A mesh baked before W7.2 has a == 0, and with r == 0
//                  it is simply rigid: an unbaked foliage asset fails SAFE and obviously rather than
//                  silently swaying wrong. Re-bake with --reimport-src/--reimport-out.

// How far foliage streams downwind on top of the whole-plant bend. This is what makes leaves FOLLOW
// the wind instead of flapping symmetrically in place.
static const float kWindFoliageStream = 0.9;

// Detail-flutter size relative to the main sway amplitude, and its rate relative to the sway rate.
// Small: the streaming term carries the bulk of the foliage motion.
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

// Stiffness profile along the plant. Straight from GPU Gems 3 ch.16: (1+u)^4 - (1+u)^2, scaled so
// f(1) == 1 (the raw polynomial evaluates to 12 at u = 1). `u` is now the BAKED geodesic weight
// rather than a normalised height, so the curve follows a leaning or curved trunk correctly. It is a
// smooth polynomial with NO clamp: the derivative discontinuity of the old saturate()d profile was
// the visible KINK partway up the trunk.
float WindBendProfile(float u)
{
    float f = 1.0 + max(u, 0.0);
    f *= f;              // (1+u)^2
    f = f * f - f;       // (1+u)^4 - (1+u)^2
    return f * (1.0 / 12.0);
}

// Returns a WORLD-space offset to add to the world position.
//   objPos       object-space vertex position (only used for the flutter's spatial phase).
//   posWS        the vertex's UN-swayed world position; the arc is built in WORLD space about the
//                object's world origin, so the object's scale and rotation are handled exactly and
//                swayAmp keeps its meaning (metres of world displacement at the canopy). Doing the
//                arc in object space instead silently divides the visible lean by the object scale.
//   windWeights  the baked COLOR_0 channels described above.
//   foliage      PER-SLOT 0..1 artistic multiplier from mesh.json "windFoliage" (0 = treat this
//                submesh as woody). Rides ON TOP of the baked weights; it does not replace them.
//   trunkStiff   PER-OBJECT: divides the main bend. >1 = stiffer trunk, <1 = whippier.
// windStrength 0 => exactly float3(0,0,0), so non-foliage objects are byte-identical to the no-wind path.
float3 WindOffset(float3 objPos, float3 posWS, float3 worldOrigin, float windStrength,
                  float4 windWeights, float foliage, float trunkStiff,
                  float2 windDirXZ, float swayAmp, float swayFreq, float gustMul, float t)
{
    if (windStrength <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float amp = swayAmp * windStrength;
    const float along = windWeights.r; // geodesic position along the plant, 0 root -> 1 extremity

    // Per-tree phase from the world origin (the sync anchor: identical for all submeshes + the
    // shadow instance). Layered sines (not one) read less robotic.
    const float p = t * swayFreq + dot(worldOrigin.xz, float2(0.13, 0.17));
    const float osc = sin(p) + 0.5 * sin(p * 2.3 + 1.7); // [-1.5, 1.5], zero-mean

    // The oscillation rides on a STEADY downwind lean. A zero-mean bend makes the tree spend as much
    // time leaning upwind as downwind and the wind direction becomes unreadable.
    const float bend = (kWindBendSteady + kWindBendOsc * osc) * gustMul;

    // --- Main bend: displace along the wind, then restore the original distance from the pivot. ---
    // The rescale is what turns a shear into a bend: the tip travels along an arc and DROPS as it
    // leans out, instead of the trunk stretching. Without it the whole crown just slides sideways.
    const float f = WindBendProfile(along) / max(trunkStiff, 0.05);

    // Leaves ride the plant's bend AND stream further downwind on their own, bending about THEIR OWN
    // attachment (windWeights.b), which also keeps the term continuous with the trunk across the
    // attachment. Folding the extra push into the SAME arc keeps the whole thing length-preserving.
    const float leaf = foliage * windWeights.b;
    const float3 rel = posWS - worldOrigin; // pivot -> vertex, world space
    float3 q = rel;
    q.xz += windDirXZ * (amp * bend * (f + leaf * kWindFoliageStream));

    const float len = length(rel);
    float3 offset = (len > 1.0e-4) ? (normalize(q) * len - rel) : float3(0.0, 0.0, 0.0);

    // --- Detail flutter: foliage only, on top of the streamed pose. ---
    if (leaf > 0.0)
    {
        // Per-frond decorrelation from the baked limb id, so every vertex of one leaf shares a phase
        // (a phase that varied WITHIN a leaf would tear it). The objPos term adds a little variation
        // along the leaf itself.
        const float limbPhase = windWeights.g * 6.2831853;
        const float lp = p * kWindFlutterRate + limbPhase + dot(objPos.xz, float2(0.7, 0.5));
        const float fa = amp * gustMul * leaf * kWindFlutterAmp;

        const float2 perp = float2(-windDirXZ.y, windDirXZ.x);
        offset.y += sin(lp) * fa;
        offset.xz += perp * (sin(lp * 1.3 + 0.7) * fa * 0.5);
    }

    return offset;
}

#endif // WIND_HLSLI
