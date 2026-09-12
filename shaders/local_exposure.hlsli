#ifndef LOCAL_EXPOSURE_HLSLI
#define LOCAL_EXPOSURE_HLSLI

// Local exposure (photographic plan, step P3B).
//
// Transcribed from `CalculateLogLocalExposure` in Unreal's PostProcessHistogramCommon.ush. A global
// exposure can only SLIDE the histogram; this stretches it, by varying exposure spatially the way
// the retina does. It is what makes an image read like an "HDR photo" -- shadow detail and highlight
// detail at the same time -- rather than like one correctly metered compromise.
//
// The trick is the decomposition. Split log-luminance into a low-frequency BASE (large-scale
// illumination) and a DETAIL residual, compress only the base, and put the detail back untouched.
// Compressing the base reduces the scene's dynamic range; leaving the detail alone preserves
// micro-contrast, which is why the result looks vivid instead of the flat, mushy "tone-mapped HDR"
// cliche.
//
// BASE LAYER (2026-09-12): UE's bilateral grid blended with a blurred log-luminance, exactly their
// `CalculateBaseLogLuminance` (PostProcessHistogramCommon.ush): `lerp(bilateral, blurred,
// BlurredLuminanceBlend)`, with the blur as the fallback wherever the grid has no data. A plain
// blur bleeds across a high-contrast edge -- a bright sky raises the base of the first rows of water
// under the horizon and the operator pulls them down (the 25-px band of 2026-09-11). The grid is
// sliced at the pixel's OWN luminance, so those rows see the mean of the water around them, not the
// sky's. `blurredBlend` 1 is the pre-grid blur-only base (the A/B arm); UE ship 0.6.

struct LocalExposureParams
{
    float highlightContrastScale; // 1 = no compression above middle grey
    float shadowContrastScale;    // 1 = no compression below middle grey
    float detailStrength;         // 1 = detail passes through untouched
    float blurredBlend;           // UE BlurredLuminanceBlend: 0 = pure grid, 1 = pure blur
    float highlightThreshold;     // stops above middle grey before compression starts
    float shadowThreshold;        // stops below middle grey before compression starts
};

bool LocalExposureIsNeutral(LocalExposureParams p)
{
    return p.highlightContrastScale == 1.0f && p.shadowContrastScale == 1.0f
        && p.detailStrength == 1.0f;
}

// UE BILATERAL_GRID_DEPTH; mirrored by exposure_bilateral_cs.hlsl and ExposureMetering.
static const float kLocalExposureBilateralBins = 32.0f;

// UE `CalculateBaseLogLuminance`. Slices the grid at this pixel's UNEXPOSED log-luminance
// (the grid was built from unexposed values) and blends with the blurred base. The result is in
// the same unexposed space; the caller shifts it by the exposure the pixel actually received.
//   uv            screen uv; the grid covers the frame exactly (see the CS header: UVScale == 1)
//   logLumScene   log2 of this pixel's unexposed luminance
//   minLogLum / invLogLumRange   the metering range the grid was binned over
float LocalExposureBaseLogLum(Texture3D<float2> grid, SamplerState smp, float2 uv,
                              float logLumScene, float minLogLum, float invLogLumRange,
                              float blurredLogLum, float blurredBlend)
{
    if (blurredBlend >= 1.0f)
    {
        return blurredLogLum;
    }
    float3 uvw;
    uvw.xy = uv;
    // Bin k's centre sits at histogram position k / (bins - 1) and at texel centre (k + 0.5) / bins.
    const float histPos = (logLumScene - minLogLum) * invLogLumRange;
    uvw.z = (histPos * (kLocalExposureBilateralBins - 1.0f) + 0.5f) / kLocalExposureBilateralBins;
    const float2 g = grid.SampleLevel(smp, uvw, 0);
    // Fallback to the blur where the grid has no data (UE: "this can happen since grid is
    // populated using half resolution image"; here, a luminance no texel of the tile came near).
    const float bilateral = (g.y < 0.001f) ? blurredLogLum : (g.x / g.y);
    return lerp(bilateral, blurredLogLum, blurredBlend);
}

// Returns the per-pixel multiplier to apply to scene colour.
// logLum / baseLogLum are log2 of the EXPOSED luminance; logMiddleGrey is log2(0.18).
float LocalExposureMultiplier(float logLum, float baseLogLum, float logMiddleGrey,
                              LocalExposureParams p)
{
    const float detailLogLum = logLum - baseLogLum;
    float baseCentered = baseLogLum - logMiddleGrey;

    // Which side of middle grey the neighbourhood sits on decides which scale applies: pushing
    // highlights down and shadows up are separate artistic choices, and UE splits them for that
    // reason.
    const float contrastScale = (baseCentered > 0.0f) ? p.highlightContrastScale
                                                      : p.shadowContrastScale;

    // Threshold region: hold the effect off until the neighbourhood is far enough from middle grey,
    // so mid-tones -- which are usually the subject -- are left alone instead of being churned.
    // The offset is carried through unscaled, which is what keeps the curve continuous at the
    // threshold rather than kinking there.
    float thresholdOffset = 0.0f;
    {
        float m;
        if (baseCentered > 0.0f)
        {
            m = max(0.0f, baseCentered - p.highlightThreshold);
        }
        else
        {
            m = min(0.0f, baseCentered + p.shadowThreshold);
        }
        thresholdOffset = baseCentered - m;
        baseCentered = m;
    }

    const float logLocalLum = logMiddleGrey + thresholdOffset
                            + baseCentered * contrastScale
                            + detailLogLum * p.detailStrength;

    return exp2(logLocalLum - logLum);
}

#endif // LOCAL_EXPOSURE_HLSLI
