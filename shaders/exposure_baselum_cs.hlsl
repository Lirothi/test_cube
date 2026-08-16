// Base log-luminance layer for local exposure (photographic plan, step P3B).
//
// Writes a small blurred log2-luminance image that the tonemap samples to know what the LARGE-SCALE
// illumination is around each pixel. Local exposure compresses that base and leaves the per-pixel
// detail alone; see local_exposure.hlsli for why that split is the whole trick.
//
// Deliberately a low resolution with a wide box filter rather than a full blur pyramid: the base
// layer must not contain detail, so resolution is not the point -- smoothness is. At 256x144 each
// texel already covers a 10x10 block of a 1440p frame, and the box below widens that further, so
// hardware bilinear sampling in the tonemap yields a smooth field for a single cheap dispatch.
//
// This runs in the existing metering pass, so it costs no extra pass, barrier or command list.
//
// t0: scene-referred HDR colour, the same source the histogram meters.
// u0: R16_FLOAT base log-luminance.

#define EXPOSURE_BASELUM_CS_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

Texture2D<float4> SceneColor : register(t0);
RWTexture2D<float> BaseLogLum : register(u0);
SamplerState gSmp : register(s0);

cbuffer ExposureBaseLumCB : register(b0)
{
    uint baseWidth;
    uint baseHeight;
    float basePad0;
    float basePad1;
};

// Box radius in TARGET texels. 2 gives a 5x5 footprint on top of each texel's own coverage, which
// at this resolution is a very wide blur in screen terms -- what a base layer wants.
static const int kBoxRadius = 2;

[numthreads(8, 8, 1)]
[RootSignature(EXPOSURE_BASELUM_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= baseWidth || dtid.y >= baseHeight)
    {
        return;
    }

    const float2 invSize = 1.0f / float2(baseWidth, baseHeight);
    float sum = 0.0f;
    float weight = 0.0f;

    [unroll]
    for (int y = -kBoxRadius; y <= kBoxRadius; ++y)
    {
        [unroll]
        for (int x = -kBoxRadius; x <= kBoxRadius; ++x)
        {
            const float2 uv = (float2(dtid.xy) + 0.5f + float2(x, y)) * invSize;
            const float3 c = SceneColor.SampleLevel(gSmp, saturate(uv), 0).rgb;
            const float lum = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
            // Average in LOG space, not linear: the base layer is consumed as a log quantity, and
            // averaging linearly would let one bright sample dominate a whole neighbourhood --
            // which is precisely the halo this pass exists to avoid.
            if (!isnan(lum) && !isinf(lum))
            {
                sum += log2(max(lum, 1e-8f));
                weight += 1.0f;
            }
        }
    }

    BaseLogLum[dtid.xy] = (weight > 0.0f) ? (sum / weight) : log2(1e-8f);
}
