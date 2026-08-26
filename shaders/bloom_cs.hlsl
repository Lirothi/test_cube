#define BLOOM_CS_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// P8 -- one level of the bloom pyramid. THREE STAGES IN ONE SHADER, selected by `stage`, because
// they share the whole binding layout and differ only in how the destination texel is produced;
// three materials would be three PSOs and three copies of the same table.
//
//   stage 0 SETUP     : the exposed HDR image -> mip 0 of the DOWN chain, through the threshold.
//   stage 1 DOWNSAMPLE: down[mip-1] -> down[mip].
//   stage 2 UPSAMPLE  : up[mip+1] blurred + down[mip] -> up[mip].
//
// WHY THE CHAINS ARE READ THROUGH THEIR OWN UAVs, not as SRVs: this engine's barrier layer
// transitions whole resources, so it cannot hold mip N-1 in SHADER_RESOURCE while mip N is a UAV.
// The HZB pyramid solved this first (hzb_build_cs.hlsl) and this follows it exactly -- successive
// levels are separated by UAV barriers rather than transitions. The cost is that every tap is a
// point load: no hardware bilinear. Both filters below are therefore written as explicit texel
// taps, which is also what makes their weights auditable.
//
// u0: source mip           (down[mip-1] for stage 1, up[mip+1] for stage 2; unused by stage 0)
// u1: destination mip
// u2: the DOWN chain's mip at this level, added in by stage 2. Inert otherwise.
// u3: the exposure record, read-only -- the same buffer the tonemap reads.
// t0: the HDR source image, only for stage 0.

RWTexture2D<float4> SrcMip : register(u0);
RWTexture2D<float4> DstMip : register(u1);
RWTexture2D<float4> AddMip : register(u2);
// Read-only here. A UAV rather than an SRV for the same reason the tonemap binds it that way: it
// rests in UNORDERED_ACCESS and an SRV binding would cost a transition down and back every frame
// for 16 bytes nobody writes in this pass.
RWByteAddressBuffer ExposureValue : register(u3);
Texture2D HDRColor : register(t0);
SamplerState gSmp : register(s0);

cbuffer BloomCB : register(b0)
{
    uint  stage;      // 0 setup, 1 downsample, 2 upsample
    uint  exposureEnabled;
    uint2 dstSize;    // destination mip dimensions, texels
    uint2 srcSize;    // source dimensions (the HDR image for stage 0, else the source mip)
    // UE's BloomThreshold, in the SAME units: luminance AFTER exposure. That is what makes the
    // threshold mean one thing across a sunset and a noon -- see the block in Setup().
    float threshold;
    // Slope of the ramp above the threshold. UE hardwire 0.5 (`saturate(BloomLuminance * 0.5f)`),
    // which is this project's default; exposed because it is the difference between a hard cut and
    // a soft shoulder, and hardwiring it would be a control that lies about being tunable.
    float softKnee;
    float radius;     // upsample tap spacing, in destination texels
    // Karis average on the FIRST downsample only: weight each tap by 1/(1+luma) so one pixel of a
    // sun glint cannot dominate the whole tile. This is the thing that stops ocean sparkle from
    // pumping the entire bloom, and it is deliberately not applied deeper in the chain, where it
    // would just eat energy.
    uint  fireflyClamp;
    uint  bloomPad0;
    uint  bloomPad1;
};

static const float3 kLumaWeights = float3(0.2126f, 0.7152f, 0.0722f);

float Luma(float3 c)
{
    return dot(c, kLumaWeights);
}

float3 LoadSrc(int2 p)
{
    const int2 c = clamp(p, int2(0, 0), int2(srcSize) - 1);
    return SrcMip[c].rgb;
}

// ---------------------------------------------------------------------------------------------
// STAGE 0 -- threshold, transcribed from UE's BloomSetupCommon (PostProcessBloom.usf).
//
// THE THRESHOLD IS COMPARED AGAINST THE EXPOSED LUMINANCE, not the raw one. UE:
//     TotalLuminance = Luminance(LinearColor) * ExposureScale;
//     BloomAmount    = saturate((TotalLuminance - BloomThreshold) * 0.5);
//     return BloomAmount * LinearColor;
// Two things follow, and both are the reason this step is called "exposure-aware" rather than
// "add a blur":
//   * the threshold is authored in the units the VIEWER sees, so a scene that gets darker does not
//     silently lose its bloom, and one that gets brighter does not turn into fog;
//   * the OUTPUT stays in scene units (`BloomAmount * LinearColor`, not the exposed colour), so
//     the tonemap can apply the one global exposure to scene and bloom alike.
// ---------------------------------------------------------------------------------------------
float3 Setup(uint2 pixel)
{
    const float2 uv = (float2(pixel) + 0.5f) / float2(dstSize);

    // FOUR TAPS, KARIS-AVERAGED, NOT ONE. This grid is half the source's resolution, so a single
    // sample looks at one of the four source texels the destination covers and ignores the rest. An
    // ocean glint is one or two pixels wide and moves every frame; sampled that way it drops in and
    // out of the set, and since it sits far above the threshold its entire contribution flickers
    // with it. That was a real, reported artefact, not a theoretical one.
    //
    // The weights are the Karis average (1/(1+luma)) rather than a plain box, for the same reason
    // the first downsample uses it: a box would keep the flicker and merely quarter it, because one
    // blown-out texel still dominates the mean.
    const float2 quarter = 0.25f / float2(dstSize);
    const float3 t0 = HDRColor.SampleLevel(gSmp, uv + float2(-quarter.x, -quarter.y), 0).rgb;
    const float3 t1 = HDRColor.SampleLevel(gSmp, uv + float2(quarter.x, -quarter.y), 0).rgb;
    const float3 t2 = HDRColor.SampleLevel(gSmp, uv + float2(-quarter.x, quarter.y), 0).rgb;
    const float3 t3 = HDRColor.SampleLevel(gSmp, uv + float2(quarter.x, quarter.y), 0).rgb;

    // A PLAIN BOX HERE, AND KARIS ONLY ONE LEVEL LATER. Karis-averaging at this stage was tried and
    // is wrong: weighting by 1/(1+luma) turns a 100x glint against three dark neighbours into ~1,
    // i.e. it does not clamp the highlight, it DELETES it -- and the highlight is the entire reason
    // the pass exists. The box already breaks the sampling correlation that made the flicker
    // visible; the first downsample then applies Karis to data that has been averaged rather than
    // point-sampled, which is where it belongs.
    const float3 color = (t0 + t1 + t2 + t3) * 0.25f;

    // P8C-4: no exposure factor here any more -- the threshold arrives ABSOLUTE, scaled on the CPU
    // by preExposure / ExposureMultiplierFromEv100(14), so it is compared against the STORED value.
    // See the long note in bloom_conv_cs.hlsl stage 0: measuring in viewer units made the same sun
    // cross the line at 1.3 on an open beach and at 6 inside a palm grove, purely because the
    // auto-exposure moved. Both methods changed together, or switching method would also be a
    // threshold change.

    // THRESHOLD < 0 MEANS NO THRESHOLD, and that is UE's DEFAULT, not an escape hatch.
    // `FPostProcessSettings::BloomThreshold` ships at -1.0 (Scene.cpp:423) and their own
    // documentation for it reads: "-1: all pixels affect bloom equally (physically correct, faster
    // as a threshold pass is omitted)". They mean it -- a lens scatters light from EVERYTHING in
    // front of it, not only from whatever passes a brightness test, so a threshold is an artistic
    // control that trades physical behaviour for a cheaper, punchier look.
    //
    // It also changes how bloom answers to exposure. With a threshold, raising exposure pushes more
    // pixels over the line AND scales what was already over it, so bloom grows faster than the
    // image does. Without one, bloom is exactly linear in exposure: twice as bright a scene, twice
    // as much bloom, no step.
    if (threshold < 0.0f)
    {
        return color;
    }
    const float exposedLuma = Luma(color);
    const float amount = saturate((exposedLuma - threshold) * max(softKnee, 1.0e-4f));
    return amount * color;
}

// ---------------------------------------------------------------------------------------------
// STAGE 1 -- the 13-tap downsample from Jimenez's SIGGRAPH 2014 "Next Generation Post Processing
// in Call of Duty", written as explicit texel taps because there is no bilinear on a UAV read.
//
// The pattern is four overlapping 2x2 boxes at the corners plus a centre box, weighted 0.125 each
// with the centre carrying 0.5 in total. A plain 2x2 box halves cleanly but aliases badly on the
// point samples this chain is full of; this one is what keeps a moving glint from strobing as it
// crosses a texel boundary.
// ---------------------------------------------------------------------------------------------
float3 Downsample(uint2 pixel)
{
    const int2 s = int2(pixel) * 2;

    // The centre 2x2 and the four corner 2x2s, as their averages.
    const float3 c00 = LoadSrc(s + int2(0, 0));
    const float3 c10 = LoadSrc(s + int2(1, 0));
    const float3 c01 = LoadSrc(s + int2(0, 1));
    const float3 c11 = LoadSrc(s + int2(1, 1));
    const float3 centre = (c00 + c10 + c01 + c11) * 0.25f;

    const float3 tl = (LoadSrc(s + int2(-2, -2)) + LoadSrc(s + int2(-1, -2)) +
                       LoadSrc(s + int2(-2, -1)) + LoadSrc(s + int2(-1, -1))) * 0.25f;
    const float3 tr = (LoadSrc(s + int2(2, -2)) + LoadSrc(s + int2(3, -2)) +
                       LoadSrc(s + int2(2, -1)) + LoadSrc(s + int2(3, -1))) * 0.25f;
    const float3 bl = (LoadSrc(s + int2(-2, 2)) + LoadSrc(s + int2(-1, 2)) +
                       LoadSrc(s + int2(-2, 3)) + LoadSrc(s + int2(-1, 3))) * 0.25f;
    const float3 br = (LoadSrc(s + int2(2, 2)) + LoadSrc(s + int2(3, 2)) +
                       LoadSrc(s + int2(2, 3)) + LoadSrc(s + int2(3, 3))) * 0.25f;

    if (fireflyClamp != 0u)
    {
        // Karis average: each box contributes in inverse proportion to its own brightness, so a
        // single blown-out texel is averaged DOWN instead of surviving the whole chain.
        const float w0 = 1.0f / (1.0f + Luma(centre));
        const float w1 = 1.0f / (1.0f + Luma(tl));
        const float w2 = 1.0f / (1.0f + Luma(tr));
        const float w3 = 1.0f / (1.0f + Luma(bl));
        const float w4 = 1.0f / (1.0f + Luma(br));
        const float sum = w0 * 0.5f + (w1 + w2 + w3 + w4) * 0.125f;
        return (centre * w0 * 0.5f + (tl * w1 + tr * w2 + bl * w3 + br * w4) * 0.125f) /
               max(sum, 1.0e-6f);
    }
    return centre * 0.5f + (tl + tr + bl + br) * 0.125f;
}

// ---------------------------------------------------------------------------------------------
// STAGE 2 -- 3x3 tent upsample of the coarser level, added to this level's downsampled image.
//
// The tent is the energy-controlled reconstruction the plan asks for: weights 1/16, 2/16, 4/16 in
// the usual separable pattern, summing to exactly 1, so the chain neither gains nor loses energy
// as it walks back up. `radius` spaces the taps in DESTINATION texels, which is what makes the
// control resolution-independent -- a radius in source texels would change meaning at every level.
// ---------------------------------------------------------------------------------------------
float3 Upsample(uint2 pixel)
{
    // The coarser level is half this one's size, hence the 0.5 -- these are its texel coordinates.
    const float2 srcPos = (float2(pixel) + 0.5f) * 0.5f - 0.5f;
    const float r = max(radius, 0.0f) * 0.5f;

    float3 sum = 0.0f.xxx;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float w = ((x == 0) ? 2.0f : 1.0f) * ((y == 0) ? 2.0f : 1.0f) / 16.0f;
            sum += w * LoadSrc(int2(round(srcPos + float2(x, y) * r)));
        }
    }
    return sum + AddMip[int2(pixel)].rgb;
}

[numthreads(8, 8, 1)]
[RootSignature(BLOOM_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dstSize.x || pixel.y >= dstSize.y)
    {
        return;
    }

    float3 result;
    if (stage == 0u)      { result = Setup(pixel); }
    else if (stage == 1u) { result = Downsample(pixel); }
    else                  { result = Upsample(pixel); }

    DstMip[pixel] = float4(result, 1.0f);
}
