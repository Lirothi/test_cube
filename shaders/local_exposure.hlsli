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
// BASE LAYER: this uses a blurred log-luminance, not UE's bilateral grid. UE blends between the two
// (`BlurredLuminanceBlend`) and falls back to the blur wherever the grid has no data, so a
// blur-only base is a configuration of the same algorithm rather than a different one. The
// difference is halo resistance: a plain blur bleeds across a high-contrast edge, so a bright sky
// can darken the treeline just inside it. If halos show up, the bilateral grid is the upgrade --
// and it is cheap for us, because our histogram pass already bins by log-luminance.

struct LocalExposureParams
{
    float highlightContrastScale; // 1 = no compression above middle grey
    float shadowContrastScale;    // 1 = no compression below middle grey
    float detailStrength;         // 1 = detail passes through untouched
    float blurredBlend;           // reserved for the bilateral-grid upgrade; 1 = pure blur today
    float highlightThreshold;     // stops above middle grey before compression starts
    float shadowThreshold;        // stops below middle grey before compression starts
};

bool LocalExposureIsNeutral(LocalExposureParams p)
{
    return p.highlightContrastScale == 1.0f && p.shadowContrastScale == 1.0f
        && p.detailStrength == 1.0f;
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
