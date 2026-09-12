// Bilateral grid for local exposure (photographic plan, P3B follow-up, 2026-09-12).
//
// Transcribed from the BILATERAL_GRID permutation of Unreal's PostProcessHistogram.usf (MainCS) with
// the sizes FHistogramCS fixes for it in PostProcessHistogram.cpp: THREADGROUP 8x8, LOOP 8x8 (one
// group covers a 64x64-texel TILE), HISTOGRAM_SIZE 32 (BILATERAL_GRID_DEPTH). Each group writes one
// column of the grid: for every luminance bin, the SUM of log-luminance and the SUM of weight of the
// tile's texels that fall in it. The tonemap slices the grid at (uv, own luminance) and divides the
// two, which yields the mean log-luminance of the texels AROUND this pixel that are ABOUT AS BRIGHT
// as this pixel -- a base layer that stops at a contrast edge instead of bleeding across it. That is
// the whole difference from the blurred base in exposure_baselum_cs, and it is what removes the
// halo a bright sky casts on the first rows of water under the horizon (fog/sky plan, 2026-09-11).
//
// Deviations from the UE file, all deliberate:
//   * The input is a FIXED virtual image (kInputWidth x kInputHeight, bilinear-sampled from the
//     scene at any render resolution) rather than their half-resolution scene colour. Same reason
//     the histogram samples a fixed grid: the cost is resolution-independent and native and DLSS
//     see the SAME normalised positions. A side effect is that the tile count divides the input
//     exactly, so UE's BilateralGridUVScale (which corrects for a partial last tile) is 1.0 here
//     and is not carried.
//   * Per-thread accumulators are float2 in groupshared and plain adds. UE pack two 16-bit fixed
//     point values into a uint and InterlockedAdd them; every thread only ever touches its OWN
//     [x][y] column, so the atomics were never racing anything and the packing existed only to
//     satisfy them (it quantises at 1/512). Floats are the same sum without the quantisation.
//   * No 2x2 Gather quad: a virtual texel has no four real neighbours to gather, so each is one
//     bilinear sample.
//   * The histogram range is the metering pass's own (kMinLogLum..kMaxLogLum, 24 stops), which is
//     UE's EyeAdaptation_HistogramScale/Bias in different clothes. Luminance is floored at the
//     range minimum exactly as their CalculateEyeAdaptationLuminance does with LuminanceMin.
//
// t0: scene-referred HDR colour, the same source the histogram meters.
// u0: RG32_FLOAT Texture3D (tilesX, tilesY, 32) = (sum log2 luminance, sum weight).

#define EXPOSURE_BILATERAL_CS_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

Texture2D<float4> SceneColor : register(t0);
RWTexture3D<float2> BilateralGrid : register(u0);
SamplerState gSmp : register(s0);

cbuffer ExposureBilateralCB : register(b0)
{
    // P16.1: measure the scene, not the scene times its exposure (same as the histogram).
    float invPreExposure;
    // The metering range: log2 luminance mapped to bin 0, and 1 / (maxLogLum - minLogLum). The
    // tonemap slices the grid with the SAME two numbers, which is what makes the Z axis agree.
    float minLogLum;
    float invLogLumRange;
    float bilateralPad0;
    // The virtual input the tiles are laid over.
    uint inputWidth;
    uint inputHeight;
    uint tilesX;
    uint tilesY;
};

// UE's HISTOGRAM_SIZE for this permutation / BILATERAL_GRID_DEPTH. Mirrored by
// ExposureMetering::kBilateralBins and by the slice in local_exposure.hlsli.
static const uint kBins = 32u;
// THREADGROUP_SIZEX/Y and LOOP_SIZEX/Y: 8x8 threads, each walking an 8x8 block, so one group covers
// a 64x64 tile. The group size is also what RecordComputeDispatch assumes.
static const uint kGroup = 8u;
static const uint kLoop = 8u;
static const uint kTile = kGroup * kLoop;

// UE's SharedHistogram[HISTOGRAM_SIZE][THREADGROUP_SIZEX][THREADGROUP_SIZEY]: one private histogram
// per thread, reduced across the group at the end. 32 x 64 x 8 bytes = 16 KB.
groupshared float2 SharedHistogram[kBins][kGroup][kGroup];

[numthreads(8, 8, 1)]
[RootSignature(EXPOSURE_BILATERAL_CS_RS)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID,
            uint groupIndex : SV_GroupIndex)
{
    [unroll]
    for (uint i = 0u; i < kBins; ++i)
    {
        SharedHistogram[i][groupThreadId.x][groupThreadId.y] = float2(0.0f, 0.0f);
    }
    GroupMemoryBarrierWithGroupSync();

    // A group past the tile count (never, given the dispatch shape, but the reduce below writes
    // the grid unconditionally) contributes nothing and writes nothing.
    const bool inGrid = groupId.x < tilesX && groupId.y < tilesY;
    if (inGrid)
    {
        const uint2 leftTop = groupId.xy * kTile;
        const float2 invInputSize = 1.0f / float2(max(inputWidth, 1u), max(inputHeight, 1u));

        // Each thread processes LOOP x LOOP texels of the tile, strided by the group size so that
        // neighbouring threads touch neighbouring texels.
        for (uint y = 0u; y < kTile; y += kGroup)
        {
            for (uint x = 0u; x < kTile; x += kGroup)
            {
                const uint2 texel = leftTop + uint2(x, y) + groupThreadId.xy;
                const float2 uv = (float2(texel) + 0.5f) * invInputSize;
                const float3 c = SceneColor.SampleLevel(gSmp, uv, 0).rgb * invPreExposure;
                const float lum = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
                // Dropped, not binned: the histogram treats a non-finite sample the same way.
                if (isnan(lum) || isinf(lum))
                {
                    continue;
                }
                // CalculateEyeAdaptationLuminance floors at LuminanceMin = exp2(HistogramLogMin):
                // nothing lands below bin 0, and the sum never goes negative.
                const float logLum = max(log2(max(lum, 1e-8f)), minLogLum);
                // ComputeHistogramPositionFromLogLuminance. Deliberately NOT saturated for the
                // sum (UE keep the true position there); only the bucket is clamped.
                const float histPos = (logLum - minLogLum) * invLogLumRange;
                const float fBucket = saturate(histPos) * (float)(kBins - 1u);
                uint bucket0 = (uint)fBucket;
                uint bucket1 = bucket0 + 1u;
                bucket0 = min(bucket0, kBins - 1u);
                bucket1 = min(bucket1, kBins - 1u);
                const float weight1 = frac(fBucket);
                const float weight0 = 1.0f - weight1;
                // (SumLumHist, SumWeight) per bucket -- UE's PackTwoFloatToUINT32 pair.
                SharedHistogram[bucket0][groupThreadId.x][groupThreadId.y] +=
                    float2(histPos * weight0, weight0);
                SharedHistogram[bucket1][groupThreadId.x][groupThreadId.y] +=
                    float2(histPos * weight1, weight1);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Reduce the 64 private histograms into the group's column of the grid: one thread per bin.
    if (inGrid && groupIndex < kBins)
    {
        float sumWeight = 0.0f;
        float sumLuminance = 0.0f;
        [loop]
        for (uint y = 0u; y < kGroup; ++y)
        {
            [loop]
            for (uint x = 0u; x < kGroup; ++x)
            {
                const float2 h = SharedHistogram[groupIndex][x][y];
                // ComputeLogLuminanceFromHistogramPosition applied to a WEIGHTED sum, as UE do:
                // (SumLumHist - SumWeight * Bias) / Scale with Bias = -minLogLum * Scale and
                // Scale = invLogLumRange, which is SumLumHist / Scale + SumWeight * minLogLum.
                sumLuminance += h.x / invLogLumRange + h.y * minLogLum;
                sumWeight += h.y;
            }
        }
        BilateralGrid[uint3(groupId.xy, groupIndex)] = float2(sumLuminance, sumWeight);
    }
}
