#define GTAO_UPSAMPLE_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

// P6B item 5 -- edge-aware upsample from the half-res AO grid to the render resolution.
//
// This is the step that pays back the halving. A plain bilinear stretch spreads every occluded
// half-res texel half a full-res pixel past the silhouette that produced it, which is exactly the
// dark outline against the sky the plan's interface contract forbids. Weighting each of the four
// taps by how well its depth agrees with the destination pixel's own depth keeps the AO on the side
// of the edge it came from.
//
// NOT transcribed from Unreal, and that is deliberate: their shipping `GTAOUpsamplePSAndCS` is a
// five-tap box average with no depth term at all, and the `SmartUpsample` kernel that would have
// been the reference is inside `#if 0` in the drop. This is the standard joint-bilateral upsample
// (Kopf et al. 2007), with depth as the guide because depth is what the AO estimate was keyed on.
//
// P16.4: two channels, one set of weights. The guide is depth, which is the same for both scales,
// so nothing here needs to know which channel is which.
//
// t0: accumulated AO (half res, RG8 -- .x contact scale, .y sky scale)
// t1: scene depth (render res)
// u0: AO at render resolution -- what lighting and compose sample

Texture2D AoTex : register(t0);
Texture2D DepthTex : register(t1);
RWTexture2D<float2> AoOut : register(u0);

SamplerState gSmpPoint : register(s0);
SamplerState gSmpLinear : register(s1);

cbuffer GtaoFilterCB : register(b0)
{
    float2 aoSize;
    float2 invAoSize;
    float2 outSize;        // render resolution here
    float2 invOutSize;
    float  depthA;
    float  depthB;
    float  planeTolerance;
    float  blendWeight;
    float  upsampleTolerance; // RELATIVE depth tolerance, fraction of the destination's depth
    uint   historyValid;
    uint   filterRadius;
    float  temporalClampRange; // temporal kernel only
};

float LinearZ(float deviceZ)
{
    return depthB / max(deviceZ - depthA, 1e-8f);
}

// The depth an AO texel was estimated from -- the same point-sampled fetch the raw and denoise
// kernels use, so the guide compares against the depth the AO actually belongs to.
float AoTapLinearZ(int2 aoPx)
{
    const float2 uv = (float2(aoPx) + 0.5f) * invAoSize;
    return LinearZ(DepthTex.SampleLevel(gSmpPoint, uv, 0).r);
}

[numthreads(8, 8, 1)]
[RootSignature(GTAO_UPSAMPLE_CS_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)outSize.x || tid.y >= (uint)outSize.y)
    {
        return;
    }

    const int2 px = int2(tid.xy);
    const float2 uv = (float2(px) + 0.5f) * invOutSize;
    const float centreLinearZ = LinearZ(DepthTex.Load(int3(px, 0)).r);

    // Continuous position in the half-res grid, and the four texels around it.
    const float2 h = uv * aoSize - 0.5f;
    const float2 baseF = floor(h);
    const float2 f = h - baseF;
    const int2 base = int2(baseF);
    const int2 hi = int2(aoSize) - int2(1, 1);

    const float bilinear[4] = {
        (1.0f - f.x) * (1.0f - f.y),
        f.x * (1.0f - f.y),
        (1.0f - f.x) * f.y,
        f.x * f.y
    };
    const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

    // RELATIVE, not absolute: half a texel of screen space covers a world distance proportional to
    // depth, so the depth step across one tap on a given surface grows with distance. An absolute
    // tolerance would reject every tap on a distant slope and keep only the nearest one, which
    // reintroduces the blocky half-res look far away.
    const float tolerance = max(upsampleTolerance * abs(centreLinearZ), 1e-4f);

    float2 sumAo = float2(0.0f, 0.0f);
    float sumWeight = 0.0f;
    float2 nearestAo = float2(1.0f, 1.0f);
    float nearestDiff = 1e30f;

    [unroll] for (int i = 0; i < 4; ++i)
    {
        const int2 tap = clamp(base + offsets[i], int2(0, 0), hi);
        const float2 tapAo = AoTex.Load(int3(tap, 0)).rg;
        const float diff = abs(AoTapLinearZ(tap) - centreLinearZ);

        const float weight = bilinear[i] * (1.0f - saturate(diff / tolerance));
        sumAo += tapAo * weight;
        sumWeight += weight;

        if (diff < nearestDiff)
        {
            nearestDiff = diff;
            nearestAo = tapAo;
        }
    }

    // All four taps rejected: this full-res pixel sits on a surface no half-res texel sampled (a
    // one-pixel-wide feature, or the far side of a silhouette). Nearest-depth is the honest answer
    // -- a bilinear average of four rejected taps would be the halo this pass exists to avoid.
    AoOut[px] = (sumWeight > 1e-4f) ? (sumAo / sumWeight) : nearestAo;
}
