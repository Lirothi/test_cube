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
// A BLURRED SKY SAMPLE MAY NOT RUN AWAY FROM THE SKY IT IS BLURRING.
//
// Prefiltering integrates a lobe. When the sky holds a sun hundreds of thousands of times brighter
// than the sky around it, that integral is the SUN in every direction the lobe can reach, and what
// comes back stops being a picture of the sky: reflections and fog turn into a flat bright wash
// while the sky next to them stays blue. The invariant it breaks is the one anybody checks first --
// the water just below the horizon must match the sky just above it.
//
// So bound it by the SHARP sample in the same direction, which is exactly the sky it has to agree
// with. Looking at plain sky, the blurred value cannot exceed it by more than the headroom a real
// blur needs. Looking INTO the sun, the sharp sample is enormous too, the bound does nothing, and
// the glitter survives -- which is correct, because there the sky really is that bright.
static const float kIblBlurHeadroom = 2.0f;

float3 IblClampToSharp(float3 blurred, float3 sharp)
{
    const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
    const float ceiling = dot(max(sharp, 0.0f.xxx), kLuma) * kIblBlurHeadroom;
    const float lum = dot(max(blurred, 0.0f.xxx), kLuma);
    if (lum <= ceiling || lum < 1.0e-6f) { return blurred; }
    // Scaled, not clipped per channel: clipping drags the hue towards whichever channel saturated
    // first, and the COLOUR is the whole reason the sky is sampled at all.
    return blurred * (ceiling / lum);
}

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

// Mip guessed for a RAW skybox cube, i.e. one that arrived without prefiltered derivatives. A real
// prefilter maps roughness to mip by construction (IblMipFromRoughness) and needs no guess.
static const float kSkyRoughMaxMip = 5.0;

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// ---------------------------------------------------------------------------------------------
// THE SKY'S INDIRECT SPECULAR, SPLIT ACROSS TWO PASSES.
//
// It used to live entirely in compose. That put it AFTER the screen-space reflection pass, which
// samples the LIGHT target -- so a metal seen in a reflection was a black disc with a highlight,
// because a metal has no diffuse and its only other contribution had not been added yet. The
// lighting pass adds it now, and compose adds the DIFFERENCE the reflection makes:
//
//     lighting : skyCol            * weight
//     compose  : (hit - skyCol*a)  * weight
//     total    : (hit + skyCol*(1-a)) * weight     <- what compose alone used to produce
//
// The identity only holds while both sides compute `skyCol` and `weight` the same way, so both
// come from here and neither pass open-codes them.
// ---------------------------------------------------------------------------------------------

// The radiance arriving from the sky along R. `specMipCount` 0 means this level's sky has no
// prefiltered derivatives, and the raw cube is sampled at a guessed mip instead.
float3 IblSkyRadiance(TextureCube prefiltered, TextureCube rawSky, SamplerState smp,
                      float3 R, float roughness, uint specMipCount, float intensity)
{
    const bool useSplitSum = (specMipCount > 0u);
    const float mip = useSplitSum ? IblMipFromRoughness(roughness, (float)specMipCount)
                                  : roughness * kSkyRoughMaxMip;
    const float3 radiance = useSplitSum ? prefiltered.SampleLevel(smp, R, mip).rgb
                                        : rawSky.SampleLevel(smp, R, mip).rgb;
    return radiance * intensity;
}

// What a radiance value reflecting off this surface is multiplied by to become indirect specular.
// With a real prefilter the split-sum LUT already integrates Fresnel AND the geometry term over
// the lobe, so applying Fresnel again would apply it twice.
float3 IblSpecularWeight(Texture2D brdfLut, SamplerState smp, float3 F0, float cosT,
                         float roughness, uint specMipCount)
{
    if (specMipCount > 0u)
    {
        const float2 ab = brdfLut.SampleLevel(smp, float2(cosT, roughness), 0).rg;
        return F0 * ab.x + ab.y;
    }
    return FresnelSchlick(cosT, F0) * saturate(1.0 - roughness);
}

#endif // IBL_COMMON_HLSLI
