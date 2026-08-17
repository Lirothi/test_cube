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

#endif // IBL_COMMON_HLSLI
