#ifndef AGX_HLSLI
#define AGX_HLSLI

// AgX display transform (photographic plan, step P3).
//
// Why AgX over the Narkowicz ACES fit we shipped before: the fit skews hue as it approaches
// clipping (saturated blues and cyans slide toward white and pick up a magenta cast) and it hard
// clips, which is visible on exactly our content -- bright cyan lagoon water and warm sand. AgX
// instead attenuates chroma progressively while a channel climbs, so a highlight desaturates the
// way film does instead of snapping to a flat colour. That chroma attenuation IS the plan's
// "controlled highlight desaturation / gamut compression" requirement (P3 item 5); it is built
// into the transform rather than bolted on afterwards.
//
// Structure, in order:
//   1. inset matrix                          (this is what does the chroma attenuation)
//   2. log2 encode over a fixed EV window -> [0,1]
//   3. sigmoid contrast curve                (polynomial fit of the reference curve)
//   4. optional "look" (slope / power / saturation)
//   5. outset matrix, then 2.2 EOTF back to linear
//
// Step 5 returns LINEAR display-referred colour on purpose: the sRGB OETF is applied once at the
// very end of the tonemap shader, so there is exactly one place that encodes for the display.
//
// NOTE there are two AgX variants in circulation and they are NOT interchangeable: the three.js
// one converts to linear Rec.2020 first and uses inset/outset matrices fitted for that space, and
// the "minimal AgX" one below folds everything into matrices that act directly on linear sRGB.
// Doing both -- a Rec.2020 conversion AND the sRGB-space inset -- transforms the primaries twice
// and tints the entire frame pink. This file uses the sRGB-space variant, no Rec.2020 anywhere.

// AgX inset: pulls the primaries inward so a channel approaching the top of the curve loses chroma
// instead of hue-shifting. The outset is its (deliberately partial) inverse.
static const float3x3 kAgxInset = float3x3(
    0.8424790f, 0.0784336f, 0.0792237f,
    0.0423282f, 0.8784686f, 0.0791661f,
    0.0423756f, 0.0784336f, 0.8791430f);

// NOTE both matrices are written ROW-major, because HLSL's float3x3(...) constructor fills rows
// and mul(M, v) dots each ROW with v. The reference implementations are GLSL, whose mat3(...)
// fills COLUMNS -- so transcribing their literals in order silently transposes them. That is not a
// harmless error here: inset and outset are not symmetric, so a transposed outset stops being the
// inset's inverse and leaves a residual colour transform. Concretely, it pushed neutral white to
// (1.091, 0.956, 0.953) and tinted the whole frame pink.
// Guard when touching these: inset * outset must be the identity, and white must stay white.
static const float3x3 kAgxOutset = float3x3(
     1.1968790f, -0.0980208f, -0.0990297f,
    -0.0528969f,  1.1519031f, -0.0989612f,
    -0.0529716f, -0.0980435f,  1.1510737f);

// The reference AgX log window. Note it is asymmetric: about 12.5 stops of shadow latitude against
// 4 of highlight, which is why the curve has so much more room below middle grey than above.
static const float kAgxMinEv = -12.47393f;
static const float kAgxMaxEv = 4.026069f;

// 6th-order polynomial fit of the AgX sigmoid on [0,1].
float3 AgxContrastApprox(float3 x)
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return  15.5f     * x4 * x2
          - 40.14f    * x4 * x
          + 31.96f    * x4
          - 6.868f    * x2 * x
          + 0.4298f   * x2
          + 0.1191f   * x
          - 0.00232f;
}

// ASC-CDL style grade in the AgX log domain plus a saturation term. slope/power/saturation of
// (1,1,1) is a no-op; the reference "punchy" look is roughly (1.0, 1.35, 1.4).
float3 AgxApplyLook(float3 val, float slope, float power, float saturation)
{
    const float luma = dot(val, float3(0.2126f, 0.7152f, 0.0722f));
    val = pow(max(val * slope, 0.0f), power);
    return luma + saturation * (val - luma);
}

// Scene-referred linear sRGB -> display-referred LINEAR sRGB (NOT encoded; the caller applies the
// sRGB OETF once, at the end).
float3 AgxTonemap(float3 color, float slope, float power, float saturation)
{
    color = mul(kAgxInset, max(color, 0.0f));

    color = clamp(log2(max(color, 1e-10f)), kAgxMinEv, kAgxMaxEv);
    color = (color - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv);
    color = AgxContrastApprox(saturate(color));

    color = AgxApplyLook(color, slope, power, saturation);

    color = mul(kAgxOutset, color);
    // 2.2 EOTF: the sigmoid's output is display-encoded, so this returns it to display LINEAR.
    // It is intentionally NOT the inverse of the sRGB OETF applied later -- that near-miss (sRGB's
    // linear toe near black) is part of the reference transform, not a bug to "fix".
    color = pow(max(color, 0.0f), 2.2f);
    return max(color, 0.0f);
}

#endif // AGX_HLSLI
