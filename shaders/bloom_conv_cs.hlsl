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
    // P8C-2h: MINIFICATION IS FILTERED ON DEMAND, not from baked mips. The kernel asset carries
    // mip 0 only -- UE's does too, and they build a downsample chain at runtime instead
    // (FBloomDownsampleKernelCS). We take the box directly here because this stage already knows
    // the exact ratio: `kernelBoxTaps` samples per axis, `kernelBoxStep` apart in kernel UV.
    // 1 tap = magnification, the common case (at convSize 1.0 the kernel is UPSCALED).
    float kernelSpanTexels;
    uint  kernelBoxTaps;
    float kernelBoxStep;
    // P8C-2j: radius, in kernel UV, of the ring just outside the kernel's CENTRE ZONE. UE define
    // that zone as one output pixel's worth of kernel texels (ViewTexelDiameterInKernelTexels,
    // floored at one) and clamp the kernel to what they measure around it -- see the stage below.
    float kernelCoreRingUV;
    // Where the kernel's centre sits in ITS texture. 0.5 for the stock EXR; kept for a
    // photographed kernel whose hot spot is off-centre (UE find it with a shader).
    float2 kernelCenterUV;
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
    float anamorphicThreshold;   // prefilter threshold, absolute units (see the CPU side)
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
        // texels.
        //
        // P8C-2h -- MINIFICATION IS BOX-FILTERED HERE, ON DEMAND. The asset carries no mip chain
        // (UE's does not either); UE generate a downsample chain at runtime with
        // FBloomDownsampleKernelCS, and this is the same box, computed where the ratio is already
        // known and stored nowhere. Note we deliberately do NOT copy UE's kernel CLAMP: their
        // clamp is only safe because they split the kernel into a centre term applied directly to
        // scene colour and a scatter term through the convolution -- we carry the whole kernel
        // through the convolution, so clamping the core would turn the bloom into pure haze.
        if (pixel.x >= transformSize.x || pixel.y >= transformSize.y) { return; }

        const float2 offset = DcFoldedOffset(pixel);
        const float2 uv = kernelCenterUV + offset / max(kernelSpanTexels, 1.0f);

        // ---- THE CENTRE SPIKE IS NOT BLOOM, AND UE DO NOT CONVOLVE WITH IT ----
        //
        // Measured on this kernel: 98.18% of its energy sits within ONE texel of the centre. That
        // spike is a delta -- convolving with it reproduces the source image, so an ADDITIVE bloom
        // built from the raw kernel is 98% a blurred copy of the scene and 2% the starburst. That
        // is the "blurry blobs instead of stars" this pass existed to fix.
        //
        // UE never see it because they SPLIT the kernel: `SceneColorApplyParameters` scales scene
        // colour by the CENTRE's energy fraction and `FFTMulitplyParameters` scales the
        // convolution by the SCATTER's (PostProcessTonemap.usf: `SceneColorTint = ColorScale0 *
        // SceneColorApplyParamaters[0]`), so what the convolution contributes is scatter alone.
        // We take the same split at its source, the way their FBloomClampKernelCS does: clamp the
        // kernel to what it measures just outside the centre zone. Our DC-divide then renormalises
        // the remainder, so the star comes back at full strength and `intensity` keeps its
        // meaning. What we do NOT copy is their scene DIMMING -- our bloom is additive and the
        // direct light is already in the image; re-adding it is the double-count we are removing.
        float3 clampLevel = 0.0f.xxx;
        {
            [unroll]
            for (int ring = 0; ring < 8; ++ring)
            {
                const float ang = 6.28318530718f * (float(ring) / 8.0f);
                const float2 o = float2(cos(ang), sin(ang)) * kernelCoreRingUV;
                clampLevel = max(clampLevel,
                    BloomKernel.SampleLevel(gSmp, kernelCenterUV + o, 0.0f).rgb);
            }
        }

        float3 k = 0.0f.xxx;
        if (all(uv >= 0.0f) && all(uv <= 1.0f))
        {
            if (kernelBoxTaps <= 1u)
            {
                k = BloomKernel.SampleLevel(gSmp, uv, 0.0f).rgb;
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
                        k += BloomKernel.SampleLevel(gSmp, uv + o, 0.0f).rgb;
                        count += 1.0f;
                    }
                }
                k /= max(count, 1.0f);
            }
        }
        k = min(k, clampLevel);
        // Packed exactly as the image is: lane 1 = (R + iG) in .xy, lane 2 = (B + i0) in .zw.
        Grid[pixel] = float4(k.r, k.g, k.b, 0.0f);
        return;
    }

    if (convStage == 4u)
    {
        // ---- stage 4: streak PREFILTER (P8C-2h) ----
        //
        // u0 = bloomDown mip0 (source), u1 = pyramid level 0. `sourceSize` is level 0,
        // `imageSize` the source. Soft-knee threshold, then a 2x2 box down.
        //
        // The knee is keijiro's, verbatim in shape (KinoStreak's Streak.cginc):
        //     c *= max(0, br - threshold) / max(br, 1e-5)
        // It is what narrows the band, and it is why no erosion is needed: a source's dim skirt
        // is scaled to nothing while its core keeps its brightness, and being POINTWISE it has no
        // window that can hang off a frame edge.
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
                c += Grid[clamp(base + int2(dx, dy), int2(0, 0), hiSrc)].rgb;
            }
        }
        c *= 0.25f;

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
