#define SSR_TEMPORAL_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

// Temporal resolve for the screen-space reflection buffer.
//
// WHY IT EXISTS. A screen-space ray is violently sensitive to its own start: at a grazing angle the
// sub-pixel jitter DLSS applies every frame moves the reflected hit by tens of pixels, so the raw
// buffer boils even with a still camera. DLSS cannot fix it downstream either -- the motion vectors
// it gets describe the REFLECTOR, while the reflected image moves to a completely different law.
// Unreal never display their raw SSR for the same reason: every hit goes through `ReprojectHit` and
// the buffer is flagged `SSR_OUTPUT_FOR_DENOISER`. This is the missing half.
//
// UNREAL HAVE A SEPARATE CONFIGURATION FOR THIS AND IT IS NOT THE ONE THEIR GTAO USES. SSR goes
// through their TAA as `ETAAPassConfig::ScreenSpaceReflections`, which is TemporalAA.usf's
// `TAA_PASS_CONFIG == 3`:
//
//     AA_HISTORY_PAYLOAD (HISTORY_PAYLOAD_RGB_OPACITY)   colour AND opacity, filtered together
//     AA_DYNAMIC 1                                       reproject by velocity
//     AA_FILTERED 1
//     AA_LERP 8                                          this frame is worth 1/8
//     AA_MANUALLY_CLAMP_HISTORY_UV 1
//     AA_YCOCG 1                                         CLAMP IN YCoCg, NOT IN RGB
//
// The two that matter and that a GTAO-shaped filter would have got wrong:
//
//   * **the neighbourhood clamp happens in YCoCg.** Clamping RGB channel-by-channel lets a history
//     sample be pulled back on one channel and not the others, which shifts its HUE -- visible on a
//     reflection as coloured fringing that a luminance/chroma split does not produce. Their exact
//     RGBToYCoCg/YCoCgToRGB are reproduced below.
//   * **the blend is 1/8**, not a number chosen by taste.
//
// Alpha (the ray's visibility) is filtered alongside, which their RGB_OPACITY payload also does --
// and it has to be, because the buffer is PREMULTIPLIED: filtering rgb on a different rule than a
// would change the implied colour of every partially-visible pixel.
//
// The reprojection, the disocclusion test and the per-tap clamped history read keep the shape of
// gtao_temporal_cs.hlsl, which is itself transcribed from UE's GTAO temporal filter.
//
// t0: this frame's raw reflection (premultiplied RGBA, reflection resolution)
// t1: the previous frame's accumulated reflection -- Deferred[(frame-1)].reflectionHistory
// t2: gbVelocity (render res); motion = currUv - prevUv
// u0: accumulated reflection -> what the blur and compose consume, AND next frame's history

Texture2D CurrTex : register(t0);
Texture2D HistTex : register(t1);
Texture2D VelocityTex : register(t2);
RWTexture2D<float4> OutTex : register(u0);

SamplerState gSmpPoint : register(s0);
SamplerState gSmpLinear : register(s1);

cbuffer SsrTemporalCB : register(b0)
{
    float2 texSize;      // reflection resolution
    float2 invTexSize;
    float  blendWeight;  // weight of THIS frame; small = long history
    uint   historyValid; // 0 on the first frame after a resize / level switch / stage toggle
    float  clampExpand;  // how much the neighbourhood box may be widened when the camera is still
    float  _pad0;
};

// UE's scale on both velocity tests: a UV motion of 0.01 in one frame is already "fast" for the
// purpose of trusting a reprojection.
static const float kVelocityScale = 100.0f;

// TemporalAA.usf, verbatim. Note their pair is not normalised -- RGBToYCoCg scales by 4 and
// YCoCgToRGB divides by 4 -- so they only round-trip as a PAIR. Do not "fix" one of them.
float3 RGBToYCoCg(float3 rgb)
{
    return float3(dot(rgb, float3( 1.0f, 2.0f,  1.0f)),
                  dot(rgb, float3( 2.0f, 0.0f, -2.0f)),
                  dot(rgb, float3(-1.0f, 2.0f, -1.0f)));
}

float3 YCoCgToRGB(float3 ycocg)
{
    const float y = ycocg.x * 0.25f;
    const float co = ycocg.y * 0.25f;
    const float cg = ycocg.z * 0.25f;
    return float3(y + co - cg, y + cg, y - co - cg);
}

// Clamp one premultiplied sample into the window. RGB goes through YCoCg so a pull-back cannot
// change the sample's hue; alpha is a scalar and clamps directly.
float4 ClampSample(float4 c, float4 lo, float4 hi)
{
    const float3 ycocg = clamp(RGBToYCoCg(c.rgb), lo.rgb, hi.rgb);
    return float4(YCoCgToRGB(ycocg), clamp(c.a, lo.a, hi.a));
}

// Bilinear history read where each of the four taps is clamped into the window BEFORE the weighted
// sum. Clamping the interpolated value instead lets one out-of-range tap drag the result onto the
// edge of the window and hold it there, which is exactly how a thin ghost survives.
float4 ReadHistoryClamp(float2 uv, float4 lo, float4 hi)
{
    const float2 pixUv = uv * texSize - 0.5f;
    const float2 baseUv = floor(pixUv);
    const float2 f = pixUv - baseUv;
    const int2 base = int2(baseUv);

    const float w0 = (1.0f - f.x) * (1.0f - f.y);
    const float w1 = f.x * (1.0f - f.y);
    const float w2 = (1.0f - f.x) * f.y;
    const float w3 = f.x * f.y;

    const int2 maxCoord = int2(texSize) - int2(1, 1);
    const float4 t0 = HistTex.Load(int3(clamp(base + int2(0, 0), int2(0, 0), maxCoord), 0));
    const float4 t1 = HistTex.Load(int3(clamp(base + int2(1, 0), int2(0, 0), maxCoord), 0));
    const float4 t2 = HistTex.Load(int3(clamp(base + int2(0, 1), int2(0, 0), maxCoord), 0));
    const float4 t3 = HistTex.Load(int3(clamp(base + int2(1, 1), int2(0, 0), maxCoord), 0));

    return w0 * ClampSample(t0, lo, hi) + w1 * ClampSample(t1, lo, hi) +
           w2 * ClampSample(t2, lo, hi) + w3 * ClampSample(t3, lo, hi);
}

[numthreads(8, 8, 1)]
[RootSignature(SSR_TEMPORAL_CS_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)texSize.x || tid.y >= (uint)texSize.y)
    {
        return;
    }

    const int2 px = int2(tid.xy);
    const float4 newC = CurrTex.Load(int3(px, 0));

    // Nothing to accumulate against yet. Seeding with this frame is what makes a resize or a level
    // switch produce a correct-but-noisy image instead of one frame of whatever was in the texture.
    if (historyValid == 0u)
    {
        OutTex[px] = newC;
        return;
    }

    // The 3x3 box of this frame, IN YCoCg for the colour channels. Everything the history is
    // allowed to be lives inside it.
    const float4 newYc = float4(RGBToYCoCg(newC.rgb), newC.a);
    float4 boxMin = newYc;
    float4 boxMax = newYc;
    const int2 maxCoord = int2(texSize) - int2(1, 1);
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float4 s = CurrTex.Load(int3(clamp(px + int2(x, y), int2(0, 0), maxCoord), 0));
            const float4 sYc = float4(RGBToYCoCg(s.rgb), s.a);
            boxMin = min(boxMin, sYc);
            boxMax = max(boxMax, sYc);
        }
    }

    const float2 uv = (float2(px) + 0.5f) * invTexSize;
    const float2 motion = VelocityTex.SampleLevel(gSmpLinear, uv, 0).xy; // currUv - prevUv
    const float2 prevUv = uv - motion;

    if (any(prevUv < 0.0f) || any(prevUv > 1.0f))
    {
        OutTex[px] = newC; // nothing on screen last frame to reproject from
        return;
    }

    // DISOCCLUSION: does the pixel we are about to read move the way we do? Sampling velocity at
    // the SOURCE answers that with no extra history channel -- a surface that was hidden behind
    // this one last frame carries different motion, so this fires on exactly the newly revealed
    // band behind a moving occluder.
    const float2 sourceMotion = VelocityTex.SampleLevel(gSmpLinear, prevUv, 0).xy;
    const float agreement = 1.0f - saturate(length(motion - sourceMotion) * kVelocityScale);

    // The box WIDENS when the camera is still and closes to the plain neighbourhood as it moves.
    // A still camera has a trustworthy reprojection and wants a long history, which a box tight
    // around one noisy frame would forbid: this is the same trade GTAO's `range` makes, and the
    // same reason it is a knob rather than a constant.
    const float velocityMag = saturate(length(motion) * kVelocityScale);
    const float4 halfSpan = (boxMax - boxMin) * 0.5f;
    const float4 centre = (boxMax + boxMin) * 0.5f;
    const float expand = 1.0f + clampExpand * (1.0f - velocityMag);
    const float4 lo = centre - halfSpan * expand;
    const float4 hi = centre + halfSpan * expand;

    const float4 reprojected = ReadHistoryClamp(prevUv, lo, hi);
    // The un-reprojected history is the fallback when the motion disagrees: stale, but it is this
    // pixel's own past, which beats a confidently wrong neighbour's.
    const float4 here = ClampSample(HistTex.Load(int3(px, 0)), lo, hi);

    const float4 history = lerp(here, reprojected, agreement);
    // NOTE a flat lerp is a MEASURED decision, not an omission. UE's final TAA blend is luma-
    // weighted (HdrWeightY + WeightedLerpFactors, "Tone map to kill fireflies") and runs for the
    // SSR config too -- but transcribing it HERE tripled the frame-to-frame boil on the
    // ssr_bronze_palms mirror (0.43 -> 1.21): their weighting acts on a SPATIALLY FILTERED
    // current frame (AA_FILTERED) with exposure-normalised luma, while our input is raw 1spp
    // leaf<->sky flips at full HDR contrast -- there the asymmetric weights (a dark sample
    // replaces a bright history almost instantly, a bright one barely lands) turn the
    // accumulator into an oscillator. Half of their pair is worse than none of it.
    OutTex[px] = lerp(history, newC, saturate(blendWeight));
}
