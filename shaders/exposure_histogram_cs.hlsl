// Photographic camera, step P2: log2-luminance histogram of the scene-referred HDR image.
//
// Two entry points share one root signature so both dispatches bind identically:
//   CSClear  -- zero the 256 bins and the open-sky sums (the build below only ever InterlockedAdds).
//   CSBuild  -- sample the HDR scene on a FIXED grid and accumulate into the bins.
//
// The grid is fixed rather than one-thread-per-pixel for two reasons the plan asks for: the pass
// costs the same at any resolution (plan P2 item 1), and native and DLSS sample the SAME normalised
// positions, so the metered result cannot drift between them (the section 6.3 parity contract).
//
// t0: scene-referred HDR colour, PRE-tonemap and PRE-exposure. Exposure must never be metered from
//     an already-exposed image -- that is a feedback loop.
// t1: the RT sky visibility (rt_sky_visibility_cs.hlsl's target: .xyz the bent normal, its length how much sky a
//     point sees; .w the device depth, 0 = the sky itself), read only while `skyVisValid` says it was traced.
// u0: raw (byte-address) histogram, 256 uint bins, then the frame's OPEN-SKY share as two sums (kOpenSkyWords).

#define EXPOSURE_HISTOGRAM_CS_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

Texture2D<float4> SceneColor : register(t0);
Texture2D<float4> SkyVis : register(t1);
RWByteAddressBuffer Histogram : register(u0);
SamplerState gSmp : register(s0);

cbuffer ExposureHistogramCB : register(b0)
{
    uint sampleGridX;    // sample counts, kept as two scalars because the engine's math library
    uint sampleGridY;    // has no uint2 and a mismatched CB field would pack silently wrong
    float minLogLum;     // log2 luminance mapped to bin 0
    float invLogLumRange;// 1 / (maxLogLum - minLogLum)
    // P16.1: scene colour arrives scaled by the pre-exposure. Divided back out here so the
    // histogram measures the SCENE, not the scene times the exposure it is about to be given --
    // otherwise the two chase each other frame to frame.
    float invPreExposure;

    // Metering weight mask. strength 0 makes MeteringWeight return exactly 1 for every sample, so
    // the mask off is bit-identical to the unweighted histogram this replaced.
    float maskStrength;
    float maskInnerRadius;
    float maskOuterRadius;
    float maskSkyBias;
    uint skyVisValid;    // 1 = t1 holds this frame's traced sky visibility; 0 = every sample counts as open sky
};

static const uint kBins = 256u;
// Behind the bins (byte kBins * 4): the metering-weighted sum of each sample's open-sky share, then the sum of the
// weights -- exposure_solve_cs.hlsl divides them. Fixed point like the bins.
static const uint kOpenSkyOffset = kBins * 4u;

// Weights are accumulated as fixed point because InterlockedAdd has no float form. 256 units per
// 1.0 of weight: with a 256x144 grid the worst case is 36864 * 256 = 9.4M in a single bin, three
// orders of magnitude below uint32 overflow, and the quantisation (1/256 of one sample) is far
// below anything a percentile can notice.
static const float kWeightScale = 256.0f;

// Centre-weighted metering, the thing a camera's meter does and what UE stores in a mask texture.
// Procedural here: a texture would have to be authored and bound before it could be tried at all,
// and the radial shape is what a mask would contain anyway.
float MeteringWeight(float2 uv)
{
    if (maskStrength <= 0.0f)
    {
        return 1.0f;
    }

    // Distance from frame centre normalised so 1.0 is the corner.
    const float2 d = uv - 0.5f;
    const float r = length(d) / 0.7071068f;
    const float radial = 1.0f - smoothstep(maskInnerRadius, maskOuterRadius, r);

    // The sky is almost always the brightest thing in an exterior and almost never the subject, so
    // a purely radial mask still lets it dominate the moment the camera tilts up. Fade linearly
    // from the vertical midline to the top edge; below the midline this does nothing.
    const float aboveMid = saturate((0.5f - uv.y) * 2.0f);
    const float sky = 1.0f - maskSkyBias * aboveMid;

    // Floor rather than zero (UE floors at 0.05 too): a subject that fills the frame border should
    // still be metered, just quietly.
    return max(lerp(1.0f, radial * sky, maskStrength), 0.05f);
}

[numthreads(8, 8, 1)]
[RootSignature(EXPOSURE_HISTOGRAM_CS_RS)]
void CSClear(uint3 dtid : SV_DispatchThreadID)
{
    // Dispatched as a single 8x8 group: 64 threads covering 256 bins, 4 each.
    const uint thread = dtid.y * 8u + dtid.x;
    if (thread >= 64u)
    {
        return;
    }
    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        Histogram.Store((thread * 4u + i) * 4u, 0u);
    }
    if (thread == 0u)
    {
        Histogram.Store2(kOpenSkyOffset, uint2(0u, 0u));
    }
}

[numthreads(8, 8, 1)]
[RootSignature(EXPOSURE_HISTOGRAM_CS_RS)]
void CSBuild(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= sampleGridX || dtid.y >= sampleGridY)
    {
        return;
    }

    const float2 uv = (float2(dtid.xy) + 0.5f) / float2(sampleGridX, sampleGridY);
    const float meterWeight = MeteringWeight(uv);

    // The frame's OPEN-SKY share (time of day: the night's camera trims are the OPEN night's, exposure_solve_cs.hlsl):
    // the sky itself counts 1, a surface how much sky it sees -- a beach ~0.9, a wall outdoors ~0.5, a closed room ~0.
    // Weighted as the meter weighs, so it is the share of what the camera is metering. One atomic per wave: every
    // thread of the grid adds to the same two words.
    {
        float open = 1.0f;
        if (skyVisValid != 0u)
        {
            uint w, h;
            SkyVis.GetDimensions(w, h);
            const uint2 texel = min(uint2(uv * float2(w, h)), uint2(w - 1u, h - 1u));
            const float4 v = SkyVis.Load(int3(texel, 0));
            open = v.w <= 0.0f ? 1.0f : saturate(length(v.xyz));
        }
        const uint openSum = WaveActiveSum((uint)(open * meterWeight * kWeightScale + 0.5f));
        const uint weightSum = WaveActiveSum((uint)(meterWeight * kWeightScale + 0.5f));
        if (WaveIsFirstLane())
        {
            Histogram.InterlockedAdd(kOpenSkyOffset, openSum);
            Histogram.InterlockedAdd(kOpenSkyOffset + 4u, weightSum);
        }
    }

    const float3 color = SceneColor.SampleLevel(gSmp, uv, 0).rgb * invPreExposure;
    const float lum = dot(color, float3(0.2126f, 0.7152f, 0.0722f));

    // Drop non-finite samples entirely rather than binning them. Counting a NaN as black would
    // drag the low percentile down and quietly brighten the whole frame; the plan's section 6.4
    // treats invalid luminance as a reset condition, not as data.
    if (isnan(lum) || isinf(lum))
    {
        return;
    }

    // max() rather than a branch: genuine black belongs in bin 0, it is not invalid.
    const float logLum = log2(max(lum, 1e-8f));
    const float t = saturate((logLum - minLogLum) * invLogLumRange);
    const uint bin = min((uint)(t * (float)(kBins - 1u) + 0.5f), kBins - 1u);

    // Weighted rather than counted: this is what makes the mask above mean anything. The solve
    // works on cumulative FRACTIONS of the total, so it needs no change -- the total is simply a
    // sum of weights now instead of a sample count.
    const uint weight = (uint)(meterWeight * kWeightScale + 0.5f);
    Histogram.InterlockedAdd(bin * 4u, weight);
}
