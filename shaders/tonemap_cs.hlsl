#define TONEMAP_CS_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE), SRV(t1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE), SRV(t2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE), SRV(t3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE), SRV(t4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE), UAV(u1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

Texture2D HDRColor : register(t0);
// P3B: blurred base log-luminance, sampled bilinearly. The metering pass writes it and owns both
// of its state transitions, so nothing here needs a barrier.
Texture2D<float> BaseLogLumTex : register(t1);
// P8: mip 0 of the bloom pyramid's UP chain, at HALF this target's resolution -- sampled
// bilinearly, which is the last upsample step and is why it is an SRV here rather than another UAV.
// Bound to an inert 1x1 when bloom is off; a zero `bloomScatterApply` is what disables it.
Texture2D BloomTex : register(t2);
// P3B bilateral grid (tiles x tiles x 32 luminance bins) = (sum log-luminance, sum weight), written
// by exposure_bilateral_cs in the same metering pass as t1 and resting in the same read state.
// Sliced trilinearly at (uv, this pixel's luminance); see local_exposure.hlsli.
Texture3D<float2> BilateralGridTex : register(t3);
// The sun corona's image (BloomSettings::sunRaysTexture), when the corona draws from one. Bound to
// a stand-in when it does not; sunRaysExtra.z = 0 is what keeps it unsampled.
Texture2D SunCoronaTex : register(t4);
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
    // P16.1: the factor every writer of scene colour already applied, and whether it did at all.
    // The flag is separate from the value because a pre-exposure of exactly 1.0 is legal.
    float preExposure;
    uint  preExposureActive;
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
    // P3B local exposure. All scales at 1 = no-op; the shader skips the block entirely.
    float localHighlightContrast;
    float localShadowContrast;
    float localDetailStrength;
    float localHighlightThreshold;
    float localShadowThreshold;
    // P3B bilateral base (2026-09-12). UE BlurredLuminanceBlend: 0 = pure grid, 1 = pure blur
    // (the pre-grid base; also what the host writes when the grid was not built this frame). The
    // two range numbers are the metering pass's own, so the slice lands on the bins it wrote.
    float localBlurredBlend;
    float bilateralMinLogLum;
    float bilateralInvLogLumRange;
    float tonemapPad6;
    // P8C-2o -- UE'S CENTRE/SCATTER SPLIT (BloomFinalizeApplyConstants.usf).
    //
    // The bloom is no longer light ADDED on top of a scene that already has it. A kernel is the
    // lens's whole point spread function: the part of it inside one output pixel is the light that
    // did NOT scatter, and the rest is the flare. So the two terms are a PARTITION --
    //
    //     out = scene * sceneApply + bloom * scatterApply
    //
    // -- with `scatterApply = Tint * saturate(Scatter * s / Total)` and
    // `sceneApply = Tint * saturate((Total - Scatter * s) / Total)`, exactly as their
    // FinalizeApplyConstants writes SceneColorApplyOutput and FFTMulitplyOutput. `s` is their
    // ScatterDispersionIntensity (BloomConvolutionScatterDispersion * BloomIntensity). The two
    // sum to Tint by construction, so pushing `s` up does not create light -- it MOVES it out of
    // the source and into the flare, which is the only way a star ever outshines its own
    // highlight. `Tint` is the kernel's own colour balance, normalised by its largest channel.
    //
    // The pyramid method has no kernel to survey, so it passes sceneApply = 1 and
    // scatterApply = its plain intensity, which is bit-identical to what it did before.
    // Zero scatterApply is the interface contract's exact no-op: no bloom is read at all.
    float3 bloomSceneApply;
    float  tonemapPad4;
    float3 bloomScatterApply;
    float  tonemapPad5;
    // SUN RAYS (BloomSettings::sunRays*), drawn here at OUTPUT resolution because they are
    // sub-degree thin -- the bloom target is a quarter of the screen and would smear them. View
    // space is the camera's (+z forward, +y up). x of sunRaysShape = 0 turns the term off.
    float4 sunRaysDir;     // xyz: direction TO the sun, view space, unit; w: halo weight
    float4 sunRaysProj;    // xy: projection _11/_22; zw: the sun disc's radius in UV
    float4 sunRaysShape;   // x: intensity, y: needle count, z: length (rad), w: regular-star weight
    float4 sunRaysLook;    // x: needle sharpness, y: rotation (rad), z: disc radius (rad), w: star spike count
    float4 sunRaysExtra;   // x: bundle count, y: halo radius (rad), z: 1 = draw from SunCoronaTex, w: its width
};

#include "utils.hlsli"
#include "agx.hlsli"
#include "color_grade.hlsli"
#include "film_curve.hlsli"
#include "tone_curves.hlsli"
#include "local_exposure.hlsli"

// ---- named constants ----
// kGammaOut, TonemapACES and LinearToSrgb moved to tone_curves.hlsli, with the curve selection,
// so the editor's asset preview ends with the same curve as this pass.
static const float kDitherAmplitude = 1.0 / 255.0; // enough to break banding

float SunRayHash(float n)
{
    return frac(sin(n * 12.9898f + 4.1414f) * 43758.5453f);
}

// Periodic value noise on the circle: `x` in cells, `period` cells around, smooth between cells.
float SunRayNoise(float x, float period)
{
    const float i = floor(x);
    const float f = x - i;
    const float a = SunRayHash(i - period * floor(i / period));
    const float i1 = i + 1.0f;
    const float b = SunRayHash(i1 - period * floor(i1 / period));
    return lerp(a, b, f * f * (3.0f - 2.0f * f));
}

// THE SUN'S CORONA -- the glare structure around the sun that the bloom cannot give, modelled on
// Ritschel et al. 2009, "Temporal Glare" (fig. 1: a point source through the eye) rather than on
// stock art: the first two cuts were eight even spikes, then a hedgehog of even rays ("ты где
// такую корону видел?", "не особо реалистично"), and real glare has neither.
//   bundles  a LOW-frequency pattern around the circle: dense bright wedges and dim gaps between
//            them ("в реале там есть плотные участки"); a bright bundle also reaches further.
//   needles  a HIGH-frequency pattern inside them -- hundreds of fine radial needles (the ciliary
//            corona), faded to their mean where a needle would be thinner than a pixel, so the
//            core does not alias into moire.
//   falloff  a power law, not an exponential: dense at the core with a long faint tail. Evaluated
//            per channel at slightly different scales (diffraction grows with wavelength), so the
//            needles' tips go warm and their roots cool.
//   halo     the lenticular halo: a faint ring whose red edge sits outside its blue one.
//   spikes   an optional regular star (eyelashes / an aperture), off by default.
// Everything is in degrees on the sky and the same cells every frame, so nothing crawls. Colour
// and strength are the frame's own pixels over the sun disc (five taps every thread shares): a palm
// or a cloud in front of the sun, or a sunset, takes the corona with it. Analytic rather than a
// texture: the taps are the price and a sprite would need them too.
float3 SunRays(float2 uv, float outputWidth)
{
    const float3 sunDir = sunRaysDir.xyz;
    if (sunDir.z <= 0.02f) { return 0.0f.xxx; }
    const float2 sunNdc = sunDir.xy * sunRaysProj.xy / sunDir.z;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float3 dir = normalize(float3(ndc / sunRaysProj.xy, 1.0f));
    const float theta = acos(clamp(dot(dir, sunDir), -1.0f, 1.0f));
    const float len = max(sunRaysShape.z, 1.0e-4f);
    const float haloR = sunRaysExtra.y;
    // IMAGE MODE: `len` is then the image's half-width on the sky, and the corona is the image.
    const bool fromImage = sunRaysExtra.z > 0.5f;
    if (fromImage && theta > 1.42f * len) { return 0.0f.xxx; }
    // The bound is the whole cost story: only the pixels inside it pay for what follows (measured
    // at 4K with the sun mid-frame: +0.04-0.06 ms; nothing with the sun out of frame). The tail is
    // faded to nothing across its last 30 % so the bound never shows as a circle.
    const float bound = max(12.0f * len, 1.3f * haloR);
    if (theta > bound) { return 0.0f.xxx; }

    const float2 sunUV = float2(0.5f + 0.5f * sunNdc.x, 0.5f - 0.5f * sunNdc.y);
    const float2 discUV = max(sunRaysProj.zw, 1.0e-5f.xx);
    const float2 toEdge = min(sunUV, 1.0f - sunUV) / discUV;
    const float edgeFade = saturate(min(toEdge.x, toEdge.y));
    if (edgeFade <= 0.0f) { return 0.0f.xxx; }

    // Five taps over the disc (every thread reads the same five texels).
    const float2 o = discUV * 0.6f;
    float3 seen = HDRColor.SampleLevel(gSmp, sunUV, 0.0f).rgb +
                  HDRColor.SampleLevel(gSmp, sunUV + float2(o.x, 0.0f), 0.0f).rgb +
                  HDRColor.SampleLevel(gSmp, sunUV - float2(o.x, 0.0f), 0.0f).rgb +
                  HDRColor.SampleLevel(gSmp, sunUV + float2(0.0f, o.y), 0.0f).rgb +
                  HDRColor.SampleLevel(gSmp, sunUV - float2(0.0f, o.y), 0.0f).rgb;
    seen *= 0.2f;
    // THE SUN, NOT THE SKY IN FRONT OF IT: a gate on how much brighter the disc is than a ring 2.5
    // disc radii out. Glare structure comes from a POINT source far brighter than its
    // neighbourhood; an overcast deck over the sun is about as bright on the disc as beside it and
    // must throw nothing -- the disc alone drew rays in a grey sky ("при оверкасте видны лучи").
    // Full strength from 3.5x the ring (a clear sun is 5x or more, its bright aureole included),
    // none at 1.5x (a cloud lit brighter toward the sun by forward scattering stays below it).
    // A gate rather than a subtraction: subtracting the ring took the aureole off a clear sun too.
    const float2 ring = discUV * (2.5f * 0.7071f);
    const float3 around = 0.25f * (HDRColor.SampleLevel(gSmp, sunUV + float2( ring.x,  ring.y), 0.0f).rgb +
                                   HDRColor.SampleLevel(gSmp, sunUV + float2(-ring.x,  ring.y), 0.0f).rgb +
                                   HDRColor.SampleLevel(gSmp, sunUV + float2( ring.x, -ring.y), 0.0f).rgb +
                                   HDRColor.SampleLevel(gSmp, sunUV + float2(-ring.x, -ring.y), 0.0f).rgb);
    const float3 lumaW = float3(0.2126f, 0.7152f, 0.0722f);
    const float contrast = dot(seen, lumaW) / max(dot(around, lumaW), 1.0e-4f);
    seen = max(seen, 0.0f.xxx) * saturate((contrast - 1.5f) * 0.5f);
    if (all(seen <= 0.0f)) { return 0.0f.xxx; }

    // Offset from the sun on the tangent plane (the /proj un-squashes the aspect), and its polar
    // angle in turns [0, 1).
    const float2 d = (ndc - sunNdc) / sunRaysProj.xy;
    const float twoPi = 6.2831853f;
    const float turn = frac((atan2(d.y, d.x) + sunRaysLook.y) / twoPi);
    const float pixel = 2.0f / (sunRaysProj.x * max(outputWidth, 1.0f));   // radians per pixel

    // IMAGE MODE: the texture laid about the sun, `len` radians to either side, turned by the
    // rotation, its mip chosen from texels per pixel (a compute shader has no derivatives). An
    // image made for additive use: black is nothing, its white core sits on the disc.
    if (fromImage)
    {
        const float rot = sunRaysLook.y;
        const float2 local = float2(d.x * cos(rot) + d.y * sin(rot), -d.x * sin(rot) + d.y * cos(rot));
        const float2 tuv = 0.5f + float2(local.x, -local.y) / (2.0f * len);
        if (any(tuv < 0.0f) || any(tuv > 1.0f)) { return 0.0f.xxx; }
        const float texelsPerPixel = max(sunRaysExtra.w, 1.0f) * pixel / (2.0f * len);
        const float lod = max(log2(max(texelsPerPixel, 1.0e-6f)), 0.0f);
        const float image = dot(SunCoronaTex.SampleLevel(gSmp, tuv, lod).rgb, float3(0.2126f, 0.7152f, 0.0722f));
        // Fade the square's own edge, which the image's black margin should already have done.
        const float2 edge = saturate(min(tuv, 1.0f - tuv) * 20.0f);
        return seen * (sunRaysShape.x * image * edge.x * edge.y * edgeFade);
    }

    // Bundles: dense wedges and dim gaps, contrast from squaring; mean ~1 so the knob keeps its scale.
    const float clumps = max(sunRaysExtra.x, 1.0f);
    const float c = SunRayNoise(turn * clumps, clumps);
    const float bundle = 3.0f * c * c;

    // Needles: two octaves, sharpened; resolved only where a needle is wider than about a pixel.
    const float m = max(sunRaysShape.y, 4.0f);
    const float n = 0.65f * SunRayNoise(turn * m, m) + 0.35f * SunRayNoise(turn * m * 3.0f + 0.5f, m * 3.0f);
    const float sharp = max(sunRaysLook.x, 1.0f);
    const float needleMean = 1.0f / (sharp + 1.0f);
    const float resolved = saturate(((twoPi / m) * theta / pixel - 1.0f) * 0.5f);
    const float needle = lerp(needleMean, pow(saturate(n), sharp), resolved) / needleMean;

    // Each needle's reach: its own random length, longer inside a bright bundle.
    const float reach = len * (0.4f + 0.6f * SunRayNoise(turn * m + 0.37f, m)) * (0.6f + 0.4f * bundle);
    const float3 chroma = float3(1.08f, 1.0f, 0.92f);   // red spreads furthest
    const float3 q = theta / (reach * chroma);
    const float3 falloff = 1.0f / (1.0f + q * q * q);
    float3 glare = (bundle * needle) * falloff;

    // The lenticular halo: a thin ring, red outside, blue inside.
    if (sunRaysDir.w > 0.0f && haloR > 0.0f)
    {
        const float3 r = theta - haloR * chroma;
        const float w = haloR * 0.07f;
        glare += sunRaysDir.w * exp(-(r * r) / (w * w)) * (0.7f + 0.6f * c);
    }

    // The optional regular star (eyelashes, an aperture), a pixel and a half wide.
    if (sunRaysShape.w > 0.0f)
    {
        const float k = max(sunRaysLook.w, 1.0f);
        const float t = turn * k;
        const float across = abs(t - round(t)) * (twoPi / k) * theta;
        const float w = max(1.5f * pixel, 3.0e-4f) * (1.0f + theta / len * 0.1f);
        glare += sunRaysShape.w * exp(-(across * across) / (w * w)) / (1.0f + theta / (4.0f * len));
    }

    // Rays from UNDER the disc's edge: fading in from one disc radius to two left a dark ring
    // between the disc and the corona ("зазор между диском и стартом лучей").
    const float rim = smoothstep(0.5f * sunRaysLook.z, sunRaysLook.z, theta);
    const float tail = 1.0f - smoothstep(0.7f * bound, bound, theta);
    return seen * (sunRaysShape.x * glare * rim * tail * edgeFade);
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
    // Kept so P3B can shift the base-luminance texture into the same space as this pixel: that
    // texture stores the UNEXPOSED scene's log luminance, and comparing the two directly would make
    // the detail term pure error.
    float exposureMultiplier = 1.0f;
    // P16.1: WHEN THE WRITERS HAVE ALREADY APPLIED THE EXPOSURE, THIS PASS APPLIES NOTHING.
    //
    // The first attempt divided by the pre-exposure here instead, and it did not cancel: the
    // writers used a CPU factor built from the exposure READBACK while this read the exposure from
    // the GPU buffer, and those are the same quantity at different ages. Measured, the sky moved
    // 50/255 with the gate on when it should not have moved at all. There is nothing to reconcile
    // between two numbers -- there should only ever be one.
    if (preExposureActive != 0)
    {
        // exposureMultiplier stays 1.0.
    }
    else if (exposureEnabled != 0)
    {
        const float ev100 = asfloat(ExposureValue.Load(0));
        if (!isnan(ev100) && !isinf(ev100))
        {
            exposureMultiplier = (0.18f * 8.0f) / exp2(ev100);
            hdr *= exposureMultiplier;
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

    // P3B: local exposure. After the global exposure and the grade, before the curve -- the curve
    // still needs a scene-referred image for its 0.18 fixed point to mean anything.
    {
        LocalExposureParams local;
        local.highlightContrastScale = localHighlightContrast;
        local.shadowContrastScale = localShadowContrast;
        local.detailStrength = localDetailStrength;
        local.blurredBlend = localBlurredBlend;
        local.highlightThreshold = localHighlightThreshold;
        local.shadowThreshold = localShadowThreshold;

        if (!LocalExposureIsNeutral(local))
        {
            const float lum = dot(hdr, float3(0.2126f, 0.7152f, 0.0722f));
            if (lum > 1e-8f)
            {
                // Shift the stored (unexposed) base by the exposure this pixel received, so base
                // and pixel live in the same space. The grade is NOT modelled here: it is a colour
                // operation on top, and folding it in would mean re-deriving a blurred field per
                // grade change for a base layer that is deliberately coarse.
                // P16.1: the shift is the exposure THE PIXEL ACTUALLY RECEIVED, which is not the
                // same as what this pass applied. Pre-exposed, the writers applied it and this pass
                // applied 1.0, while the base layer is stored unexposed either way -- so using
                // `exposureMultiplier` alone put base and pixel a whole exposure apart and the
                // local operator pulled the frame down by up to 10/255.
                const float totalExposure =
                    exposureMultiplier * ((preExposureActive != 0) ? preExposure : 1.0f);
                const float logExposure = log2(max(totalExposure, 1e-8f));
                const float logLum = log2(lum);
                // The grid is sliced at the pixel's UNEXPOSED luminance, the space it was built
                // in; the blend and the fallback to the blur are UE's CalculateBaseLogLuminance.
                const float baseLog = LocalExposureBaseLogLum(BilateralGridTex, gSmp, uv,
                                          logLum - logExposure,
                                          bilateralMinLogLum, bilateralInvLogLumRange,
                                          BaseLogLumTex.SampleLevel(gSmp, uv, 0),
                                          local.blurredBlend)
                                    + logExposure;
                hdr *= LocalExposureMultiplier(logLum, baseLog, log2(0.18f), local);
            }
        }
    }

    // P8: bloom, added AFTER the grade and the local exposure and BEFORE the curve. That placement
    // is UE's, and both halves of it matter (PostProcessTonemap.usf):
    //
    //     FinalLinearColor  = SceneColor * SceneColorTint * (GlobalExposure * ... * LocalExposure);
    //     FinalLinearColor += Bloom * (GlobalExposure * ...);
    //
    // The bloom gets the GLOBAL exposure -- it must, it was extracted from an image that had not
    // been exposed yet -- but NOT the local one and not the colour grade: local exposure is a
    // per-pixel contrast operator derived from THIS pixel's neighbourhood, and a halo that spread
    // from somewhere else is not part of that neighbourhood. Before the curve, because bloom is
    // scene-referred light and the curve is what maps scene-referred light to the display.
    if (any(bloomScatterApply > 0.0f))
    {
        const float3 bloom = BloomTex.SampleLevel(gSmp, uv, 0).rgb;
        // The scene is scaled FIRST and by its own factor: this is a partition of the light, not
        // an addition to it. With the pyramid's sceneApply of 1 the line reduces to the old `+=`.
        hdr = hdr * bloomSceneApply + bloom * (bloomScatterApply * exposureMultiplier);
    }
    // The sun's star, beside the bloom and in the same units: global exposure, no local one.
    if (sunRaysShape.x > 0.0f)
    {
        hdr += SunRays(uv, (float)width) * exposureMultiplier;
    }

    // P3C film curve / AgX / legacy ACES -- the selection lives in tone_curves.hlsli, shared with
    // the editor's asset preview so the two end with the same curve.
    FilmCurveParams film;
    film.slope = filmSlope;
    film.toe = filmToe;
    film.shoulder = filmShoulder;
    film.blackClip = filmBlackClip;
    film.whiteClip = filmWhiteClip;
    float3 ldr = ToneCurveToDisplay(hdr, toneCurve, film, agxSlope, agxPower, agxSaturation);

    // Optional: add identical noise to every channel — sufficient to break banding
    //float d = Dither(dispatchThreadId.xy) * kDitherAmplitude;
    //ldr += d;

    LdrTarget[dispatchThreadId.xy] = float4(saturate(ldr), 1.0);
}
