#ifndef TONE_CURVES_HLSLI
#define TONE_CURVES_HLSLI

// The tone curves the tonemap pass ends with, as FUNCTIONS -- so the editor's asset preview can end
// with exactly the same ones. They lived inside tonemap_cs.hlsl, which made them unreachable from
// anywhere else, and the preview grew its own lighting and no curve at all; a material then looked
// one way in the Mesh Editor and another in the level. Moved here verbatim: the tonemap pass
// compiles to the same code it did before.

#include "utils.hlsli"      // LinearToGamma
#include "agx.hlsli"
#include "film_curve.hlsli"

// ---- named constants ----
static const float kGammaOut = 2.2;

// ACES fitted (K. Narkowicz)
float3 TonemapACES(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// The real sRGB OETF, not pow(1/2.2). The two diverge most in the deep shadows, where the linear
// toe below 0.0031308 keeps near-black from being lifted -- which is precisely the region the
// reference image has and we do not (the P0 measurements put our p02 ABOVE the reference's).
float3 LinearToSrgb(float3 x)
{
    x = saturate(x);
    const float3 lo = x * 12.92f;
    const float3 hi = 1.055f * pow(x, 1.0f / 2.4f) - 0.055f;
    return lerp(hi, lo, step(x, 0.0031308f));
}

// The exact inverse of the above. For a consumer that writes into an _SRGB render target, which
// encodes on store: handing it the tonemap pass's display CODE VALUES decoded by this makes the
// target store the very same bytes the tonemap pass would have written.
float3 SrgbToLinear(float3 x)
{
    x = saturate(x);
    const float3 lo = x / 12.92f;
    const float3 hi = pow((x + 0.055f) / 1.055f, 2.4f);
    return lerp(hi, lo, step(x, 0.04045f));
}

// Scene-referred light (already exposed and graded) to display code values, by the selected curve.
// 0 = legacy Narkowicz ACES fit + pow(1/2.2); 1 = AgX + sRGB OETF; 2 = Unreal's film curve + sRGB
// OETF. AgX and the film curve return display-referred LINEAR, so the display encoding happens in
// exactly one place per branch.
float3 ToneCurveToDisplay(float3 hdr, uint toneCurve, FilmCurveParams film,
                          float agxSlope, float agxPower, float agxSaturation)
{
    if (toneCurve == 2)
    {
        return LinearToSrgb(FilmCurveToneMap(hdr, film));
    }
    else if (toneCurve == 1)
    {
        return LinearToSrgb(AgxTonemap(hdr, agxSlope, agxPower, agxSaturation));
    }
    // Legacy path, byte-for-byte what shipped before P3.
    return LinearToGamma(TonemapACES(hdr), kGammaOut);
}

#endif // TONE_CURVES_HLSLI
