#define BLOOM_CONV_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// P8C / P8C-2 -- the non-transform stages of convolution bloom. The transform itself is
// bloom_fft_cs.hlsl, which since P8C-2 also owns the Hermitian spectral multiply.
//
//   stage 0 SETUP    HDR frame -> the padded complex grid, through UE's FilterPixel, downscaled.
//   stage 3 KERNEL   the PHOTOGRAPHED kernel image resampled into the grid, centre folded to the
//                    DC corner. P8C-2: this replaced the generated aperture -- a 2-wavelength
//                    |FT{aperture}|^2 is sinc^2 interference WITH ZEROS, physically dashed at any
//                    single wavelength; the photograph integrates the whole visible spectrum and
//                    is smooth, and its rainbow dispersion comes for free.
//   stages 4-7 STREAK  the anamorphic streak as an anisotropic PYRAMID (P8C-2h), the structure
//                    KinoStreak uses: prefilter, horizontal-only downsample chain, upsample with
//                    per-level weights, composite. Nothing about it is part of the FFT kernel.
//   stage 2 RESOLVE  the convolved grid -> mip 0 of the bloom UP chain, plus the ghost composite.
//
// COMPLEX PACKING, shared with the transform: `.xy` = R + iG, `.zw` = B + i0. Since P8C-2 the
// kernel is COLOURED, so the frequency-domain product unpacks the lanes at (k, -k) -- see
// bloom_fft_cs.hlsl.
//
// ZERO PADDING IS NOT OPTIONAL. An FFT computes the CIRCULAR convolution, so without a border of
// zeros a streak leaving the right edge of the image reappears on the left. The image occupies
// `imageSize` of a larger `transformSize`; everything outside it is zero, and that margin is the
// pad. UE hit the same wall and CLAMP the pad when the transform would exceed their 4096 limit,
// accepting the wrap because the kernel's tails are faint (PostProcessFFTBloom.cpp) -- the same
// acceptance this grid makes for a full-viewport kernel in a 25% pad.

Texture2D HDRColor : register(t0);
// P8C-2: the scattered-bokeh flare image (lens_flare.hlsl writes it), read by the resolve's ghost
// composite. UE's LensFlareBlur output -- the actual defocused image of the actual bright
// sources, with a x2 guard band folded in by the scatter. The sun probe (stage 10) binds the scene
// before the upscaler here instead.
Texture2D FlareBlur : register(t1);
// The photographed convolution kernel (UE's DefaultBloomKernel), FP16 with a mip chain. The mips
// stand in for UE's successive-downsample prefilter when the kernel is drawn small.
Texture2D BloomKernel : register(t2);
// Local exposure's base (the tonemap's t3 / t1): UE apply local exposure to the FFT's input
// (PostProcessFFTBloom.cpp:826-850, AddApplyLocalExposurePass), so the bloom is spread from the
// scene as the tone curve will show it. Stage 0 only; bound on every stage, the table is positional.
Texture3D<float2> LocalExposureGrid : register(t3);
Texture2D<float> LocalExposureBase : register(t4);
RWTexture2D<float4> Grid : register(u0);        // the complex grid, transformSize
RWTexture2D<float4> BloomOut : register(u1);    // mip 0 of the bloom UP chain, resolve only
SamplerState gSmp : register(s0);

#include "sun_probe.hlsli"
#include "local_exposure.hlsli"

cbuffer BloomConvCB : register(b0)
{
    uint  convStage;        // 0 setup, 2 resolve, 3 kernel resample, 4-7 anamorphic streak, 8 ghosts, 9 sun glare, 10 sun probe, 11 pixel probe, 12 temporal input
    uint  exposureEnabled;
    uint2 transformSize;    // the ACTIVE padded power-of-two grid (may be a sub-grid of the texture)
    uint2 imageSize;        // how much of it the frame occupies; the rest is the zero pad
    uint2 sourceSize;       // the HDR image being read (setup) or written (resolve)
    // UE's BloomConvolutionPreFilterMin/Max/Mult, in the PRE-EXPOSED units the buffers hold (UE's
    // FilterPixel runs on the pre-exposed scene colour). Active when Min < Max and Mult > 0.
    float preFilterMin;
    float preFilterMax;
    float preFilterMult;
    // P8C-2 kernel placement. `kernelSpanTexels` is how many GRID texels the kernel photograph's
    // full width covers: convSize (UE's BloomConvolutionSize, a fraction of the viewport) times
    // the image's major axis -- UE's own rule (PostProcessFFTBloom.cpp, KernelSupportScale).
    // P8C-2h: MINIFICATION IS FILTERED ON DEMAND, not from baked mips. The kernel asset carries
    // mip 0 only -- UE's does too, and they build a downsample chain at runtime instead
    // (FBloomDownsampleKernelCS). We take the box directly here because this stage already knows
    // the exact ratio: `kernelBoxTaps` samples per axis, `kernelBoxStep` apart in kernel UV.
    // 1 tap = magnification, the common case (at convSize 1.0 the kernel is UPSCALED).
    float kernelSpanTexels;
    uint  kernelBoxTaps;
    float kernelBoxStep;
    // UE's MaxScatterDispersion (BloomSurveyMaxScatterDispersion.usf): the kernel is clamped to this
    // level texel by texel BEFORE it is filtered and resampled (BloomClampKernel.usf). Measured on
    // the CPU from the same pixels (BloomRenderer::SurveyKernel), one grey value as theirs is.
    float kernelClampLevel;
    // The centre of the kernel's brightest texel, in ITS texture's UV -- UE's FindKernelCenter.
    float2 kernelCenterUV;
    // The resolve's per-channel scale: UE's FFTMultiply, Tint * saturate(Scatter * s / Total)
    // (BloomFinalizeApplyConstants.usf), applied to the convolution's output as their ApplyTint does.
    // Everything composited into the bloom target after it (ghosts, streak, glare) is therefore in
    // the scene's own units, and the tonemap adds the target as it is.
    float3 resolveScale;
    // Stage 0's local exposure, the tonemap's own parameters (CameraExposureSettings::local*).
    // `leBlurredBlend` is 1 when the grid was not built this frame (never sliced then); `leExposure`
    // is the exposure the stored value already carries (the pre-exposure), 0 = none, the frame's
    // metered one is applied here instead (ExposureMultiplier).
    float leHighlightContrast;
    float leShadowContrast;
    float leDetailStrength;
    float leBlurredBlend;
    float leHighlightThreshold;
    float leShadowThreshold;
    float leMinLogLum;
    float leInvLogLumRange;
    float leExposure;
    // P8C-2h: THE ANAMORPHIC STREAK IS AN ANISOTROPIC PYRAMID, the structure keijiro's KinoStreak
    // uses (github.com/keijiro/KinoStreak, Unlicense) and the one this engine should have started
    // from. Width halves per level, height is untouched; every tap is ~1 texel OF ITS OWN LEVEL,
    // so reach comes from resolution rather than from a growing step. That is what removes both
    // failure modes of the fixed-resolution cascade it replaces: a step coarser than the falloff
    // REPLICATES the source instead of extending it, and a min-filter (the old erosion) cannot
    // taper at a frame edge -- it either clamps one-sided or collapses a whole row.
    //   4 PREFILTER  bloomDown mip0 -> level 0: soft-knee threshold, border fade, 2x2 box
    //   5 DOWNSAMPLE 6 horizontal taps at +-1,+-3,+-5 x 1.25 source texels -> next level
    //   6 UPSAMPLE   acc = upsample(acc from level k+1) + level k * weight  (per-channel weight
    //                IS the chroma: blue is weighted toward the longer levels)
    //   7 COMPOSITE  level 0 -> bloom mip 0, vertical Gaussian, tint, intensity
    // Narrowing is the THRESHOLD's job now: on a soft source, raising it 7 -> 11 took a 149-row
    // corona to 77 rows (measured), and unlike an erosion it is pointwise, so it has no edge
    // behaviour to get wrong.
    float anamorphicIntensity;   // direct brightness multiplier of the composite
    float anamorphicLength;      // the band's visible extent, fraction of the SCREEN width
    float anamorphicSigma;       // composite's vertical Gaussian sigma, in DST texels
    float anamorphicThreshold;   // prefilter threshold, pre-exposed units (as the ghosts' and UE's)
    float anamorphicChroma;      // 0..1: how far the per-channel level weights spread
    float3 anamorphicTint;
    float3 streakWeight;         // this level's own per-channel weight in the up-chain
    // The weight applied to the accumulator ARRIVING from the level above. It is 1 everywhere
    // except on the first up pass, where the "accumulator" is the coarsest level's raw content
    // and this is what applies ITS weight -- otherwise the top of the pyramid would enter the sum
    // unweighted, which is a silent brightness error that grows with Length.
    float3 streakSrcWeight;
    uint2 streakOffsets;         // x = destination strip offset, y = source strip offset
    // Ghost composite (resolve): UE's LensFlareComposite, N full-image copies of the flare-blur
    // image scaled about the screen centre.
    uint  ghostCount;
    float ghostIntensity;
    // Stage 9, the FAKE SUN GLARE (BloomSettings::sunGlare*), and stage 10, the sun probe. View
    // space is the camera's (+z forward, +y up); the projection's x/y scales turn a view direction
    // into NDC and back.
    float4 sunGlareDir;      // xyz: direction TO the sun, view space, unit; w: unused
    float4 sunGlareProj;     // xy: projection _11/_22; zw: the sun disc's radius in source UV
    float4 sunGlareParams;   // stage 9: x intensity, y core radius (rad), z veil weight, w veil radius (rad)
                             // stage 10: x the off-frame fade angle (rad), yzw world up in view space
    float4 historyParams;    // stage 12: x this frame's weight, y the history's pre-exposure rescale, z 1 = valid
    // Stage 0, the ART split (BloomSettings::convSunKernelScale): the sun's own share of the kernel's
    // light. x the scale, y the sun's radius (rad: the disc and the sky round it), z 1 = the sun is in
    // view (its direction and the projection then ride in sunGlareDir.xyz / sunGlareProj.xy).
    float4 sunShare;
};

// The exposure record. It carries the sun probe (sun_probe.hlsli).
RWByteAddressBuffer ExposureValue : register(u2);

static const float3 kLumaWeights = float3(0.2126f, 0.7152f, 0.0722f);

float ExposureMultiplier()
{
    if (exposureEnabled == 0u) { return 1.0f; }
    const float ev100 = asfloat(ExposureValue.Load(0));
    if (isnan(ev100) || isinf(ev100)) { return 1.0f; }
    return (0.18f * 8.0f) / exp2(ev100);
}

// UE's FilterPixel (GPUFastFourierTransform.usf), the only filtering their FFT bloom has -- there is
// no threshold anywhere in that path. Below Min nothing is touched; above it the luminance is
// remapped with slope Mult and CLAMPED at Max. Active when Min < Max, UE's gate -- and Mult > 0:
// under theirs Mult 0 clamps every pixel above Min TO Min, which no one wants, and this engine's
// levels have always meant "off" by it.
// The art split's mask: 1 on the SUN -- the disc AND the bright sky round it, whose light makes the
// sun's halo and ghosts as much as the disc's does -- 0 elsewhere and while the sun is not in view
// (sunShare.z). SKY ONLY (reverse-Z depth 0; stage 0 binds the depth in t1, which only the resolve
// reads as the flare image): a glint on geometry is never the sun's, however close to it on screen --
// the sea's glitter path stands right under a low sun. Angles, not UV, so the aspect cannot squash it:
// full to half the radius sunShare.y, gone by all of it.
float SunShareMask(float2 uv)
{
    if (sunShare.z < 0.5f) { return 0.0f; }
    if (FlareBlur.SampleLevel(gSmp, uv, 0).r > 0.0f) { return 0.0f; }
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float3 dir = normalize(float3(ndc / sunGlareProj.xy, 1.0f));
    const float angle = acos(clamp(dot(dir, sunGlareDir.xyz), -1.0f, 1.0f));
    const float r = max(sunShare.y, 1.0e-4f);
    return 1.0f - smoothstep(0.5f * r, r, angle);
}

float3 BrightPixelGain(float3 c)
{
    if (!(preFilterMin < preFilterMax) || !(preFilterMult > 0.0f)) { return c; }
    const float luma = dot(c, kLumaWeights);
    if (luma > preFilterMin)
    {
        const float target = min(preFilterMult * (luma - preFilterMin) + preFilterMin, preFilterMax);
        c *= target / luma;
    }
    return c;
}

// UE's AddApplyLocalExposurePass on the FFT's input: the tone curve's local exposure, term for term
// (tonemap_cs.hlsl, local_exposure.hlsli), so a highlight the tonemap will compress blooms as it
// will be shown. `c` is the stored value; `uv` the screen position.
float3 ApplyLocalExposure(float3 c, float2 uv)
{
    LocalExposureParams p;
    p.highlightContrastScale = leHighlightContrast;
    p.shadowContrastScale = leShadowContrast;
    p.detailStrength = leDetailStrength;
    p.blurredBlend = leBlurredBlend;
    p.highlightThreshold = leHighlightThreshold;
    p.shadowThreshold = leShadowThreshold;
    if (LocalExposureIsNeutral(p)) { return c; }
    const float lum = dot(c, kLumaWeights);
    if (!(lum > 1e-8f)) { return c; }
    // The exposure the pixel receives in the tonemap: the pre-exposure it already carries, or the
    // metered one the tonemap would apply (exactly its `totalExposure`).
    const float exposure = (leExposure > 0.0f) ? leExposure : ExposureMultiplier();
    const float logLum = log2(lum * ((leExposure > 0.0f) ? 1.0f : exposure));
    const float logExposure = log2(max(exposure, 1e-8f));
    const float baseLog = LocalExposureBaseLogLum(LocalExposureGrid, gSmp, uv, logLum - logExposure,
                                                  leMinLogLum, leInvLogLumRange,
                                                  LocalExposureBase.SampleLevel(gSmp, uv, 0), leBlurredBlend)
                        + logExposure;
    return c * LocalExposureMultiplier(logLum, baseLog, log2(0.18f), p);
}

// UE's ScrubNaNs on the forward transform's input, `-min(-v, 0)`: a NaN or a negative becomes 0. One
// NaN would otherwise spread through the whole spectrum and blank the frame's bloom. An FP16 +Inf is
// held at the format's largest finite value too: FilterPixel would turn it into Inf * 0 = NaN.
float3 ScrubNaNs(float3 c)
{
    c = float3(c.r > 0.0f ? c.r : 0.0f, c.g > 0.0f ? c.g : 0.0f, c.b > 0.0f ? c.b : 0.0f);
    return min(c, 65504.0f.xxx);
}

// The kernel CLAMPED texel by texel, then bilinearly filtered -- UE clamp the whole photograph
// (BloomClampKernel.usf) before they downsample and resize it, so the delta spike never smears into
// its neighbours. A hardware bilinear tap would blend the spike in first and clamp after.
float3 ClampedKernelBilinear(float2 uv)
{
    uint kw, kh;
    BloomKernel.GetDimensions(kw, kh);
    const float2 p = uv * float2(kw, kh) - 0.5f;
    const int2 b = int2(floor(p));
    const float2 f = p - floor(p);
    const int2 hi = int2(kw, kh) - 1;
    const float3 s00 = min(BloomKernel.Load(int3(clamp(b, 0, hi), 0)).rgb, kernelClampLevel.xxx);
    const float3 s10 = min(BloomKernel.Load(int3(clamp(b + int2(1, 0), 0, hi), 0)).rgb, kernelClampLevel.xxx);
    const float3 s01 = min(BloomKernel.Load(int3(clamp(b + int2(0, 1), 0, hi), 0)).rgb, kernelClampLevel.xxx);
    const float3 s11 = min(BloomKernel.Load(int3(clamp(b + int2(1, 1), 0, hi), 0)).rgb, kernelClampLevel.xxx);
    return max(lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y), 0.0f.xxx);
}

// UE's screen-border falloff (PostProcessCommon.ush): 0 at the border, 1 in the centre. The
// composite applies it twice at different radii, which is what makes a flare sit IN the frame.
float DiscMask(float2 screenPos)
{
    const float x = saturate(1.0f - dot(screenPos, screenPos));
    return x * x;
}

// Signed offset of a grid texel from the DC corner, with wrap-around: the kernel's centre must
// sit at texel (0,0) or the convolution comes out translated by half the grid. Same fold UE's
// BloomResizeKernel.usf performs with its frac() trick.
float2 DcFoldedOffset(uint2 pixel)
{
    const float2 p = float2(pixel);
    const float2 n = float2(transformSize);
    return p - n * step(n * 0.5f, p); // p, or p - N once past the halfway line
}

// Bilinear read of the convolved grid. The grid is a UAV -- the chain never leaves
// UNORDERED_ACCESS, so there is no sampler to use and no hardware filtering to inherit.
float4 SampleGridBilinear(float2 gridUv)
{
    const float2 extent = float2(imageSize);
    const float2 p = gridUv * extent - 0.5f;
    const float2 f = frac(p);
    const int2 b = int2(floor(p));
    const int2 hi = int2(imageSize) - 1;

    const float4 s00 = Grid[clamp(b, int2(0, 0), hi)];
    const float4 s10 = Grid[clamp(b + int2(1, 0), int2(0, 0), hi)];
    const float4 s01 = Grid[clamp(b + int2(0, 1), int2(0, 0), hi)];
    const float4 s11 = Grid[clamp(b + int2(1, 1), int2(0, 0), hi)];
    return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// UE's LensFlareTints[8], verbatim (Scene.cpp). RGB is the tint; ALPHA encodes the ghost's scale
// about the screen centre through `(A * (N-1) - (N-1)/2) * GuardBandScale`, which is why the table
// is stored with the alpha rather than a separate position list.
static const float4 kFlareTints[8] = {
    float4(1.0f, 0.8f, 0.4f, 0.60f),
    float4(1.0f, 1.0f, 0.6f, 0.53f),
    float4(0.8f, 0.8f, 1.0f, 0.46f),
    float4(0.5f, 1.0f, 0.4f, 0.39f),
    float4(0.5f, 0.8f, 1.0f, 0.31f),
    float4(0.9f, 1.0f, 0.8f, 0.27f),
    float4(1.0f, 0.8f, 0.4f, 0.22f),
    float4(0.9f, 0.7f, 0.7f, 0.15f)
};

// Stage 11 (the pixel probe): each of the group's 64 threads' share of the window, reduced by thread 0.
groupshared float4 gProbeBest[64];    // rgb of the brightest pixel the thread saw, w its luminance
groupshared int2 gProbeBestAt[64];
groupshared float4 gProbeSum[64];     // rgb sum, w the number of pixels

[numthreads(8, 8, 1)]
[RootSignature(BLOOM_CONV_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;

    // ---- stage 12: the bloom's light over frames (BloomSettings::convTemporalWeight) ----
    //
    // The scene before the upscaler is JITTERED: a glint smaller than a pixel lands on a pixel centre
    // one frame and between them the next, so what the FFT, the ghosts and the streak took from it
    // jumped frame to frame -- the star on a chrome glint flickered 40-50 %. UE's bloom reads TSR's
    // output, which has done this averaging; here it is an exponential average at half the render
    // resolution (a 2x2 box of the scene per texel), reprojected by the G-buffer velocity (every pixel
    // has one, the sky included: currUv - prevUv), the light shafts' temporal step term for term. No
    // neighbourhood clamp: the glint IS the outlier a clamp would cut, and a lagging flare is a soft
    // trail, not a broken edge. Slots, positional: t0 the scene, t1 the velocity, t2 last frame's
    // history; u0 this frame's. Off the frame, or without a valid history, this frame as it is.
    if (convStage == 12u)
    {
        if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }
        const float2 uv = (float2(pixel) + 0.5f) / float2(sourceSize);
        const float3 current = ScrubNaNs(HDRColor.SampleLevel(gSmp, uv, 0).rgb);
        float3 result = current;
        if (historyParams.z > 0.5f)
        {
            const float2 prevUv = uv - FlareBlur.SampleLevel(gSmp, uv, 0).xy;
            if (all(prevUv >= 0.0f) && all(prevUv <= 1.0f))
            {
                const float3 history = BloomKernel.SampleLevel(gSmp, prevUv, 0).rgb * historyParams.y;
                result = lerp(history, current, saturate(historyParams.x));
            }
        }
        Grid[pixel] = float4(result, 1.0f);
        return;
    }

    if (convStage == 0u)
    {
        if (pixel.x >= transformSize.x || pixel.y >= transformSize.y) { return; }

        // Outside the image rectangle the grid is ZERO -- that is the pad, and it is what makes the
        // convolution linear rather than circular.
        if (pixel.x >= imageSize.x || pixel.y >= imageSize.y)
        {
            Grid[pixel] = 0.0f.xxxx;
            return;
        }

        // Four averaged taps, exactly as the pyramid's setup does and for the same reason:
        // this grid is a fraction of the source's resolution, so a single sample would ignore most
        // of the source texels it covers and a moving glint would flicker in and out of the set.
        const float2 uv = (float2(pixel) + 0.5f) / float2(imageSize);
        const float2 quarter = 0.25f / float2(imageSize);
        const float3 t0 = HDRColor.SampleLevel(gSmp, uv + float2(-quarter.x, -quarter.y), 0).rgb;
        const float3 t1 = HDRColor.SampleLevel(gSmp, uv + float2(quarter.x, -quarter.y), 0).rgb;
        const float3 t2 = HDRColor.SampleLevel(gSmp, uv + float2(-quarter.x, quarter.y), 0).rgb;
        const float3 t3 = HDRColor.SampleLevel(gSmp, uv + float2(quarter.x, quarter.y), 0).rgb;
        // Box, not Karis -- see the note in bloom_cs.hlsl: weighting by luma here removes the
        // highlight instead of clamping it, and the convolution has nothing left to spread.
        const float3 color = (t0 + t1 + t2 + t3) * 0.25f;

        // NO THRESHOLD: UE's FFT bloom has none. The convolution's input IS the scene, which is
        // what lets the tonemap PARTITION the light -- scene x the kernel's centre fraction plus
        // this x its scatter fraction (BloomFinalizeApplyConstants) -- instead of adding a glow on
        // top. Their order: local exposure on the input, then on the forward transform ScrubNaNs
        // and FilterPixel.
        // The art split AFTER FilterPixel: before it, Boost's ceiling would hand the dimmed sun its
        // light straight back.
        const float3 lit = BrightPixelGain(ScrubNaNs(ApplyLocalExposure(color, uv))) *
                           lerp(1.0f, max(sunShare.x, 0.0f), SunShareMask(uv));

        Grid[pixel] = float4(lit.r, lit.g, lit.b, 0.0f);
        return;
    }

    if (convStage == 3u)
    {
        // ---- stage 3: the kernel image, resampled into the grid (P8C-2) ----
        //
        // Each grid texel takes the kernel-texture sample that lands on it when the photograph's
        // centre is folded to the DC corner and its full width spans `kernelSpanTexels` grid
        // texels.
        //
        // P8C-2h -- MINIFICATION IS BOX-FILTERED HERE, ON DEMAND. The asset carries no mip chain
        // (UE's does not either); UE generate a downsample chain at runtime with
        // FBloomDownsampleKernelCS, and this is the same box, computed where the ratio is already
        // known and stored nowhere.
        //
        // THE KERNEL IS CLAMPED FIRST, as UE's is (BloomClampKernel.usf, before the downsample and
        // the resize). Measured on the stock kernel: 98% of its energy sits within one texel of the
        // centre, and that spike is a delta -- convolving with it reproduces the source image. UE
        // SPLIT it out: the clamp leaves the scatter in the kernel, and the tonemap scales the scene
        // by the CENTRE's energy fraction and the convolution by the SCATTER's
        // (BloomFinalizeApplyConstants, applied in tonemap_cs.hlsl), so the light the clamp takes
        // out of the kernel is not lost -- it stays in the scene image, unblurred.
        if (pixel.x >= transformSize.x || pixel.y >= transformSize.y) { return; }

        const float2 offset = DcFoldedOffset(pixel);
        const float2 uv = kernelCenterUV + offset / max(kernelSpanTexels, 1.0f);

        float3 k = 0.0f.xxx;
        if (all(uv >= 0.0f) && all(uv <= 1.0f))
        {
            if (kernelBoxTaps <= 1u)
            {
                k = ClampedKernelBilinear(uv);
            }
            else
            {
                const float first = -0.5f * float(kernelBoxTaps - 1u) * kernelBoxStep;
                float count = 0.0f;
                [loop]
                for (uint sy = 0u; sy < kernelBoxTaps; ++sy)
                {
                    [loop]
                    for (uint sx = 0u; sx < kernelBoxTaps; ++sx)
                    {
                        const float2 o = first + float2(sx, sy) * kernelBoxStep;
                        k += ClampedKernelBilinear(uv + o);
                        count += 1.0f;
                    }
                }
                k /= max(count, 1.0f);
            }
        }
        // Packed exactly as the image is: lane 1 = (R + iG) in .xy, lane 2 = (B + i0) in .zw.
        Grid[pixel] = float4(k.r, k.g, k.b, 0.0f);
        return;
    }

    if (convStage == 4u)
    {
        // ---- stage 4: streak PREFILTER (P8C-2h) ----
        //
        // t0 = the HDR source at DISPLAY resolution, u1 = pyramid level 0. `sourceSize` is
        // level 0. Soft-knee threshold, then a box down.
        //
        // P8C-2l: reading the HDR source DIRECTLY, rather than the bloom chain's mip 0, is what
        // frees the streak from the convolution. The pyramid needs its own mip 0 thresholded by
        // `bloom.threshold`, while every flare consumer needs the raw image plus its OWN absolute
        // threshold -- sharing one texture made those two thresholds CASCADE, which is the bug
        // that cut the sun to a remnant while brighter glints sailed through. One source, one
        // threshold per consumer.
        //
        // The knee is keijiro's, verbatim in shape (KinoStreak's Streak.cginc):
        //     c *= max(0, br - threshold) / max(br, 1e-5)
        // It is what narrows the band, and it is why no erosion is needed: a source's dim skirt
        // is scaled to nothing while its core keeps its brightness, and being POINTWISE it has no
        // window that can hang off a frame edge.
        if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }

        // Four taps spread across this level-0 texel's footprint in the source. The footprint is
        // 4x4 display texels at a quarter-resolution level 0, so these sample its corners rather
        // than covering it: enough to break the correlation that makes a moving glint flicker,
        // and the same tap count the half-res version used.
        const float2 uvSrc = (float2(pixel) + 0.5f) / float2(sourceSize);
        const float2 quarter = 0.25f / float2(sourceSize);
        float3 c = 0.25f * (
            HDRColor.SampleLevel(gSmp, uvSrc + float2(-quarter.x, -quarter.y), 0).rgb +
            HDRColor.SampleLevel(gSmp, uvSrc + float2( quarter.x, -quarter.y), 0).rgb +
            HDRColor.SampleLevel(gSmp, uvSrc + float2(-quarter.x,  quarter.y), 0).rgb +
            HDRColor.SampleLevel(gSmp, uvSrc + float2( quarter.x,  quarter.y), 0).rgb);

        const float br = max(c.r, max(c.g, c.b));
        c *= max(br - anamorphicThreshold, 0.0f) / max(br, 1.0e-5f);

        // BORDER FADE: a source cut by the frame is not a source we can see, and its band used to
        // glue itself to the screen edge. Fading the outer fifth detaches it smoothly.
        const float2 ndc = ((float2(pixel) + 0.5f) / float2(sourceSize)) * 2.0f - 1.0f;
        const float border = max(abs(ndc.x), abs(ndc.y));
        c *= 1.0f - smoothstep(0.8f, 1.0f, border);

        BloomOut[uint2(pixel.x + streakOffsets.x, pixel.y)] = float4(c, 0.0f);
        return;
    }

    if (convStage == 5u)
    {
        // ---- stage 5: pyramid DOWNSAMPLE, horizontal only (P8C-2h) ----
        //
        // Six taps at +-1, +-3, +-5 times 1.25 SOURCE texels, box-weighted -- KinoStreak's
        // pattern. The destination is half as wide and exactly as tall: the band's vertical
        // resolution never degrades, which is what keeps a thin band thin.
        //
        // Levels live packed side by side in one texture (widths halve, so 1/2 + 1/4 + ... fits
        // inside the full width): `streakOffsets.y` is where the source level starts,
        // `streakOffsets.x` where this one does. `sourceSize` is the destination level's size,
        // `imageSize` the source level's.
        if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }

        const float srcX = float(pixel.x) * 2.0f + 0.5f;
        const int hiSrcX = int(imageSize.x) - 1;

        float3 sum = 0.0f.xxx;
        [unroll]
        for (int i = 0; i < 6; ++i)
        {
            const float o = (float(i) * 2.0f - 5.0f) * 1.25f;   // -5,-3,-1,+1,+3,+5 times 1.25
            const float x = srcX + o;
            // Manual bilinear along X; clamped, which is SAFE here because every operator in this
            // chain is an average. It was the min-filter that could not survive a frame edge.
            const float xf = clamp(x - 0.5f, 0.0f, float(hiSrcX));
            const int x0 = int(floor(xf));
            const int x1 = min(x0 + 1, hiSrcX);
            const float f = frac(xf);
            const float3 a = Grid[uint2(uint(x0) + streakOffsets.y, pixel.y)].rgb;
            const float3 b = Grid[uint2(uint(x1) + streakOffsets.y, pixel.y)].rgb;
            sum += lerp(a, b, f);
        }

        BloomOut[uint2(pixel.x + streakOffsets.x, pixel.y)] = float4(sum / 6.0f, 0.0f);
        return;
    }

    if (convStage == 6u)
    {
        // ---- stage 6: pyramid UPSAMPLE + ACCUMULATE (P8C-2h) ----
        //
        //     acc(level k) = upsample_x(acc(level k+1)) + level_k * streakWeight
        //
        // `streakWeight` is PER CHANNEL, and that is where the chroma lives now: the CPU shifts
        // the tent toward the coarser levels for blue and the finer ones for red, so blue runs
        // farther along the band exactly as a real cylindrical coating makes it.
        if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }

        const float srcXf = (float(pixel.x) + 0.5f) * 0.5f - 0.5f;
        const int hiSrcX = int(imageSize.x) - 1;
        const int x0 = clamp(int(floor(srcXf)), 0, hiSrcX);
        const int x1 = min(x0 + 1, hiSrcX);
        const float f = saturate(srcXf - floor(srcXf));
        const float3 a = Grid[uint2(uint(x0) + streakOffsets.y, pixel.y)].rgb;
        const float3 b = Grid[uint2(uint(x1) + streakOffsets.y, pixel.y)].rgb;

        const uint2 dst = uint2(pixel.x + streakOffsets.x, pixel.y);
        const float3 own = BloomOut[dst].rgb;
        BloomOut[dst] = float4(lerp(a, b, f) * streakSrcWeight + own * streakWeight, 0.0f);
        return;
    }

    if (convStage == 7u)
    {
        // ---- stage 7: streak COMPOSITE into bloom mip 0 (P8C-2h) ----
        //
        // u0 = the pyramid (level 0 at `streakOffsets.y`), u1 = bloom mip 0, additive -- the
        // resolve has already written it. A small vertical Gaussian gives the band its final
        // softness; it works on an already-blurred band, so it can be narrow without aliasing.
        if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }

        const float2 uv = (float2(pixel) + 0.5f) / float2(sourceSize);
        const float sigma = max(anamorphicSigma, 0.25f);

        float3 sum = 0.0f.xxx;
        float wsum = 0.0f;
        [unroll]
        for (int t = -2; t <= 2; ++t)
        {
            const float dy = float(t) * sigma * 0.7071f;
            const float w = exp(-(dy * dy) / (2.0f * sigma * sigma));
            const float2 p2 = uv * float2(imageSize) + float2(0.0f, dy) - 0.5f;
            const float2 fr = frac(p2);
            const int2 b0 = int2(floor(p2));
            const int2 hi2 = int2(imageSize) - 1;
            const int2 c00 = clamp(b0, int2(0, 0), hi2);
            const int2 c11 = clamp(b0 + int2(1, 1), int2(0, 0), hi2);
            const float3 s00 = Grid[uint2(uint(c00.x) + streakOffsets.y, uint(c00.y))].rgb;
            const float3 s10 = Grid[uint2(uint(c11.x) + streakOffsets.y, uint(c00.y))].rgb;
            const float3 s01 = Grid[uint2(uint(c00.x) + streakOffsets.y, uint(c11.y))].rgb;
            const float3 s11 = Grid[uint2(uint(c11.x) + streakOffsets.y, uint(c11.y))].rgb;
            sum += lerp(lerp(s00, s10, fr.x), lerp(s01, s11, fr.x), fr.y) * w;
            wsum += w;
        }
        const float3 streak = (sum / max(wsum, 1.0e-6f)) * anamorphicTint * anamorphicIntensity;
        float4 dst = BloomOut[pixel];
        BloomOut[pixel] = float4(dst.rgb + streak, dst.a);
        return;
    }

    if (convStage == 11u)
    {
        // ---- stage 11: THE PIXEL PROBE, one group, a debug read-out (editor MCP get_readouts) ----
        //
        // The brightest PRE-EXPOSED pixel in a square window of the frame the tonemap reads, the
        // window's mean, the 4x4 box at that pixel (one texel of the FFT grid at 25 % averages that
        // many) and the pixel after the local exposure the FFT input gets (ApplyLocalExposure, as
        // stage 0). A screenshot cannot say any of it: the tone curve has clipped a glint to white.
        // sunGlareDir carries this stage's fields: xy the window's centre (UV), z its radius (px).
        // One group of 64, so `pixel` is the thread's place in it.
        uint w, h;
        HDRColor.GetDimensions(w, h);
        const int2 centre = int2(sunGlareDir.xy * float2(w, h));
        const int radius = clamp(int(sunGlareDir.z), 0, 128);
        const int side = 2 * radius + 1;
        const uint lane = pixel.y * 8u + pixel.x;
        float4 best = float4(0.0f, 0.0f, 0.0f, -1.0f);
        int2 bestAt = centre;
        float4 sum = 0.0f.xxxx;
        for (int i = int(lane); i < side * side; i += 64)
        {
            const int2 p = centre + int2(i % side - radius, i / side - radius);
            if (any(p < 0) || p.x >= int(w) || p.y >= int(h)) { continue; }
            const float3 c = HDRColor.Load(int3(p, 0)).rgb;
            const float l = dot(c, kLumaWeights);
            sum += float4(c, 1.0f);
            if (l > best.w) { best = float4(c, l); bestAt = p; }
        }
        gProbeBest[lane] = best;
        gProbeBestAt[lane] = bestAt;
        gProbeSum[lane] = sum;
        GroupMemoryBarrierWithGroupSync();
        if (lane != 0u) { return; }
        for (uint t = 1u; t < 64u; ++t)
        {
            if (gProbeBest[t].w > best.w) { best = gProbeBest[t]; bestAt = gProbeBestAt[t]; }
            sum += gProbeSum[t];
        }
        float3 box = 0.0f.xxx;
        for (int by = -2; by < 2; ++by)
        {
            for (int bx = -2; bx < 2; ++bx)
            {
                box += HDRColor.Load(int3(clamp(bestAt + int2(bx, by), int2(0, 0), int2(w, h) - 1), 0)).rgb;
            }
        }
        const float2 bestUV = (float2(bestAt) + 0.5f) / float2(w, h);
        const float exposedLuma = dot(ApplyLocalExposure(max(best.rgb, 0.0f.xxx), bestUV), kLumaWeights);
        ExposureValue.Store3(kPixelProbeOffset, asuint(max(best.rgb, 0.0f.xxx)));
        ExposureValue.Store(kPixelProbeOffset + 12u, asuint(exposedLuma));
        ExposureValue.Store3(kPixelProbeOffset + 16u, asuint(sum.rgb / max(sum.w, 1.0f)));
        ExposureValue.Store(kPixelProbeOffset + 28u, asuint(dot(box / 16.0f, kLumaWeights)));
        return;
    }

    if (convStage == 10u)
    {
        // ---- stage 10: THE SUN PROBE, one thread, before the glare and the tone curve ----
        //
        // What the sun disc looks like in the frame, metered ONCE into the metering record for the
        // sun glare (stage 9) and the corona (tonemap_cs.hlsl SunRays) to read, where each pixel
        // used to tap the disc itself. Only the taps that land INSIDE the frame count, and when
        // none does the last value seen is held: the taps stopped at the frame's edge, so both
        // faded out as the disc's centre reached it and were gone once it passed ("очень резко все
        // добро вокруг солнца пропадает"). Past the edge they now fade by the ANGLE the sun has left
        // the frame by (sunGlareParams.x, radians); the CPU stops asking beyond it.
        //   WHERE the disc shows, too. With the disc half behind a palm trunk the corona, centred on
        // the disc, drew rays out of the trunk ("лучи из ниоткуда"). So the taps are a sunflower
        // over the disc and a margin round it (THE ZONE below), each weighted by its luminance -- a trunk or a leaf in front is
        // dark next to the sun and weighs nothing -- and by whether its DIRECTION is above the
        // horizon, decided geometrically (sunGlareParams.yzw = world up in view space), because the
        // sea below a low sun can glitter as bright as the disc. Their centroid is where the
        // corona's rays leave from, and the visible share how far inside it they start. The
        // horizon used to be a separate analytic segment on the CPU; this covers it and every
        // occluder with one rule. Both are held, like the colour, unless every tap is in frame.
        //   record +16  last disc colour seen (rgb) and last gate (a) -- held off the frame
        //   record +32  this frame's value (rgb) = colour x gate x off-frame fade; (a) visible share
        //   record +48  centroid of what shows: tangent-plane offset from the disc's centre (xy)
        if (any(pixel != 0u)) { return; }
        float3 held = asfloat(ExposureValue.Load3(kSunProbeHeld));
        float gate = asfloat(ExposureValue.Load(kSunProbeHeld + 12u));
        float share = asfloat(ExposureValue.Load(kSunProbeOut + 12u));
        float2 shownAt = asfloat(ExposureValue.Load2(kSunProbeCentroid));
        // Last frame's OUTPUT, for the temporal blend at the end.
        const float3 prevOut = asfloat(ExposureValue.Load3(kSunProbeOut));
        const float prevShare = share;
        const float2 prevShownAt = shownAt;
        float fade = 0.0f;
        const float3 sunDir = sunGlareDir.xyz;
        if (sunDir.z > 0.02f)
        {
            const float2 sunTan = sunDir.xy / sunDir.z;
            const float2 sunNdc = sunTan * sunGlareProj.xy;
            const float2 sunUV = float2(0.5f + 0.5f * sunNdc.x, 0.5f - 0.5f * sunNdc.y);
            const float2 discUV = max(sunGlareProj.zw, 1.0e-5f.xx);
            const float discTan = 2.0f * discUV.x / max(sunGlareProj.x, 1.0e-6f);
            const float3 upView = sunGlareParams.yzw;
            const float3 lumaW = float3(0.2126f, 0.7152f, 0.0722f);
            // THE ZONE (2026-10-27, owner: "a micro-pixel is covered and the corona is gone -- widen the
            // test zone"): 64 taps over the WHOLE disc and a quarter of its radius beyond, where there were
            // 32 over its flat core (0.7 R). The disc's soft edge (0.71-1 R, compose_cs.hlsl SunDiscRaw) and
            // the gaps fronds leave round its rim were invisible to the probe: a core under leaves read as
            // no sun while that edge still bloomed white. The zone's mean goes back to the CORE radiance a
            // clear disc reads -- x kZone^2 / kProfileMean, the disc profile saturate(2 (1 - rho^2)) averaging
            // 0.75 over the unit disc -- so a clear sun reads as before and a covered one by the share of its
            // ENERGY that shows anywhere in the zone. (The sky inside the zone adds (kZone^2 - 1) / 0.75 of its
            // own radiance: thousands of times under the disc.)
            //   WHERE it shows -- the share and the centroid -- is read from t1, the scene BEFORE the upscaler
            // (the same image without DLSS): DLSS rings on the disc, and its brightest tap, the share's
            // denominator, read a clear disc as half covered. The colour stays the upscaled image's (t0),
            // which the corona's and the glare's strength were calibrated on.
            static const uint kZoneTaps = 64u;
            static const float kZone = 1.25f;
            static const float kProfileMean = 0.75f;
            float3 disc = 0.0f.xxx;
            float discTaps = 0.0f;
            float allInFrame = 1.0f;
            float weight = 0.0f;
            float2 weighted = 0.0f.xx;
            float lumMax = 0.0f;
            [unroll]
            for (uint k = 0u; k < kZoneTaps; ++k)
            {
                // Equal area per tap, golden-angle turns; the tangent plane's y is up.
                const float rho = kZone * sqrt(((float)k + 0.5f) / (float)kZoneTaps);
                const float a = (float)k * 2.39996323f;
                const float2 t = float2(cos(a), sin(a)) * (rho * discTan);
                const float2 tapNdc = (sunTan + t) * sunGlareProj.xy;
                const float2 tapUV = float2(0.5f + 0.5f * tapNdc.x, 0.5f - 0.5f * tapNdc.y);
                if (all(tapUV >= 0.0f) && all(tapUV <= 1.0f))
                {
                    // Softened over a fifth of the disc's radius, so a setting sun slides instead
                    // of stepping tap by tap.
                    const float above =
                        saturate(dot(normalize(float3(sunTan + t, 1.0f)), upView) / (0.2f * discTan) + 0.5f);
                    const float3 col = max(HDRColor.SampleLevel(gSmp, tapUV, 0.0f).rgb, 0.0f.xxx) * above;
                    const float lum = dot(max(FlareBlur.SampleLevel(gSmp, tapUV, 0.0f).rgb, 0.0f.xxx), lumaW) * above;
                    disc += col;
                    discTaps += 1.0f;
                    weight += lum;
                    weighted += lum * t;
                    lumMax = max(lumMax, lum);
                }
                else
                {
                    allInFrame = 0.0f;
                }
            }
            if (allInFrame > 0.5f)
            {
                shownAt = weight > 0.0f ? weighted / weight : 0.0f.xx;
                // The share of the disc that shows: its energy as read (the zone's mean, normalised as the
                // colour below) over a clear disc of the brightest tap's radiance.
                share = lumMax > 0.0f
                    ? saturate(weight / (float)kZoneTaps * (kZone * kZone / kProfileMean) / lumMax) : 0.0f;
            }
            // THE SUN, NOT THE SKY IN FRONT OF IT: a gate on how much brighter the disc is than a
            // ring 2.5 disc radii out. Glare structure comes from a POINT source far brighter than
            // its neighbourhood; an overcast deck over the sun is about as bright on the disc as
            // beside it and must throw nothing -- the disc alone drew rays in a grey sky ("при
            // оверкасте видны лучи"). Full strength from 3.5x the ring (a clear sun is 5x or more,
            // its bright aureole included), none at 1.5x (a cloud lit brighter toward the sun by
            // forward scattering stays below it). A gate rather than a subtraction: subtracting
            // the ring took the aureole off a clear sun too.
            float3 around = 0.0f.xxx;
            float ringTaps = 0.0f;
            [unroll]
            for (uint r = 0u; r < 4u; ++r)
            {
                const float2 corner = float2((r & 1u) ? 1.0f : -1.0f, (r & 2u) ? 1.0f : -1.0f);
                const float2 tapUV = sunUV + corner * discUV * (2.5f * 0.7071f);
                if (all(tapUV >= 0.0f) && all(tapUV <= 1.0f))
                {
                    around += HDRColor.SampleLevel(gSmp, tapUV, 0.0f).rgb;
                    ringTaps += 1.0f;
                }
            }
            if (discTaps > 0.0f)
            {
                // The disc as it is -- physical since 2026-10-14, thousands of times the sky round
                // it -- with no FilterPixel: the glare and the corona scale with the disc's ENERGY
                // (BloomRenderer::SunGlareScale / SunRays), so a disc dimmed by a thin cloud or a
                // sunset dims them in proportion, where a capped value would stay pinned at the cap.
                held = max(min(disc / discTaps * (kZone * kZone / kProfileMean), 65504.0f.xxx), 0.0f.xxx);
                if (ringTaps > 0.0f)
                {
                    const float3 lumaW = float3(0.2126f, 0.7152f, 0.0722f);
                    const float contrast = dot(held, lumaW) / max(dot(around / ringTaps, lumaW), 1.0e-4f);
                    gate = saturate((contrast - 1.5f) * 0.5f);
                }
            }
            // How far past the frame's edge the sun's centre is, as angles per axis (the tangent
            // plane's edge sits at 1 / projection).
            const float2 past = max(atan(abs(sunDir.xy) / sunDir.z) - atan(1.0f / sunGlareProj.xy), 0.0f);
            fade = 1.0f - smoothstep(0.0f, max(sunGlareParams.x, 1.0e-4f), length(past));
        }
        // TEMPORAL SMOOTHING of what the glare and the corona read (owner, 2026-10-14: "the corona starts
        // dancing when moving leaves partly cover the sun"). Leaves crossing the disc change its 32 taps
        // frame to frame, and the glare and the corona, which scale with them, flickered: a mean 3.6 %
        // frame-to-frame change 40-120 px round the sun (steps to 10.8 %) against the swaying leaves' own
        // 1.8 % -- the light shafts, smoothed by their own time constant, added nothing. An exponential
        // blend of this frame's value into the last, frame-rate independent: sunGlareDir.w =
        // 1 - exp(-dt / BloomSettings::sunGlareSmoothingSec), 1 = none. The HELD colour and gate stay raw.
        float3 outColour = held * (gate * fade);
        const float blend = saturate(sunGlareDir.w);
        if (blend < 1.0f && all(isfinite(prevOut)) && isfinite(prevShare) && all(isfinite(prevShownAt)))
        {
            outColour = lerp(max(prevOut, 0.0f.xxx), outColour, blend);
            share = lerp(prevShare, share, blend);
            shownAt = lerp(prevShownAt, shownAt, blend);
        }
        ExposureValue.Store3(kSunProbeHeld, asuint(held));
        ExposureValue.Store(kSunProbeHeld + 12u, asuint(gate));
        ExposureValue.Store3(kSunProbeOut, asuint(outColour));
        ExposureValue.Store(kSunProbeOut + 12u, asuint(share));
        ExposureValue.Store2(kSunProbeCentroid, asuint(shownAt));
        return;
    }

    if (convStage == 9u)
    {
        // ---- stage 9: FAKE SUN GLARE, additive onto bloom mip 0 ----
        //
        // The convolution cannot give the sun a corona: its kernel is the size of the bloom
        // image's percent and the photographed star has no wide veil, so the sun came out a small
        // disc with a faint rim ("мне не хватает норм короны вокруг солнца"). This is an analytic
        // halo about the sun's DIRECTION -- an exponential core plus a wide Lorentzian veil, in
        // degrees, so it is the same size at any resolution or field of view -- whose colour and
        // strength are the frame's OWN pixels over the sun disc: a palm in front of the sun, a
        // cloud over it or a sunset's transmittance dims and tints it with nothing else wired.
        // Those are metered once a frame by the sun probe (stage 10) -- including its hold while
        // the disc is off the frame and its fade by how far off -- and read here.
        if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }
        const float3 sunDir = sunGlareDir.xyz;
        if (sunDir.z <= 0.02f) { return; }
        const float3 seen = asfloat(ExposureValue.Load3(kSunProbeOut));
        if (all(seen <= 0.0f)) { return; }

        const float2 uv = (float2(pixel) + 0.5f) / float2(sourceSize);
        const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        const float3 dir = normalize(float3(ndc / sunGlareProj.xy, 1.0f));
        const float angle = acos(clamp(dot(dir, sunDir), -1.0f, 1.0f));
        const float core = exp(-angle / max(sunGlareParams.y, 1.0e-4f));
        const float v = angle / max(sunGlareParams.w, 1.0e-4f);
        const float veil = sunGlareParams.z / (1.0f + v * v);
        const float3 glare = seen * (sunGlareParams.x * (core + veil));

        float4 dst = BloomOut[pixel];
        BloomOut[pixel] = float4(dst.rgb + glare, dst.a);
        return;
    }

    if (convStage == 8u)
    {
        // ---- stage 8: the GHOST COMPOSITE, additive onto bloom mip 0 (P8C-2l) ----
        //
        // Lifted out of the convolution's RESOLVE so that both bloom methods can run it: the
        // pyramid has no resolve to fold it into. u1 is bloom mip 0, `sourceSize` its extent.
        //
        // The flare image already IS the defocused picture of every bright source -- the scatter
        // pass (lens_flare.hlsl) splatted one bokeh sprite per bright tile, exactly as UE's
        // LensFlareBlurVS does. This is then N full-image copies of it scaled about the SCREEN
        // CENTRE by the tint table's alpha ladder (negative scales mirror through the centre,
        // which is what a real ghost chain does), each tinted, masked by a double DiscMask, and
        // added. Drawing a scaled quad and gathering through the inverse map reach the same
        // image; a gather is legal HERE because the transform is one fixed affine map per ghost
        // -- it was the PER-SOURCE gather that could never work.
        //
        // Two suns give two chains for free, and no sun position is plumbed anywhere: the
        // sources' locations are in the image.
        if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }
        if (ghostCount == 0u || ghostIntensity <= 0.0f) { return; }

        const float2 uv = (float2(pixel) + 0.5f) / float2(sourceSize);
        const float2 screenPos = (uv - 0.5f) * 2.0f;
        const float mask = DiscMask(screenPos) * DiscMask(screenPos * 0.8f);
        if (mask <= 0.0f) { return; }

        const uint count = min(ghostCount, 8u);
        const float aScale = float(count) - 1.0f;
        const float aBias = -aScale * 0.5f;
        const float guardBand = 2.0f;                 // UE's GuardBandScale
        // UE divide the composite tint by the guard band's AREA -- the scatter spread the image
        // over a quarter of the flare target, and this puts the energy back.
        const float guardArea = 1.0f / (guardBand * guardBand);

        float3 sum = 0.0f.xxx;
        [loop]
        for (uint g = 0u; g < count; ++g)
        {
            const float4 tint = kFlareTints[g];
            const float scale = (tint.a * aScale + aBias) * guardBand;
            if (abs(scale) < 1.0e-3f) { continue; }

            // The inverse of drawing the full flare texture on a quad of size `scale` centred on
            // the screen centre. A negative scale flips both axes = the mirror.
            const float2 t = (uv - 0.5f) / scale + 0.5f;
            if (any(t < 0.0f) || any(t > 1.0f)) { continue; }

            sum += FlareBlur.SampleLevel(gSmp, t, 0.0f).rgb * tint.rgb *
                   (ghostIntensity * guardArea * mask);
        }

        float4 dst = BloomOut[pixel];
        BloomOut[pixel] = float4(dst.rgb + sum, dst.a);
        return;
    }

    // ---- stage 2: resolve ----
    if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }

    // The grid holds the convolved image in its `imageSize` rectangle; sample it across the bloom
    // target. This is a straight scale-up of a signal that is low frequency by construction.
    const float2 uv = (float2(pixel) + 0.5f) / float2(sourceSize);
    float4 c = SampleGridBilinear(uv);

    // The convolution leaves the grid at unit energy per channel (the per-channel DC divide); the
    // scatter's share of the kernel, its tint and `intensity` arrive here in one multiply, the way
    // UE apply FFTMultiply after the inverse transform.
    const float3 scatter = float3(max(c.x, 0.0f), max(c.y, 0.0f), max(c.z, 0.0f)) * max(resolveScale, 0.0f.xxx);
    BloomOut[pixel] = float4(scatter, 1.0f);
}
