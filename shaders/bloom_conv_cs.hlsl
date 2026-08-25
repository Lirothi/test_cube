#define BLOOM_CONV_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// P8C / P8C-2 -- the non-transform stages of convolution bloom. The transform itself is
// bloom_fft_cs.hlsl, which since P8C-2 also owns the Hermitian spectral multiply.
//
//   stage 0 SETUP    HDR frame -> the padded complex grid, thresholded, downscaled.
//   stage 3 KERNEL   the PHOTOGRAPHED kernel image resampled into the grid, centre folded to the
//                    DC corner. P8C-2: this replaced the generated aperture -- a 2-wavelength
//                    |FT{aperture}|^2 is sinc^2 interference WITH ZEROS, physically dashed at any
//                    single wavelength; the photograph integrates the whole visible spectrum and
//                    is smooth, and its rainbow dispersion comes for free.
//   stage 4 STREAK   the anamorphic streak, drawn as its own image so its spectrum can be ADDED
//                    to the kernel's (the transform is linear). The stock EXR is a spherical-lens
//                    kernel and carries no streak, so without this stage "anamorphic" would not
//                    exist at all.
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
// sources, with a x2 guard band folded in by the scatter.
Texture2D FlareBlur : register(t1);
// The photographed convolution kernel (UE's DefaultBloomKernel), FP16 with a mip chain. The mips
// stand in for UE's successive-downsample prefilter when the kernel is drawn small.
Texture2D BloomKernel : register(t2);
RWTexture2D<float4> Grid : register(u0);        // the complex grid, transformSize
RWTexture2D<float4> BloomOut : register(u1);    // mip 0 of the bloom UP chain, resolve only
SamplerState gSmp : register(s0);

cbuffer BloomConvCB : register(b0)
{
    uint  convStage;        // 0 setup, 2 resolve, 3 kernel resample, 4 anamorphic streak
    uint  exposureEnabled;
    uint2 transformSize;    // the ACTIVE padded power-of-two grid (may be a sub-grid of the texture)
    uint2 imageSize;        // how much of it the frame occupies; the rest is the zero pad
    uint2 sourceSize;       // the HDR image being read (setup) or written (resolve)
    float threshold;
    float softKnee;
    // P8C-2 kernel placement. `kernelSpanTexels` is how many GRID texels the kernel photograph's
    // full width covers: convSize (UE's BloomConvolutionSize, a fraction of the viewport) times
    // the image's major axis -- UE's own rule (PostProcessFFTBloom.cpp, KernelSupportScale).
    // `kernelTexLod` pre-picks the mip whose texel density matches that span.
    float kernelSpanTexels;
    float kernelTexLod;
    // Where the kernel's centre sits in ITS texture. 0.5 for the stock EXR; kept for a
    // photographed kernel whose hot spot is off-centre (UE find it with a shader).
    float2 kernelCenterUV;
    // P8C-2b: the anamorphic streak is its OWN separable pass now, not part of the FFT kernel.
    // A convolution physically cannot make the band thinner than its source, and this scene's
    // "source" is a sun disc with a baked corona hundreds of pixels tall -- so the streak gets a
    // nonlinear front end instead: its own (high) threshold takes the CORE of a source and drops
    // the corona, and an optional vertical luminance EROSION narrows what remains. Stages:
    //   4 PREFILTER  bloomDown mip0 -> streak grid (mip1 size): threshold + vertical erosion
    //   5 HBLUR      one cascaded horizontal exponential blur pass (run 4x with growing step);
    //                per-channel 1/e lengths = the chroma (blue coatings streak farther)
    //   6 COMPOSITE  streak grid -> bloom mip 0, tinted, with a small vertical Gaussian (width)
    float anamorphicIntensity;   // direct brightness multiplier of the composite
    float anamorphicLength;      // 1/e extent as a fraction of the SCREEN width
    float anamorphicSigma;       // composite's vertical Gaussian sigma, in DST texels
    float anamorphicThreshold;   // prefilter threshold, viewer units (dot(rgb, luma))
    float anamorphicNarrow;      // erosion half-window in SOURCE texels; 0 = off
    float anamorphicChroma;      // 0..1: how far the per-channel 1/e lengths spread
    float3 anamorphicTint;
    float streakTapStep;         // this hblur pass's tap step, in streak-grid texels
    float streakLambdaTexels;    // 1/e length in streak-grid texels
    // Ghost composite (resolve): UE's LensFlareComposite, N full-image copies of the flare-blur
    // image scaled about the screen centre.
    uint  ghostCount;
    float ghostIntensity;
};

// The exposure record, read to put `threshold` in the units the viewer sees. Same buffer and same
// arithmetic as tonemap_cs.hlsl and bloom_cs.hlsl -- a threshold measured against a different
// exposure than the image is not a threshold.
RWByteAddressBuffer ExposureValue : register(u2);

static const float3 kLumaWeights = float3(0.2126f, 0.7152f, 0.0722f);

float ExposureMultiplier()
{
    if (exposureEnabled == 0u) { return 1.0f; }
    const float ev100 = asfloat(ExposureValue.Load(0));
    if (isnan(ev100) || isinf(ev100)) { return 1.0f; }
    return (0.18f * 8.0f) / exp2(ev100);
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

[numthreads(8, 8, 1)]
[RootSignature(BLOOM_CONV_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;

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

        // threshold < 0 = no threshold, which is UE's default -- see the note in bloom_cs.hlsl.
        float3 lit = color;
        if (threshold >= 0.0f)
        {
            const float exposedLuma = dot(color, kLumaWeights) * ExposureMultiplier();
            const float amount = saturate((exposedLuma - threshold) * max(softKnee, 1.0e-4f));
            lit = amount * color;
        }

        Grid[pixel] = float4(lit.r, lit.g, lit.b, 0.0f);
        return;
    }

    if (convStage == 3u)
    {
        // ---- stage 3: the kernel image, resampled into the grid (P8C-2) ----
        //
        // Each grid texel takes the kernel-texture sample that lands on it when the photograph's
        // centre is folded to the DC corner and its full width spans `kernelSpanTexels` grid
        // texels. The mip is pre-picked on the CPU so a kernel drawn at an eighth of its native
        // size reads the mip whose texels are that size -- UE build the same thing as an explicit
        // downsample chain (FBloomDownsampleKernelCS).
        if (pixel.x >= transformSize.x || pixel.y >= transformSize.y) { return; }

        const float2 offset = DcFoldedOffset(pixel);
        const float2 uv = kernelCenterUV + offset / max(kernelSpanTexels, 1.0f);

        float3 k = 0.0f.xxx;
        if (all(uv >= 0.0f) && all(uv <= 1.0f))
        {
            k = BloomKernel.SampleLevel(gSmp, uv, kernelTexLod).rgb;
        }
        // Packed exactly as the image is: lane 1 = (R + iG) in .xy, lane 2 = (B + i0) in .zw.
        Grid[pixel] = float4(k.r, k.g, k.b, 0.0f);
        return;
    }

    if (convStage == 4u)
    {
        // ---- stage 4: streak PREFILTER (P8C-2b) ----
        //
        // u0 = bloomDown mip0 (source), u1 = the streak grid (write). `sourceSize` is the streak
        // grid, `imageSize` the source. 2x2 box down, a vertical luminance EROSION (take the
        // dimmest of the window -- the nonlinearity that actually narrows a wide source), then
        // the streak's own threshold so only source CORES survive, not the corona.
        if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }

        const int2 base = int2(pixel) * 2;
        const int2 hiSrc = int2(imageSize) - 1;

        float3 c = 0.0f.xxx;
        [unroll]
        for (int dy = 0; dy < 2; ++dy)
        {
            [unroll]
            for (int dx = 0; dx < 2; ++dx)
            {
                int2 sp = clamp(base + int2(dx, dy), int2(0, 0), hiSrc);
                if (anamorphicNarrow > 0.5f)
                {
                    // Erosion: the dimmest sample of a vertical window. A bright region shrinks
                    // by the window's half-height; small glints below the window vanish, which
                    // is the deliberate trade -- the streak belongs to the DOMINANT source.
                    const int n = int(anamorphicNarrow);
                    float3 best = 0.0f.xxx;
                    float bestLum = 1.0e30f;
                    [unroll]
                    for (int t = -2; t <= 2; ++t)
                    {
                        // CLAMPED, deliberately. Treating off-frame as dark (tried, P8C-2e) put
                        // a straight horizontal CUT across the band wherever the erosion window
                        // first reached past the top edge -- the min collapses to zero on one side
                        // of a row and not the other, and a hard edge is worse than a fat band.
                        // Clamping assumes a source that leaves the frame continues, which is the
                        // only honest thing to assume, and degrades smoothly.
                        const int2 tp = clamp(sp + int2(0, t * n / 2), int2(0, 0), hiSrc);
                        const float3 v = Grid[tp].rgb;
                        const float l = dot(v, kLumaWeights);
                        if (l < bestLum) { bestLum = l; best = v; }
                    }
                    c += best;
                }
                else
                {
                    c += Grid[sp].rgb;
                }
            }
        }
        c *= 0.25f;

        const float lum = dot(c, kLumaWeights);
        const float gate = saturate((lum - anamorphicThreshold) /
                                    max(anamorphicThreshold * 0.5f, 1.0e-3f));

        // BORDER FADE, the same treatment the ghost scatter got and for the same reason: a source
        // cut by the frame is not a source we can see. Its erosion is one-SIDED there -- every tap
        // past the edge clamps back onto a bright row -- so a sun touching the top edge kept its
        // full vertical extent and the band came out as a block GLUED to the top of the screen
        // (observed; the same sun 60 px lower gave a localised band). Fading the outer fifth
        // detaches it smoothly. Zeroing off-frame taps in the erosion was tried instead and cut a
        // straight horizontal edge across the band -- a min collapses, it does not taper.
        const float2 ndc = ((float2(pixel) + 0.5f) / float2(sourceSize)) * 2.0f - 1.0f;
        const float border = max(abs(ndc.x), abs(ndc.y));
        const float edgeFade = 1.0f - smoothstep(0.8f, 1.0f, border);

        BloomOut[pixel] = float4(c * (gate * edgeFade), 0.0f);
        return;
    }

    if (convStage == 5u)
    {
        // ---- stage 5: one cascaded horizontal blur pass (P8C-2b) ----
        //
        // 15 taps at `streakTapStep` spacing, weighted exp(-|d| / lambda_c) with PER-CHANNEL 1/e
        // lengths -- the chroma: real anamorphic streaks run farther in blue because the
        // cylindrical elements' coatings do. Run 4x with steps 1, 5, 25, 125 the cascade
        // approximates one exponential with a ~900-texel reach. u0 = src, u1 = dst.
        if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }

        const float spread = saturate(anamorphicChroma);
        const float3 lambda = max(streakLambdaTexels.xxx *
            float3(1.0f - 0.30f * spread, 1.0f, 1.0f + 0.45f * spread), 1.0f.xxx);

        float3 sum = 0.0f.xxx;
        float3 wsum = 0.0f.xxx;
        const int w_ = int(sourceSize.x);
        [unroll]
        for (int i = -7; i <= 7; ++i)
        {
            const float d = abs(float(i)) * streakTapStep;
            const float3 w = exp(-d / lambda);
            const int x = int(pixel.x) + int(float(i) * streakTapStep);
            // ZERO past the screen edge, weight KEPT in the normalisation. Clamping instead read
            // the border column -- where an edge-riding sun sits -- once per out-of-range tap, and
            // multiplied the source by the clamped tap count: a sun at the frame edge threw a
            // screen-long band of nearly constant brightness (observed). Off screen there is no
            // light; the window is empty and the band honestly fades.
            if (x >= 0 && x < w_)
            {
                sum += Grid[int2(x, int(pixel.y))].rgb * w;
            }
            wsum += w;
        }
        BloomOut[pixel] = float4(sum / max(wsum, 1.0e-6f.xxx), 0.0f);
        return;
    }

    if (convStage == 6u)
    {
        // ---- stage 6: streak COMPOSITE into bloom mip 0 (P8C-2b) ----
        //
        // u0 = the streak grid, u1 = bloom mip 0 (additive -- the resolve has already written
        // it). Manual bilinear + a small vertical Gaussian: `anamorphicSigma` is the band's
        // final soft width, and it works on the BLURRED band, which is why it can be narrow
        // without aliasing.
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
            const float2 f = frac(p2);
            const int2 b = int2(floor(p2));
            const int2 hi2 = int2(imageSize) - 1;
            const float3 s00 = Grid[clamp(b, int2(0, 0), hi2)].rgb;
            const float3 s10 = Grid[clamp(b + int2(1, 0), int2(0, 0), hi2)].rgb;
            const float3 s01 = Grid[clamp(b + int2(0, 1), int2(0, 0), hi2)].rgb;
            const float3 s11 = Grid[clamp(b + int2(1, 1), int2(0, 0), hi2)].rgb;
            sum += lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y) * w;
            wsum += w;
        }
        const float3 streak = (sum / max(wsum, 1.0e-6f)) * anamorphicTint * anamorphicIntensity;
        float4 dst = BloomOut[pixel];
        BloomOut[pixel] = float4(dst.rgb + streak, dst.a);
        return;
    }

    // ---- stage 2: resolve ----
    if (pixel.x >= sourceSize.x || pixel.y >= sourceSize.y) { return; }

    // The grid holds the convolved image in its `imageSize` rectangle; sample it across the bloom
    // target. This is a straight scale-up of a signal that is low frequency by construction.
    const float2 uv = (float2(pixel) + 0.5f) / float2(sourceSize);
    float4 c = SampleGridBilinear(uv);

    // ---- ghosts: UE's LensFlareComposite, transcribed (P8C-2 step 5b) ----
    //
    // The flare-blur image already IS the defocused picture of every bright source -- the scatter
    // pass (lens_flare.hlsl) splatted one bokeh sprite per bright pixel, exactly as UE's
    // LensFlareBlurVS does. The composite is then N full-image copies of it scaled about the
    // SCREEN CENTRE by the tint table's alpha ladder (negative scales mirror -- through the
    // centre, which is what a real ghost chain does), each tinted, masked by a double DiscMask,
    // and added. Drawing a scaled quad and gathering with the inverse map reach the same image;
    // a gather is legal HERE because the transform is one fixed affine map per ghost -- it was
    // the PER-SOURCE gather that could never work.
    //
    // Two suns give two chains for free, and no sun position is plumbed anywhere: the source's
    // location is IN the flare image.
    if (ghostCount > 0u && ghostIntensity > 0.0f)
    {
        const float2 screenPos = (uv - 0.5f) * 2.0f;
        const float mask = DiscMask(screenPos) * DiscMask(screenPos * 0.8f);
        if (mask > 0.0f)
        {
            const uint count = min(ghostCount, 8u);
            const float aScale = float(count) - 1.0f;
            const float aBias = -aScale * 0.5f;
            const float guardBand = 2.0f;                 // UE's GuardBandScale
            // UE divide the composite tint by the guard band's AREA -- the scatter spread the
            // image over a quarter of the flare target, and this puts the energy back.
            const float guardArea = 1.0f / (guardBand * guardBand);

            [loop]
            for (uint g = 0u; g < count; ++g)
            {
                const float4 tint = kFlareTints[g];
                const float scale = (tint.a * aScale + aBias) * guardBand;
                if (abs(scale) < 1.0e-3f) { continue; }

                // The inverse of drawing the full flare texture on a quad of size `scale`
                // centred on the screen centre. A negative scale flips both axes = the mirror.
                const float2 t = (uv - 0.5f) / scale + 0.5f;
                if (any(t < 0.0f) || any(t > 1.0f)) { continue; }

                c.xyz += FlareBlur.SampleLevel(gSmp, t, 0.0f).rgb * tint.rgb *
                         (ghostIntensity * guardArea * mask);
            }
        }
    }

    // UNIT MATCHING BETWEEN THE TWO METHODS, so `intensity` means the same thing in both and
    // switching method is not also a brightness change.
    //
    // The convolution CONSERVES energy: its kernel is normalised through the DC divide, so the
    // output carries the thresholded image's total light, redistributed. The pyramid does not --
    // its tent upsample ADDS the levels together, so its output is roughly the level count times
    // brighter. kBloomMaxMips (8) is the structural reason for most of the measured factor (~13),
    // and is used rather than the measurement because it is the thing that would change if the
    // pyramid ever gained or lost a level.
    const float kConvolutionGain = 8.0f;
    BloomOut[pixel] = float4(max(c.x, 0.0f) * kConvolutionGain,
                             max(c.y, 0.0f) * kConvolutionGain,
                             max(c.z, 0.0f) * kConvolutionGain, 1.0f);
}
