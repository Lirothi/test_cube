#ifndef FILM_CURVE_HLSLI
#define FILM_CURVE_HLSLI

// Unreal's parameterised film curve (photographic plan, step P3C stage 1).
//
// Transcribed from `FilmToneMap` in the engine's TonemapCommon.ush. The five controls are exactly
// the ones an artist sees in Unreal's post-process volume under "Film": Slope, Toe, Shoulder,
// Black clip, White clip. That is the whole reason for having it -- the fixed Narkowicz fit
// approximates the same family of curve but exposes nothing to turn.
//
// WHAT IS FAITHFUL: the curve itself. Three segments solved in log10 -- a sigmoid toe, a straight
// middle, a sigmoid shoulder -- with the match points solved so that 0.18 in gives 0.18 out
// regardless of how the knobs are set. That invariant is what keeps the curve from fighting the
// exposure solve, which targets the same 0.18. Also faithful: the pre- and post-desaturation
// (0.96 / 0.93) that keep the segments from separating the channels.
//
// WHAT IS NOT INCLUDED, and why: Unreal runs this in ACEScg (AP1) working space and applies the
// ACES "glow" module and red modifier in AP0 first. Those need `ACES/ACESCommon.ush` and
// `ACES/ACES_v1.3.ush`, which are not in the reference drop -- and inventing their matrices from
// memory is exactly how the AgX outset matrix got transposed and tinted the frame pink. So this
// runs in linear sRGB. The consequence is a slightly different treatment of very saturated colour
// approaching the shoulder; the tonal response, which is what the knobs control, is the same.
// Dropping those two headers in makes finishing this a small, contained change.

struct FilmCurveParams
{
    float slope;      // steepness of the straight segment
    float toe;        // how much the shadows roll off
    float shoulder;   // how much the highlights roll off
    float blackClip;  // how far below zero the toe is allowed to reach
    float whiteClip;  // how far above one the shoulder is allowed to reach
};

// Unreal's defaults.
static const float kFilmSlopeDefault = 0.88f;
static const float kFilmToeDefault = 0.55f;
static const float kFilmShoulderDefault = 0.26f;
static const float kFilmBlackClipDefault = 0.0f;
static const float kFilmWhiteClipDefault = 0.04f;

float3 FilmCurveToneMap(float3 color, FilmCurveParams p)
{
    color = max(color, 0.0f);

    // Pre-desaturate: without it the three channels take the toe and shoulder at different points
    // and a bright saturated colour separates into a hue shift on the way up.
    const float3 kLumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
    color = lerp(dot(color, kLumaWeights).xxx, color, 0.96f);
    color = max(color, 0.0f);

    const float toeScale = 1.0f + p.blackClip - p.toe;
    const float shoulderScale = 1.0f + p.whiteClip - p.shoulder;

    // The match points are solved so that InMatch maps to OutMatch. Both are 0.18, so middle grey
    // is a fixed point of this curve no matter how the five knobs are set.
    const float kInMatch = 0.18f;
    const float kOutMatch = 0.18f;

    float toeMatch;
    if (p.toe > 0.8f)
    {
        // 0.18 lands on the straight segment.
        toeMatch = (1.0f - p.toe - kOutMatch) / p.slope + log10(kInMatch);
    }
    else
    {
        // 0.18 lands on the toe; solve for the match. The 0.5*log((1+b)/(1-b)) term is atanh(b).
        const float bt = (kOutMatch + p.blackClip) / toeScale - 1.0f;
        toeMatch = log10(kInMatch) - 0.5f * log((1.0f + bt) / (1.0f - bt)) * (toeScale / p.slope);
    }

    const float straightMatch = (1.0f - p.toe) / p.slope - toeMatch;
    const float shoulderMatch = p.shoulder / p.slope - straightMatch;

    const float3 logColor = log10(max(color, 1e-10f));
    const float3 straightColor = p.slope * (logColor + straightMatch);

    float3 toeColor = (-p.blackClip)
        + (2.0f * toeScale) / (1.0f + exp((-2.0f * p.slope / toeScale) * (logColor - toeMatch)));
    float3 shoulderColor = (1.0f + p.whiteClip)
        - (2.0f * shoulderScale) / (1.0f + exp((2.0f * p.slope / shoulderScale) * (logColor - shoulderMatch)));

    // select(), not ?: -- a per-component condition cannot drive a short-circuiting ternary in
    // current HLSL. UE's source uses select() here for the same reason; writing it as a ternary
    // compiles nowhere.
    toeColor = select(logColor < toeMatch, toeColor, straightColor);
    shoulderColor = select(logColor > shoulderMatch, shoulderColor, straightColor);

    float3 t = saturate((logColor - toeMatch) / (shoulderMatch - toeMatch));
    // A shoulder match below the toe match means the knobs have inverted the segments; flipping t
    // keeps the blend running the right way instead of producing a discontinuity.
    t = (shoulderMatch < toeMatch) ? (1.0f - t) : t;
    t = (3.0f - 2.0f * t) * t * t;
    float3 toneColor = lerp(toeColor, shoulderColor, t);

    // Post-desaturate, the counterpart to the pre-desaturate above.
    toneColor = lerp(dot(toneColor, kLumaWeights).xxx, toneColor, 0.93f);
    return max(toneColor, 0.0f);
}

#endif // FILM_CURVE_HLSLI
