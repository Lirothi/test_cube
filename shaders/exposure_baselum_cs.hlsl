// Base log-luminance layer for local exposure (photographic plan, step P3B): UE's BLURRED LOG
// LUMINANCE, the blur half of the base the tonemap blends with the bilateral grid
// (LocalExposureBlurredLuminanceBlend). Local exposure compresses that base and leaves the per-pixel
// detail alone; see local_exposure.hlsli for why that split is the whole trick.
//
// UE build it in two steps (PostProcessLocalExposure.cpp AddLocalExposureBlurredLogLuminancePass):
// SetupLogLuminanceCS takes log2 of the luminance of a DOWNSAMPLED scene -- the downsample averages
// LINEAR colour -- and a Gaussian of LocalExposureBlurredLuminanceKernelSizePercent of the view
// (default 50) blurs that log image. Three stages here, on a fixed 256x144 grid:
//
//   0 SETUP  log2 of the linear mean luminance over this texel's footprint      -> u0
//   1 BLUR X UE's Gaussian along x                                               u0 -> u1
//   2 BLUR Y the same along y                                                    u1 -> u0
//
// THE WIDTH IS THE POINT (2026-10-14). This used to be a 5x5 box of point-sampled log luminances,
// ~2 % of the screen. Once the sun's disc became physical -- 19 stops above the sky round it -- the
// two or three of those 25 samples that landed on it lifted the base by ~2 stops in a disc-sized
// neighbourhood, and the highlight compression drew a dark ring round the sun. UE's half-screen
// Gaussian spreads the same disc over thousands of texels: well under a tenth of a stop.
//
// This runs in the existing metering pass, so it costs no extra pass or command list.
//
// t0: scene-referred HDR colour, the same source the histogram meters.
// u0: R16_FLOAT base log-luminance (the result the tonemap reads).
// u1: R16_FLOAT intermediate for the separable blur.

#define EXPOSURE_BASELUM_CS_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

Texture2D<float4> SceneColor : register(t0);
RWTexture2D<float> BaseLogLum : register(u0);
RWTexture2D<float> BlurTemp : register(u1);
SamplerState gSmp : register(s0);

cbuffer ExposureBaseLumCB : register(b0)
{
    // P16.1: same reason as the histogram -- measure the scene, not the scene times its exposure.
    float invPreExposure;
    uint baseWidth;
    uint baseHeight;
    uint baseStage;      // 0 setup, 1 blur x, 2 blur y
    // UE's GetBlurRadius in this grid's texels: width * KernelSizePercent / 100 * 0.5. The
    // Gaussian is exp(-16.7 (x / R)^2) out to ceil(R) (PostProcessWeightedSampleSum.cpp
    // NormalDistributionUnscaled, LegacyCompatibilityConstant), normalised over its taps.
    float blurRadius;
};

// The loop bound is a literal (a bound read from the constant buffer cannot be unrolled or proven
// finite by the compiler); taps past the radius carry no weight. 128 = the whole 256-wide grid at
// KernelSizePercent 100.
static const int kMaxBlurTaps = 128;

float BlurTap(int2 p, bool fromTemp)
{
    const int2 hi = int2(baseWidth, baseHeight) - 1;
    const int2 c = clamp(p, int2(0, 0), hi);
    return fromTemp ? BlurTemp[c] : BaseLogLum[c];
}

[numthreads(8, 8, 1)]
[RootSignature(EXPOSURE_BASELUM_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= baseWidth || dtid.y >= baseHeight)
    {
        return;
    }

    if (baseStage == 0u)
    {
        // A 4x4 grid of bilinear taps over the texel's footprint: each tap averages 2x2 source
        // texels, so the mean covers an 8x8 block -- the whole footprint up to 2048 px wide.
        // LINEAR mean, then the log, as UE's downsample-then-SetupLogLuminance does.
        const float2 invSize = 1.0f / float2(baseWidth, baseHeight);
        float sum = 0.0f;
        float weight = 0.0f;
        [unroll]
        for (int y = 0; y < 4; ++y)
        {
            [unroll]
            for (int x = 0; x < 4; ++x)
            {
                const float2 uv = (float2(dtid.xy) + (float2(x, y) + 0.5f) * 0.25f) * invSize;
                const float3 c = SceneColor.SampleLevel(gSmp, uv, 0).rgb * invPreExposure;
                const float lum = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
                if (!isnan(lum) && !isinf(lum))
                {
                    sum += max(lum, 0.0f);
                    weight += 1.0f;
                }
            }
        }
        BaseLogLum[dtid.xy] = log2(max((weight > 0.0f) ? sum / weight : 0.0f, 1e-8f));
        return;
    }

    // Stages 1 / 2: UE's Gaussian, one axis at a time.
    const bool alongX = (baseStage == 1u);
    const float R = max(blurRadius, 1.0e-3f);
    const int taps = min((int)ceil(R), kMaxBlurTaps);
    float sum = 0.0f;
    float weight = 0.0f;
    for (int i = -kMaxBlurTaps; i <= kMaxBlurTaps; ++i)
    {
        if (abs(i) > taps) { continue; }
        const float w = exp(-16.7f * ((float)i / R) * ((float)i / R));
        const int2 p = int2(dtid.xy) + (alongX ? int2(i, 0) : int2(0, i));
        sum += w * BlurTap(p, !alongX);
        weight += w;
    }
    const float blurred = sum / max(weight, 1.0e-8f);
    if (alongX) { BlurTemp[dtid.xy] = blurred; }
    else { BaseLogLum[dtid.xy] = blurred; }
}
