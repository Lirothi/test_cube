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
// down and back every frame for 48 bytes this dispatch never writes. Behind the exposure record it
// carries the sun probe (sun_probe.hlsli), written by one thread earlier in this same pass.
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
    float4 sunRaysLook;    // x: star spike length (rad), y: rotation (rad), z: disc radius (rad), w: star spike count
    float4 sunRaysExtra;   // x: bundle count, y: halo radius (rad), z: 1 = draw from SunCoronaTex, w: its width
    float4 sunRaysWidth;   // x: needle width, y: star spike width -- each the Gaussian's 1/e half-width (rad);
                           // z: bundle width (turns: a raised cosine's half-width, which is its FWHM);
                           // w: bundle strength 0-1 (how dark the gaps and how gathered the needles)
};

#include "utils.hlsli"
#include "agx.hlsli"
#include "color_grade.hlsli"
#include "film_curve.hlsli"
#include "tone_curves.hlsli"
#include "local_exposure.hlsli"
#include "sun_probe.hlsli"

// ---- named constants ----
// kGammaOut, TonemapACES and LinearToSrgb moved to tone_curves.hlsli, with the curve selection,
// so the editor's asset preview ends with the same curve as this pass.
static const float kDitherAmplitude = 1.0 / 255.0; // enough to break banding

// PCG: a needle's or a bundle's random numbers are the 10-bit fields of one hash.
uint SunRayHashU(uint n)
{
    const uint state = n * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The corona proper -- bundles, needles and the lenticular halo -- `theta` radians from the sun
// and `turn` turns round it; see SunRays.
float3 SunCoronaGlare(float theta, float turn, float pixel, float len, float haloR)
{
    const float twoPi = 6.2831853f;
    // Bundles: one to a cell round the circle at a random angle inside it, each a raised-cosine bump
    // of its own brightness whose FWHM is the Bundle Width (sunRaysWidth.z, turns). The first cut
    // was value noise, whose wedges were always as wide as their spacing, so narrow bundles took 48
    // of them ("ширину бандлов надо тоже задавать"). Only the three nearest are summed, so a bundle
    // is at most ~one spacing wide.
    //   A bundle both BRIGHTENS the needles and GATHERS them, by one knob, the Bundle Strength g
    //   (sunRaysWidth.w): the gaps are lit at lerp(1, 0.05, sqrt g) of a bundle's peak, and the
    //   needles are laid out evenly in the running integral of a density that is gapDensity between
    //   bundles and 1 more at a bundle's peak. Brightening alone (needles even round the circle) lit
    //   only the ~15 of 100 needles that crossed 12 bundles, and a full corona took hundreds;
    //   gathering alone collapsed a small corona into one beam per bundle, where the needle count
    //   no longer showed ("сделай чтобы не нужно было такое безумное кол-во бандлов и иголок").
    //   Moving both together also keeps the light roughly constant: the brightness is taken where
    //   the needles are. The integral of a raised cosine is closed-form; the bundles before the
    //   three nearest contribute their whole mass, hwB each.
    const float nB = max(sunRaysExtra.x, 1.0f);
    const float hwB = clamp(sunRaysWidth.z, 1.0e-4f, 1.1f / nB);
    const float jb0 = floor(turn * nB);
    float bumps = 0.0f;
    float bumpsLit = 0.0f;
    float bumpMass = hwB * (jb0 - 1.0f);
    [unroll] for (int b = -1; b <= 1; ++b)
    {
        const float jb = jb0 + (float)b;
        const uint bits = SunRayHashU((uint)(jb - nB * floor(jb / nB)) + 7919u);
        const float2 hb = float2(bits & 1023u, (bits >> 10) & 1023u) * (1.0f / 1023.0f);
        const float x = clamp(turn - (jb + 0.5f + 0.7f * (hb.x - 0.5f)) / nB, -hwB, hwB);
        float s, c;
        sincos(3.14159265f * x / hwB, s, c);
        const float bump = 0.5f + 0.5f * c;                      // 0 at and past +-hwB
        bumps += bump;
        bumpsLit += bump * (0.5f + 0.5f * hb.y);
        bumpMass += 0.5f * (x + hwB) + hwB * s * (1.0f / twoPi);  // 0 before, hwB after
    }
    const float strength = saturate(sunRaysWidth.w);
    const float gapLit = lerp(1.0f, 0.05f, sqrt(strength));
    const float gapDensity = 0.5f * (1.0f - strength) / max(strength, 1.0e-3f);
    const float mass = nB * hwB;                   // all the bumps' integral round the circle
    const float densityTotal = gapDensity + mass;
    // Divided by its mean where the needles ARE (a raised cosine integrates to hw, its square to
    // 0.75 hw; a bundle's own brightness averages 0.75), so the strength moves light between the
    // bundles and the gaps rather than adding it: measured, not dividing let strength 0 throw 4.5x
    // the light of strength 1.
    const float litMean = (gapDensity * gapLit + mass * (0.75f * (1.0f - gapLit) * (gapDensity + 0.75f) + gapLit))
                          / densityTotal;
    const float bundle = (gapLit + (1.0f - gapLit) * min(bumpsLit, 1.5f)) / litMean;

    // Needles: LINES of a set width on the sky, not wedges of a set angle. The first cut was a
    // pattern around the circle, so a needle was a wedge that widened with distance, no knob set
    // its thickness ("толщина лучей непонятно как регулируется"), and past what fit side by side
    // more needles only aliased ("выше 150 не выглядит что их реально больше"). Needle j sits at a
    // random place inside its cell of the bundles' integral (so mostly inside a bundle) with its
    // own brightness and length (a Gaussian across, the power law along); only the three nearest
    // are summed, across-distances linearised by the local needles-per-turn. A needle is never
    // drawn thinner than a pixel: a thinner one is drawn a pixel wide and correspondingly fainter.
    const float m = max(sunRaysShape.y, 4.0f);
    const float w = max(sunRaysWidth.x, 1.0e-6f);
    const float wDrawn = max(w, 0.6f * pixel);
    // Where this pixel is among the needles, and how many there are to a turn here.
    const float cells = (gapDensity * turn + bumpMass) * (m / densityTotal);
    const float perTurn = max((gapDensity + bumps) * (m / densityTotal), 1.0e-3f);
    const float cell = twoPi * theta / perTurn;    // radians between neighbouring needles, here
    // 1 where the needles stand apart, 0 where they are packed as close as their width. Not below
    // twice the width for "apart": closer than that the needle dropped from the three nearest as the
    // pixel crosses a cell edge still showed, as a dotted seam along a ray.
    const float apart = saturate(cell / wDrawn - 1.0f);
    // The power law per channel from one scalar: (theta / reach)^3 times each channel's 1/chroma^3,
    // chroma (1.08, 1, 0.92) -- red spreads furthest. A bright bundle reaches further.
    const float3 chromaInvCube = float3(0.7938f, 1.0f, 1.2842f);
    const float qBase = theta / (len * (0.6f + 0.4f * bundle));
    const float qBase3 = qBase * qBase * qBase;
    float3 lines = 0.0f.xxx;
    if (apart > 0.0f)
    {
        [unroll] for (int k = -1; k <= 1; ++k)
        {
            const float j = floor(cells) + (float)k;
            const uint bits = SunRayHashU((uint)(j - m * floor(j / m)));   // wraps round the circle
            const float3 h = float3(bits & 1023u, (bits >> 10) & 1023u, (bits >> 20) & 1023u) * (1.0f / 1023.0f);
            const float across = (cells - (j + 0.5f + 0.7f * (h.x - 0.5f))) * cell / wDrawn;
            // Its length: 0.4-1.0 of the scale, and the top tenth up to twice as far (h^8).
            const float h2 = h.y * h.y;
            const float h4 = h2 * h2;
            const float reach = 0.4f + 0.6f * h.y + 1.2f * h4 * h4;
            const float3 q3 = (qBase3 / (reach * reach * reach)) * chromaInvCube;
            lines += (0.35f + 0.65f * h.z) * exp(-across * across) / (1.0f + q3);
        }
        lines *= w / wDrawn;
    }
    float3 merged = 0.0f.xxx;
    if (apart < 1.0f)
    {
        // Packed closer than their width they ARE a glow, which the three nearest no longer add up
        // to: their mean -- the comb's coverage sqrt(pi) w / cell times the mean brightness and the
        // fall-off averaged over the spread of lengths (five-point quadrature: 1/reach^3 at h = 0.1,
        // 0.35, 0.6, 0.85, 0.97). The coverage is softly capped at 3 as if the needles were even
        // round the circle (it would grow without bound toward the disc), THEN scaled by how
        // gathered they are here -- capping the local one took the light out of a gathered bundle.
        // Also what a count too high to show becomes: glow, not moire.
        static const float kInvReach3[5] = { 10.2737f, 4.3998f, 2.106f, 0.5283f, 0.1407f };
        static const float kWeight[5] = { 0.22f, 0.25f, 0.25f, 0.2f, 0.08f };
        float3 fallMean = 0.0f.xxx;
        [unroll] for (int s = 0; s < 5; ++s)
        {
            fallMean += kWeight[s] / (1.0f + (qBase3 * kInvReach3[s]) * chromaInvCube);
        }
        float cover = 1.7724539f * w * m / max(twoPi * theta, 1.0e-9f);
        cover /= 1.0f + cover / 3.0f;
        merged = (0.675f * cover * perTurn / m) * fallMean;
    }
    float3 glare = bundle * lerp(merged, lines, apart);

    // The lenticular halo: a thin ring, red outside, blue inside.
    if (sunRaysDir.w > 0.0f && haloR > 0.0f)
    {
        const float3 r = theta - haloR * float3(1.08f, 1.0f, 0.92f);
        const float ringW = haloR * 0.07f;
        glare += sunRaysDir.w * exp(-(r * r) / (ringW * ringW)) * (0.7f + 0.6f * saturate(bumps));
    }
    return glare;
}

// THE SUN'S CORONA -- the glare structure around the sun that the bloom cannot give, modelled on
// Ritschel et al. 2009, "Temporal Glare" (fig. 1: a point source through the eye) rather than on
// stock art: the first two cuts were eight even spikes, then a hedgehog of even rays ("ты где
// такую корону видел?", "не особо реалистично"), and real glare has neither.
//   bundles  bright wedges of a set angular width at random angles, dim gaps between them ("в реале
//            там есть плотные участки"); a bright bundle also reaches further.
//   needles  radial LINES all round (the ciliary corona), lit by the bundles, each of a set width
//            on the sky, its own brightness and its own length -- most short, a few long. Where
//            they are packed closer than their width they are drawn as the glow they merge into.
//   falloff  a power law, not an exponential: dense at the core with a long faint tail. Evaluated
//            per channel at slightly different scales (diffraction grows with wavelength), so the
//            needles' tips go warm and their roots cool.
//   halo     the lenticular halo: a faint ring whose red edge sits outside its blue one.
//   spikes   an optional regular star (eyelashes / an aperture), off by default.
// Everything is in degrees on the sky and the same cells every frame, so nothing crawls. Colour
// and strength are the frame's own pixels over the sun disc, metered once by the sun probe
// (sun_probe.hlsli): a palm or a cloud in front of the sun, or a sunset, takes the corona with it,
// and a sun just off the frame keeps throwing it in, fading with the angle it has left by.
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
    // The bounds are the whole cost story: only the pixels inside them pay for what follows
    // (measured at 4K with the sun mid-frame: +0.02-0.07 ms; nothing with the sun out of frame).
    // The corona and the star have their own, so a long star does not make the needles run over
    // its whole reach. Each tail is faded to nothing across its last 30 % so no bound shows as a
    // circle.
    const float coronaBound = max(12.0f * len, 1.3f * haloR);
    const float spikeLen = max(sunRaysLook.x, 1.0e-4f);
    const float spikeBound = sunRaysShape.w > 0.0f ? 3.5f * spikeLen : 0.0f;
    if (theta > max(coronaBound, spikeBound)) { return 0.0f.xxx; }

    // The sun disc's colour and visibility -- metered once a frame by the sun probe
    // (bloom_conv_cs.hlsl stage 10), held while the disc is off the frame and faded by how far off,
    // gated against an overcast sky.
    const float3 seen = asfloat(ExposureValue.Load3(kSunProbeOut));
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
        return seen * (sunRaysShape.x * image * edge.x * edge.y);
    }

    // The corona proper, inside its own bound.
    const float coronaFade = 1.0f - smoothstep(0.7f * coronaBound, coronaBound, theta);
    float3 glare = 0.0f.xxx;
    [branch] if (coronaFade > 0.0f)
    {
        glare = SunCoronaGlare(theta, turn, pixel, len, haloR) * coronaFade;
    }

    // The optional regular star (eyelashes, an aperture): its own width (no thinner than a pixel,
    // as the needles) and its own length, the angle where it is at half -- it was pinned to 4x the
    // corona's. Falls as 1 / (1 + (theta / length)^2): a tenth at three lengths, faded out by 3.5.
    if (sunRaysShape.w > 0.0f && theta <= spikeBound)
    {
        const float spikes = max(sunRaysLook.w, 1.0f);
        const float at = turn * spikes;
        const float ws = max(sunRaysWidth.y, 1.0e-6f);
        const float wsDrawn = max(ws, 0.6f * pixel);
        const float across = abs(at - round(at)) * (twoPi / spikes) * theta / wsDrawn;
        const float along = theta / spikeLen;
        const float spikeFade = 1.0f - smoothstep(0.7f * spikeBound, spikeBound, theta);
        glare += (sunRaysShape.w * ws / wsDrawn * spikeFade) * exp(-across * across) / (1.0f + along * along);
    }

    // Rays from UNDER the disc's edge: fading in from one disc radius to two left a dark ring
    // between the disc and the corona ("зазор между диском и стартом лучей").
    const float rim = smoothstep(0.5f * sunRaysLook.z, sunRaysLook.z, theta);
    return seen * (sunRaysShape.x * glare * rim);
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
