#ifndef IBL_COMMON_HLSLI
#define IBL_COMMON_HLSLI

// F8 — the roughness <-> mip mapping for the prefiltered sky cube.
//
// Transcribed from Unreal's ReflectionEnvironmentShared.ush (the user supplied the file; the two
// constants are theirs verbatim). It is LOGARITHMIC, not linear, and the reason is in their own
// comment: a given mip always means the same roughness regardless of how many mips the texture has,
// so adding mips buys sharper reflections instead of re-scaling every existing one.
//
// Our first version mapped `roughness = mip / (mips - 1)`, which is self-consistent -- bake and
// sample agreed, so it rendered correctly -- but it spends half the chain on roughness > 0.5, where
// the lobe is so wide that neighbouring mips are nearly identical, and crams 0.1..0.3 (the range
// where a reflection still reads as a reflection) into the first two steps.
//
// THE BAKE AND THE SAMPLE MUST USE THESE AND ONLY THESE. A mismatch between the two is invisible in
// a screenshot -- it just looks like "the prefilter is a bit off" -- so there is exactly one
// definition and both sides call it.
#define REFLECTION_CAPTURE_ROUGHEST_MIP 1.0f
#define REFLECTION_CAPTURE_ROUGHNESS_MIP_SCALE 1.2f

// Absolute mip to sample for a given perceptual roughness. `mipCount` is the cube's real mip count.
float IblMipFromRoughness(float roughness, float mipCount)
{
    const float levelFrom1x1 = REFLECTION_CAPTURE_ROUGHEST_MIP -
        REFLECTION_CAPTURE_ROUGHNESS_MIP_SCALE * log2(max(roughness, 0.001f));
    return clamp(mipCount - 1.0f - levelFrom1x1, 0.0f, mipCount - 1.0f);
}

// The exact inverse: which roughness mip `mip` was prefiltered for. Used by the importer (mirrored
// in C++) so the two ends cannot drift.
float IblRoughnessFromMip(float mip, float mipCount)
{
    const float levelFrom1x1 = mipCount - 1.0f - mip;
    return exp2((REFLECTION_CAPTURE_ROUGHEST_MIP - levelFrom1x1) /
                REFLECTION_CAPTURE_ROUGHNESS_MIP_SCALE);
}

// F9 -- roughness- and view-aware specular occlusion from a SCALAR ambient occlusion value.
//
// Lagarde & de Rousiers, "Moving Frostbite to Physically Based Rendering", listing 26. The shape is
// the point: a mirror reflects a single direction, so a cavity term that describes hemispherical
// visibility says almost nothing about whether THAT direction is blocked -- and multiplying a sharp
// reflection by AO produces the "dirty chrome" look. As roughness grows the lobe covers more of the
// hemisphere and AO becomes an increasingly good description of it, so the exponent lets occlusion
// take hold. NoV matters for the same reason: at grazing angles the lobe skims the surface.
//
// Not UE's `GetDistanceFieldAOSpecularOcclusion` (SkyLightingShared.ush), which is a cone-cone
// intersection between the reflection lobe and an unoccluded cone -- it needs a BENT NORMAL, i.e.
// the direction visibility is open in, and our GBAux carries a scalar only. Revisit if a bent
// normal ever ships (GTAO can produce one).
//
// Measured, AO = 0.3 (a flat multiply would give 0.300 everywhere; higher = less occluded):
//     roughness   NoV=0.2   NoV=0.5   NoV=0.9
//         0.02      0.058     0.215     0.376
//         0.30      0.288     0.296     0.303
//         0.60+     0.300     0.300     0.300
// A near-mirror seen face-on keeps MORE of its reflection than AO alone would allow, the same
// mirror seen at a grazing angle keeps far less, and by roughness 0.6 the term has converged on
// plain AO because by then the lobe really does cover the hemisphere AO describes.
//
// AO = 1 returns exactly 1 -- verified over the whole (NoV, roughness) domain, not just spot
// checked -- so an unauthored material is bit-identical to before this existed.
float IblSpecularOcclusion(float ndotv, float ao, float roughness)
{
    return saturate(pow(abs(ndotv) + ao, exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao);
}

#endif // IBL_COMMON_HLSLI
