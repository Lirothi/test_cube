#ifndef COLOR_GRADE_HLSLI
#define COLOR_GRADE_HLSLI

// Colour grading (photographic plan, step P3C).
//
// This is the half of Unreal's film pipeline that actually produces the "juicy" look. Their tone
// curve and this grade are baked together into a 3D LUT (`CombineLUTs` -> `ColorGradingLUT`); the
// curve alone is neutral-ish, and every bit of punch an artist dials in comes from here.
//
// Applied in scene-referred LINEAR space, BEFORE the tone curve -- same place UE applies it. Doing
// it after the curve would be grading display code values, where contrast and saturation stop
// behaving predictably because the curve has already compressed the range.
//
// Order matches the standard (UE's ColorCorrect, and ASC CDL for the last two):
//   saturation -> contrast (pivoted on middle grey) -> gamma -> gain -> offset
//
// Every parameter is neutral at its default, so a scene that grades nothing is bit-identical to no
// grading at all -- see ColorGradeIsNeutral.

struct ColorGradeParams
{
    float saturation; // 1 = unchanged, 0 = greyscale, >1 = more chroma
    float contrast;   // 1 = unchanged; pivots on 0.18 so midtones stay put
    float gamma;      // 1 = unchanged
    float gain;       // 1 = unchanged, a plain multiplier
    float offset;     // 0 = unchanged, a plain lift
};

// Middle grey, the pivot the contrast term rotates around. Same constant the exposure solve
// targets (render::kMiddleGrey) -- if one moves, so does the other, or contrast starts pivoting
// somewhere the camera is not aiming.
static const float kGradeMiddleGrey = 0.18f;

bool ColorGradeIsNeutral(ColorGradeParams p)
{
    return p.saturation == 1.0f && p.contrast == 1.0f && p.gamma == 1.0f
        && p.gain == 1.0f && p.offset == 0.0f;
}

float3 ApplyColorGrade(float3 color, ColorGradeParams p)
{
    color = max(color, 0.0f);

    // Saturation around luma. Rec.709 weights: the same ones the metering uses, so "luma" means
    // one thing across the whole pipeline.
    const float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    color = max(lerp(luma.xxx, color, p.saturation), 0.0f);

    // Contrast pivoted on middle grey rather than on zero. Pivoting on zero would darken
    // everything as contrast rises (which is exactly the trap AgX's `power` look fell into).
    color = pow(color / kGradeMiddleGrey, p.contrast) * kGradeMiddleGrey;

    color = pow(max(color, 0.0f), 1.0f / max(p.gamma, 1e-4f));
    color = color * p.gain + p.offset;

    return max(color, 0.0f);
}

#endif // COLOR_GRADE_HLSLI
