// Photographic camera, step P2: log2-luminance histogram of the scene-referred HDR image.
//
// Two entry points share one root signature so both dispatches bind identically:
//   CSClear  -- zero the 256 bins (the build below only ever InterlockedAdds).
//   CSBuild  -- sample the HDR scene on a FIXED grid and accumulate into the bins.
//
// The grid is fixed rather than one-thread-per-pixel for two reasons the plan asks for: the pass
// costs the same at any resolution (plan P2 item 1), and native and DLSS sample the SAME normalised
// positions, so the metered result cannot drift between them (the section 6.3 parity contract).
//
// t0: scene-referred HDR colour, PRE-tonemap and PRE-exposure. Exposure must never be metered from
//     an already-exposed image -- that is a feedback loop.
// u0: raw (byte-address) histogram, 256 uint bins.

#define EXPOSURE_HISTOGRAM_CS_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

Texture2D<float4> SceneColor : register(t0);
RWByteAddressBuffer Histogram : register(u0);
SamplerState gSmp : register(s0);

cbuffer ExposureHistogramCB : register(b0)
{
    uint sampleGridX;    // sample counts, kept as two scalars because the engine's math library
    uint sampleGridY;    // has no uint2 and a mismatched CB field would pack silently wrong
    float minLogLum;     // log2 luminance mapped to bin 0
    float invLogLumRange;// 1 / (maxLogLum - minLogLum)
};

static const uint kBins = 256u;

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
    const float3 color = SceneColor.SampleLevel(gSmp, uv, 0).rgb;
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

    Histogram.InterlockedAdd(bin * 4u, 1u);
}
