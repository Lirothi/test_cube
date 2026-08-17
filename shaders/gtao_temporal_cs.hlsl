#define GTAO_TEMPORAL_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

// P6B item 4 -- temporal accumulation with disocclusion rejection, half resolution.
//
// The raw pass rotates its direction set by `frameIndex` (GetRandomVector), so consecutive frames
// estimate the SAME occlusion from DIFFERENT directions. That is the whole point of this kernel:
// averaging those estimates over time is what buys a 2x6-tap pass the quality of a much wider one.
// Without it the only way to remove the remaining noise is a spatial blur wide enough to erase the
// contacts the pass exists to produce.
//
// Transcribed from Unreal's `GTAOTemporalFilterPSAndCS` (PostProcessAmbientOcclusion.usf),
// including `ReadHistoryClamp`. Two deliberate simplifications and one deliberate correction:
//
//   * UE reprojects with ClipToPrevClip and only falls back to the velocity texture where it has
//     data. Every one of our G-buffer variants writes velocity for every pixel INCLUDING the sky
//     (gbuffer_common.hlsli: prevH = prevWorldPos * prevViewProjNoJitter), so the velocity buffer
//     alone is exact here and the matrix path would be a second, disagreeing source of truth.
//   * their `NeighbourhoodClamp` helper is dead in the shipping path -- the live clamp window is
//     `NewAO +- RangeVal`, and that is what is reproduced.
//   * CORRECTION: their `CompareVeloc` is `1 - saturate(abs(V12.x + V12.y) * 100)`, a SUM of the
//     components, which cancels to zero for a difference of (+a, -a) -- i.e. it reports perfect
//     agreement for two velocities pointing 90 degrees apart. Uses length() instead.
//
// t0: this frame's denoised AO (half res)
// t1: the previous frame's accumulated AO (half res) -- Deferred[(frame-1)].gtaoHistory
// t2: gbVelocity (render res); motion = currUv - prevUv
// u0: accumulated AO (half res) -> consumed by the upsample AND kept as next frame's history

Texture2D CurrTex : register(t0);
Texture2D HistTex : register(t1);
Texture2D VelocityTex : register(t2);
RWTexture2D<float> AoOut : register(u0);

SamplerState gSmpPoint : register(s0);
SamplerState gSmpLinear : register(s1);

cbuffer GtaoFilterCB : register(b0)
{
    float2 aoSize;
    float2 invAoSize;
    float2 outSize;
    float2 invOutSize;
    float  depthA;
    float  depthB;
    float  planeTolerance;
    float  blendWeight;    // weight of THIS frame; small = long history
    float  upsampleTolerance;
    uint   historyValid;   // 0 on the first frame after a resize / after the stage was off
    uint   filterRadius;
    float  temporalClampRange; // temporal kernel only
};

// UE's scale on both velocity tests. A UV-space motion of 0.01 (one hundredth of the screen in one
// frame) is already "fast" for the purpose of trusting a reprojection.
static const float kVelocityScale = 100.0f;
// The clamp window around this frame's value when the camera is still now arrives in the CB as
// `temporalClampRange`. UE hardcode 0.1; ours defaults to 0.35, and that is a measured deviation,
// not a preference. Their AO does not sit behind a DLSS jitter that moves the depth buffer every
// frame, so our per-frame input spread is larger than their window: at 0.1 the history was clamped
// back onto each noisy frame and never accumulated. Distant flicker, static camera, frozen wind:
// 0.1 -> 4.600, 0.35 -> 2.935, 1.0 -> 2.501 (8-bit units). 1.0 is barely better than 0.35 and
// removes the clamp entirely, so 0.35 is where the curve bends.

// UE's ReadHistoryClamp: bilinear, but each of the four taps is clamped into the accepted window
// BEFORE the weighted sum. Clamping the interpolated value instead would let one out-of-range tap
// drag the result to the edge of the window and stay there, which is how a thin ghost survives.
float ReadHistoryClamp(float2 uv, float minAo, float maxAo)
{
    const float2 pixUv = uv * aoSize - 0.5f;
    const float2 baseUv = floor(pixUv);
    const float2 f = pixUv - baseUv;
    const int2 base = int2(baseUv);

    const float w0 = (1.0f - f.x) * (1.0f - f.y);
    const float w1 = f.x * (1.0f - f.y);
    const float w2 = (1.0f - f.x) * f.y;
    const float w3 = f.x * f.y;

    const int2 hi = int2(aoSize) - int2(1, 1);
    const float t0 = HistTex.Load(int3(clamp(base + int2(0, 0), int2(0, 0), hi), 0)).r;
    const float t1 = HistTex.Load(int3(clamp(base + int2(1, 0), int2(0, 0), hi), 0)).r;
    const float t2 = HistTex.Load(int3(clamp(base + int2(0, 1), int2(0, 0), hi), 0)).r;
    const float t3 = HistTex.Load(int3(clamp(base + int2(1, 1), int2(0, 0), hi), 0)).r;

    return w0 * clamp(t0, minAo, maxAo) + w1 * clamp(t1, minAo, maxAo) +
           w2 * clamp(t2, minAo, maxAo) + w3 * clamp(t3, minAo, maxAo);
}

[numthreads(8, 8, 1)]
[RootSignature(GTAO_TEMPORAL_CS_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)aoSize.x || tid.y >= (uint)aoSize.y)
    {
        return;
    }

    const int2 px = int2(tid.xy);
    const float newAo = CurrTex.Load(int3(px, 0)).r;

    // No history to accumulate against: the first frame after a resize, a level switch, or the
    // stage being switched on. Seeding with this frame is what makes those transitions produce a
    // correct-but-noisy image instead of one frame of whatever the texture happened to contain.
    if (historyValid == 0u)
    {
        AoOut[px] = newAo;
        return;
    }

    const float2 uv = (float2(px) + 0.5f) * invAoSize;
    const float2 motion = VelocityTex.SampleLevel(gSmpLinear, uv, 0).xy; // currUv - prevUv
    const float2 prevUv = uv - motion;

    // Off-screen last frame: there is nothing to reproject from. UE relies on the clamp sampler
    // here; rejecting outright is the same result without inventing an edge-stretched history.
    if (any(prevUv < 0.0f) || any(prevUv > 1.0f))
    {
        AoOut[px] = newAo;
        return;
    }

    // DISOCCLUSION: does the pixel we are about to read move the way we do? Sampling the velocity
    // at the SOURCE location answers that with no extra history channel -- a surface that was
    // hidden behind this one last frame carries a different motion, so the test fires exactly on
    // the newly revealed band behind a moving occluder.
    const float2 sourceMotion = VelocityTex.SampleLevel(gSmpLinear, prevUv, 0).xy;
    const float agreement = 1.0f - saturate(length(motion - sourceMotion) * kVelocityScale);

    // How far the history may sit from this frame's estimate. It CLOSES with motion on purpose:
    // a still camera has a trustworthy reprojection and wants a long history, a moving one does
    // not, and clamping hard is cheaper than detecting every way reprojection can be wrong.
    const float velocityMag = saturate(length(motion) * kVelocityScale);
    const float range = lerp(temporalClampRange, 0.0f, velocityMag);
    const float minAo = saturate(newAo - range);
    const float maxAo = saturate(newAo + range);

    const float historyReprojected = ReadHistoryClamp(prevUv, minAo, maxAo);
    // The un-reprojected history is the fallback when the motion disagrees: it is stale but it is
    // this pixel's own past, which beats a confidently wrong neighbour's.
    const float historyHere = clamp(HistTex.Load(int3(px, 0)).r, minAo, maxAo);

    const float history = lerp(historyHere, historyReprojected, agreement);
    AoOut[px] = lerp(history, newAo, saturate(blendWeight));
}
