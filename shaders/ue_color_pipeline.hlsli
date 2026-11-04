#ifndef UE_COLOR_PIPELINE_HLSLI
#define UE_COLOR_PIPELINE_HLSLI

// Unreal's colour pipeline around its film curve, as their tonemapper bakes it into the colour grading LUT for an sRGB
// display (the Filmic tonemapping method). Transcribed from UE 5.8.2 (D:\zalupa\Engine\Shaders\Private):
//   PostProcessCombineLUTs.usf  CombineLUTsCommon (:344-416), ComputeFilmColorNoGamma (:160-233), ColorCorrect (:88-102)
//   TonemapCommon.ush           FilmToneMap (:110-225)
//   ACES/ACESCommon.ush         the matrices, rgb_2_saturation / rgb_2_hue / center_hue / glow_fwd / rgb_2_yc /
//                               sigmoid_shaper (:17-124, :140-299)
// In order: linear sRGB -> ACEScg (AP1); ExpandGamut (bright saturated colour pushed toward a wide gamut); the grade
// (ColorCorrect, luma by AP1's weights); BlueCorrection; the film curve -- the ACES glow module and red modifier in AP0,
// then the five-knob curve per AP1 channel, its pre/post desaturation by AP1 luma; the blue correction undone; back to
// linear sRGB clipped at 0. The display transfer (sRGB OETF) is the caller's.
//
// Left out because UE's defaults make them identities: white balance (6500 K, tint 0), the shadows / midtones /
// highlights split of the grade (all three take the global values, their weights sum to 1), the legacy LDR LUT blend,
// ColorCorrection's mapping polynomial (0, 1, 0), ColorScale / OverlayColor, InverseGamma.y (display gamma 2.2).
//
// film_curve.hlsli's FilmCurveToneMap is the same curve run in linear sRGB without the glow and the red modifier (their
// headers were not in the old reference drop); `ColorPipelineSettings::ueGamut` 0 keeps that path, the A/B.

#include "color_grade.hlsli"
#include "film_curve.hlsli"

static const float3x3 kUeAP0_2_XYZ =
{
    0.9525523959, 0.0000000000, 0.0000936786,
    0.3439664498, 0.7281660966,-0.0721325464,
    0.0000000000, 0.0000000000, 1.0088251844,
};
static const float3x3 kUeXYZ_2_AP0 =
{
     1.0498110175, 0.0000000000,-0.0000974845,
    -0.4959030231, 1.3733130458, 0.0982400361,
     0.0000000000, 0.0000000000, 0.9912520182,
};
static const float3x3 kUeAP1_2_XYZ =
{
     0.6624541811, 0.1340042065, 0.1561876870,
     0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003,
};
static const float3x3 kUeXYZ_2_AP1 =
{
     1.6410233797, -0.3248032942, -0.2364246952,
    -0.6636628587,  1.6153315917,  0.0167563477,
     0.0117218943, -0.0082844420,  0.9883948585,
};
static const float3x3 kUeAP0_2_AP1 =
{
     1.4514393161, -0.2365107469, -0.2149285693,
    -0.0765537734,  1.1762296998, -0.0996759264,
     0.0083161484, -0.0060324498,  0.9977163014,
};
static const float3x3 kUeAP1_2_AP0 =
{
     0.6954522414,  0.1406786965,  0.1638690622,
     0.0447945634,  0.8596711185,  0.0955343182,
    -0.0055258826,  0.0040252103,  1.0015006723,
};
static const float3 kUeAP1_RGB2Y = float3(0.2722287168, 0.6740817658, 0.0536895174);
static const float3x3 kUeXYZ_2_sRGB =
{
     3.2409699419, -1.5373831776, -0.4986107603,
    -0.9692436363,  1.8759675015,  0.0415550574,
     0.0556300797, -0.2039769589,  1.0569715142,
};
static const float3x3 kUeSRGB_2_XYZ =
{
    0.4123907993, 0.3575843394, 0.1804807884,
    0.2126390059, 0.7151686788, 0.0721923154,
    0.0193308187, 0.1191947798, 0.9505321522,
};
static const float3x3 kUeD65_2_D60 =
{
     1.0130349146, 0.0061052578, -0.0149709436,
     0.0076982301, 0.9981633521, -0.0050320385,
    -0.0028413174, 0.0046851567,  0.9245061375,
};
static const float3x3 kUeD60_2_D65 =
{
     0.9872240087, -0.0061132286, 0.0159532883,
    -0.0075983718,  1.0018614847, 0.0053300358,
     0.0030725771, -0.0050959615, 1.0816806031,
};

float UeRgbToSaturation(float3 rgb)
{
    const float minrgb = min(min(rgb.r, rgb.g), rgb.b);
    const float maxrgb = max(max(rgb.r, rgb.g), rgb.b);
    return (max(maxrgb, 1e-10f) - max(minrgb, 1e-10f)) / max(maxrgb, 1e-2f);
}

float UeRgbToHue(float3 rgb)
{
    float hue = 0.0f;
    if (!(rgb.r == rgb.g && rgb.g == rgb.b))
    {
        hue = (180.0f / 3.14159265f) * atan2(sqrt(3.0f) * (rgb.g - rgb.b), 2.0f * rgb.r - rgb.g - rgb.b);
    }
    if (hue < 0.0f) { hue += 360.0f; }
    return clamp(hue, 0.0f, 360.0f);
}

float UeCenterHue(float hue, float centerH)
{
    float hueCentered = hue - centerH;
    if (hueCentered < -180.0f) { hueCentered += 360.0f; }
    else if (hueCentered > 180.0f) { hueCentered -= 360.0f; }
    return hueCentered;
}

float UeGlowFwd(float ycIn, float glowGainIn, float glowMid)
{
    if (ycIn <= 2.0f / 3.0f * glowMid) { return glowGainIn; }
    if (ycIn >= 2.0f * glowMid) { return 0.0f; }
    return glowGainIn * (glowMid / ycIn - 0.5f);
}

float UeRgbToYc(float3 rgb)
{
    const float kYcRadiusWeight = 1.75f;
    const float chroma = sqrt(max(rgb.b * (rgb.b - rgb.g) + rgb.g * (rgb.g - rgb.r) + rgb.r * (rgb.r - rgb.b), 0.0f));
    return (rgb.b + rgb.g + rgb.r + kYcRadiusWeight * chroma) / 3.0f;
}

float UeSigmoidShaper(float x)
{
    const float t = max(1.0f - abs(0.5f * x), 0.0f);
    const float y = 1.0f + sign(x) * (1.0f - t * t);
    return 0.5f * y;
}

// UE ColorCorrect on an AP1 colour, the global values only (see above).
float3 UeColorCorrect(float3 colorAP1, ColorGradeParams p)
{
    const float luma = dot(colorAP1, kUeAP1_RGB2Y);
    colorAP1 = max(0.0f, lerp(luma.xxx, colorAP1, p.saturation));
    colorAP1 = pow(colorAP1 * (1.0f / 0.18f), p.contrast) * 0.18f;
    colorAP1 = pow(colorAP1, 1.0f / max(p.gamma, 1e-4f));
    return colorAP1 * p.gain + p.offset;
}

// UE FilmToneMap: AP1 in, AP1 out.
float3 UeFilmToneMapAP1(float3 colorAP1, FilmCurveParams film)
{
    float3 colorAP0 = mul(kUeAP1_2_AP0, colorAP1);

    // The ACES "glow" module.
    const float kGlowGain = 0.05f;
    const float kGlowMid = 0.08f;
    const float saturation = UeRgbToSaturation(colorAP0);
    const float ycIn = UeRgbToYc(colorAP0);
    const float s = UeSigmoidShaper((saturation - 0.4f) / 0.2f);
    colorAP0 *= 1.0f + UeGlowFwd(ycIn, kGlowGain * s, kGlowMid);

    // The ACES red modifier.
    const float kRedScale = 0.82f;
    const float kRedPivot = 0.03f;
    const float kRedHue = 0.0f;
    const float kRedWidth = 135.0f;
    const float centeredHue = UeCenterHue(UeRgbToHue(colorAP0), kRedHue);
    const float hueWeightRoot = smoothstep(0.0f, 1.0f, 1.0f - abs(2.0f * centeredHue / kRedWidth));
    const float hueWeight = hueWeightRoot * hueWeightRoot;
    colorAP0.r += hueWeight * saturation * (kRedPivot - colorAP0.r) * (1.0f - kRedScale);

    float3 working = max(0.0f, mul(kUeAP0_2_AP1, colorAP0));
    working = lerp(dot(working, kUeAP1_RGB2Y).xxx, working, 0.96f);

    const float toeScale = 1.0f + film.blackClip - film.toe;
    const float shoulderScale = 1.0f + film.whiteClip - film.shoulder;
    const float kInMatch = 0.18f;
    const float kOutMatch = 0.18f;
    float toeMatch;
    if (film.toe > 0.8f)
    {
        toeMatch = (1.0f - film.toe - kOutMatch) / film.slope + log10(kInMatch);
    }
    else
    {
        const float bt = (kOutMatch + film.blackClip) / toeScale - 1.0f;
        toeMatch = log10(kInMatch) - 0.5f * log((1.0f + bt) / (1.0f - bt)) * (toeScale / film.slope);
    }
    const float straightMatch = (1.0f - film.toe) / film.slope - toeMatch;
    const float shoulderMatch = film.shoulder / film.slope - straightMatch;

    // UE take log10 of the colour as it is; a channel at exactly 0 gives -inf there and a NaN in the blend below.
    const float3 logColor = log10(max(working, 1e-10f));
    const float3 straightColor = film.slope * (logColor + straightMatch);
    float3 toeColor = (-film.blackClip)
        + (2.0f * toeScale) / (1.0f + exp((-2.0f * film.slope / toeScale) * (logColor - toeMatch)));
    float3 shoulderColor = (1.0f + film.whiteClip)
        - (2.0f * shoulderScale) / (1.0f + exp((2.0f * film.slope / shoulderScale) * (logColor - shoulderMatch)));
    toeColor = FILM_SELECT(logColor < toeMatch, toeColor, straightColor);
    shoulderColor = FILM_SELECT(logColor > shoulderMatch, shoulderColor, straightColor);

    float3 t = saturate((logColor - toeMatch) / (shoulderMatch - toeMatch));
    t = (shoulderMatch < toeMatch) ? (1.0f - t) : t;
    t = (3.0f - 2.0f * t) * t * t;
    float3 toneColor = lerp(toeColor, shoulderColor, t);
    toneColor = lerp(dot(toneColor, kUeAP1_RGB2Y).xxx, toneColor, 0.93f);
    return max(0.0f, toneColor);
}

// Scene-referred linear sRGB (exposed, locally exposed, bloomed) -> display-referred linear sRGB, as UE's LUT holds it.
float3 UeFilmPipeline(float3 linearSrgb, ColorGradeParams grade, FilmCurveParams film, float expandGamut,
                      float blueCorrection)
{
    const float3x3 sRGB_2_AP1 = mul(kUeXYZ_2_AP1, mul(kUeD65_2_D60, kUeSRGB_2_XYZ));
    const float3x3 AP1_2_sRGB = mul(kUeXYZ_2_sRGB, mul(kUeD60_2_D65, kUeAP1_2_XYZ));

    float3 colorAP1 = mul(sRGB_2_AP1, max(linearSrgb, 0.0f));

    // Expand bright saturated colours outside the sRGB gamut to fake wide gamut rendering.
    const float lumaAP1 = dot(colorAP1, kUeAP1_RGB2Y);
    if (lumaAP1 > 0.0f)
    {
        const float3 chromaAP1 = colorAP1 / lumaAP1;
        const float chromaDistSqr = dot(chromaAP1 - 1.0f, chromaAP1 - 1.0f);
        const float expandAmount = (1.0f - exp2(-4.0f * chromaDistSqr)) * (1.0f - exp2(-4.0f * expandGamut * lumaAP1 * lumaAP1));
        // UE: "Bizarre matrix but this expands sRGB to between P3 and AP1".
        const float3x3 wide_2_XYZ =
        {
             0.5441691,  0.2395926,  0.1666943,
             0.2394656,  0.7021530,  0.0583814,
            -0.0023439,  0.0361834,  1.0552183,
        };
        const float3x3 expandMat = mul(mul(kUeXYZ_2_AP1, wide_2_XYZ), AP1_2_sRGB);
        colorAP1 = lerp(colorAP1, mul(expandMat, colorAP1), expandAmount);
    }

    colorAP1 = UeColorCorrect(colorAP1, grade);

    const float3x3 blueCorrect =
    {
        0.9404372683, -0.0183068787, 0.0778696104,
        0.0083786969,  0.8286599939, 0.1629613092,
        0.0005471261, -0.0008833746, 1.0003362486
    };
    const float3x3 blueCorrectInv =
    {
         1.06318,     0.0233956, -0.0865726,
        -0.0106337,   1.20632,   -0.19569,
        -0.000590887, 0.00105248, 0.999538
    };
    const float3x3 blueCorrectAP1 = mul(kUeAP0_2_AP1, mul(blueCorrect, kUeAP1_2_AP0));
    const float3x3 blueCorrectInvAP1 = mul(kUeAP0_2_AP1, mul(blueCorrectInv, kUeAP1_2_AP0));

    colorAP1 = lerp(colorAP1, mul(blueCorrectAP1, colorAP1), blueCorrection);
    colorAP1 = UeFilmToneMapAP1(colorAP1, film);
    colorAP1 = lerp(colorAP1, mul(blueCorrectInvAP1, colorAP1), blueCorrection);

    return max(0.0f, mul(AP1_2_sRGB, colorAP1));
}

#endif // UE_COLOR_PIPELINE_HLSLI
