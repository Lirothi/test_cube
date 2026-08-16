#define TONEMAP_CS_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE), UAV(u1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

Texture2D HDRColor : register(t0);
RWTexture2D<float4> LdrTarget : register(u0);
// P2: the persistent exposure record, read-only here. Bound as a UAV rather than an SRV purely so
// it never leaves its canonical UNORDERED_ACCESS state -- an SRV binding would cost a transition
// down and back every frame for 16 bytes nobody writes in this pass.
// This runs AFTER the DLSS resolve (the upscaler evaluates earlier in this same pass) and BEFORE
// the tone curve, which is the ordering the plan's section 6.3 fixes. NGX keeps its own internal
// auto-exposure -- nothing here is handed to it.
RWByteAddressBuffer ExposureValue : register(u1);
SamplerState gSmp : register(s0);

cbuffer TonemapCB : register(b0)
{
    // 0 = dormant. Kept as an explicit flag rather than writing a neutral EV into the buffer so
    // the disabled path multiplies by a literal 1.0 and is bit-identical to the pre-plan image.
    uint exposureEnabled;
    // P3: 0 = legacy (Narkowicz ACES fit + pow(1/2.2)), 1 = AgX + sRGB transfer.
    // Legacy is bit-identical to the pre-P3 image and exists so any regression can be A/B'd
    // against the curve rather than argued about.
    uint toneCurve;
    uint tonemapPad0, tonemapPad1;
    // AgX look: slope / power / saturation. (1,1,1) is neutral.
    float agxSlope;
    float agxPower;
    float agxSaturation;
    float tonemapPad2;
    // P3C colour grade, applied in linear BEFORE the curve. Neutral defaults are a no-op.
    float gradeSaturation;
    float gradeContrast;
    float gradeGamma;
    float gradeGain;
    float gradeOffset;
    // P3C film curve (Unreal's five controls). Only read by toneCurve == 2.
    float filmSlope;
    float filmToe;
    float filmShoulder;
    float filmBlackClip;
    float filmWhiteClip;
    float tonemapPad3, tonemapPad4, tonemapPad5;
};

#include "utils.hlsli"
#include "agx.hlsli"
#include "color_grade.hlsli"
#include "film_curve.hlsli"

// ---- named constants ----
static const float kGammaOut = 2.2;
static const float kDitherAmplitude = 1.0 / 255.0; // enough to break banding

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

// Stable, cheap hash based on the pixel coordinate
float Dither(uint2 p)
{
    float n = frac(sin(dot(float2(p), float2(12.9898, 78.233))) * 43758.5453);
    return n - 0.5; // [-0.5, 0.5)
}

[numthreads(8,8,1)]
[RootSignature(TONEMAP_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width, height;
    LdrTarget.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(width, height);
    float3 hdr = HDRColor.SampleLevel(gSmp, uv, 0).rgb;

    // Exposure applied exactly once, here, immediately before the tone curve.
    // Mirrors render::ExposureMultiplierFromEv100: m = kMiddleGrey * (S/K) / 2^EV100, with
    // kMiddleGrey = 0.18 and S/K = 8. If that changes, this changes with it.
    if (exposureEnabled != 0)
    {
        const float ev100 = asfloat(ExposureValue.Load(0));
        if (!isnan(ev100) && !isinf(ev100))
        {
            hdr *= (0.18f * 8.0f) / exp2(ev100);
        }
    }

    // Grade in linear, before the curve -- the same place Unreal bakes it into its LUT. After the
    // curve you would be grading display code values, where contrast and saturation stop behaving
    // predictably because the range has already been compressed.
    {
        ColorGradeParams grade;
        grade.saturation = gradeSaturation;
        grade.contrast = gradeContrast;
        grade.gamma = gradeGamma;
        grade.gain = gradeGain;
        grade.offset = gradeOffset;
        if (!ColorGradeIsNeutral(grade))
        {
            hdr = ApplyColorGrade(hdr, grade);
        }
    }

    float3 ldr;
    if (toneCurve == 2)
    {
        // P3C: Unreal's parameterised film curve. Returns display-referred linear, so the sRGB
        // OETF below stays the single place the display encoding happens.
        FilmCurveParams film;
        film.slope = filmSlope;
        film.toe = filmToe;
        film.shoulder = filmShoulder;
        film.blackClip = filmBlackClip;
        film.whiteClip = filmWhiteClip;
        ldr = LinearToSrgb(FilmCurveToneMap(hdr, film));
    }
    else if (toneCurve == 1)
    {
        // AgX returns display-referred LINEAR, so the display encoding happens in exactly one
        // place: the sRGB OETF below.
        ldr = LinearToSrgb(AgxTonemap(hdr, agxSlope, agxPower, agxSaturation));
    }
    else
    {
        // Legacy path, byte-for-byte what shipped before P3.
        ldr = LinearToGamma(TonemapACES(hdr), kGammaOut);
    }

    // Optional: add identical noise to every channel — sufficient to break banding
    //float d = Dither(dispatchThreadId.xy) * kDitherAmplitude;
    //ldr += d;

    LdrTarget[dispatchThreadId.xy] = float4(saturate(ldr), 1.0);
}
