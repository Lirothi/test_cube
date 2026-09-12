// Volumetric clouds, plan C2: the temporal resolve of the half-res trace. THIS IS NOT A TRANSCRIPTION
// OF UE's VolumetricRenderTarget.usf, and it is worth being exact about which parts are whose:
//   * FROM UE's reconstruction (VolumetricRenderTarget.usf:205-218): the CLOUD'S OWN FRONT DEPTH
//     reprojects the pixel -- the sky's velocity buffer only knows the camera's rotation, and a
//     cloud 3 km out has parallax the sky does not -- and an off-screen reprojection falls back to
//     the new sample.
//   * NOT UE: the exponential blend (weight 1 - historyWeight), the 3x3 neighbourhood clamp and the
//     still-camera relaxation/inertia. Those are ssr_temporal_cs.hlsl's, i.e. UE TemporalAA.usf's
//     shape (AA_LERP, neighbourhood clamp) plus a measured still-camera rule of ours. UE's VRT does
//     none of this: in its default mode 0 it traces ONE of four checkerboard texels per frame, takes
//     that texel's new value outright and the reprojected history for the other three (no blend,
//     no clamp -- the optional ReprojectionBoxConstraint AABB clamp is off by default), and leaves
//     the stochastic noise to the frame's TAA/TSR at full resolution. We have no TAA pass of our
//     own on the composed frame (DLSS plays that role only when it is on), and at a budget sample
//     count our per-frame noise is several times UE's at 768, so the accumulation lives here.
//   * NOT UE: any history rejection on CLOUD depth -- see the note at the test below.
// Depth is NOT filtered (the new trace's depth is kept where it saw cloud), which is what UE do for
// the cloud shadow map for precision reasons and what keeps the next frame's reprojection honest.
// The pre-exposure moved between frames: the history is rescaled by preExposure / previous.
//
// b0 CloudCB   t0 CloudTrace  t1 CloudTraceDepth  t2 PrevResolved  t3 PrevResolvedDepth
// u0 Resolved  u1 ResolvedDepth   s0 linear clamp  s1 point clamp
#pragma pack_matrix(row_major)
#include "cloud_common.hlsli"

#define CLOUD_TEMPORAL_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

Texture2D<float4> CloudTrace : register(t0);
Texture2D<float> CloudTraceDepth : register(t1);
Texture2D<float4> PrevResolved : register(t2);
Texture2D<float> PrevResolvedDepth : register(t3);
RWTexture2D<float4> Resolved : register(u0);
RWTexture2D<float> ResolvedDepth : register(u1);
SamplerState gSmpLinear : register(s0);
SamplerState gSmpPoint : register(s1);

[numthreads(8, 8, 1)]
[RootSignature(CLOUD_TEMPORAL_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = uint2(cloudOutput.xy);
    if (any(id.xy >= size)) { return; }
    const int2 px = int2(id.xy);
    const float4 current = CloudTrace.Load(int3(px, 0));
    const float currentDepth = CloudTraceDepth.Load(int3(px, 0));

    // Nothing to accumulate against yet, or a debug view (which must show the raw trace).
    if (cloudTemporal.x == 0.0f || cloudAerial.w != 0.0f)
    {
        Resolved[px] = current;
        ResolvedDepth[px] = currentDepth;
        return;
    }

    // The 3x3 box of this frame: everything the history is allowed to be.
    float4 boxMin = current, boxMax = current;
    const int2 maxCoord = int2(size) - 1;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const float4 s = CloudTrace.Load(int3(clamp(px + int2(x, y), int2(0, 0), maxCoord), 0));
        boxMin = min(boxMin, s);
        boxMax = max(boxMax, s);
    }

    // Reproject the cloud's front point through last frame's camera. A texel whose ray MISSED the
    // cloud this frame (a thin wisp between the jittered samples) carries the far end of the traced
    // range as its depth, not the wisp's: reprojecting with that would not hurt (kilometres away,
    // a metre of camera motion is a fraction of a pixel either way), but COMPARING it with the
    // history's depth is what tore the history down every frame on exactly the thin parts, and
    // worse the fewer the samples -- the "pixel dance" the owner saw. So a miss reprojects with
    // last frame's depth at this texel, and the depth test below only speaks when BOTH frames hold
    // cloud.
    const bool currentCloud = current.a < 0.99f;
    const float reprojectDepth = currentCloud ? currentDepth : PrevResolvedDepth.Load(int3(px, 0));
    const float2 uv = (float2(px) + 0.5f) / cloudOutput.xy;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 onRay = mul(float4(ndc, 0.5f, 1.0f), cloudInvViewProjNoJitter);
    const float3 rayDir = normalize(onRay.xyz / onRay.w - cloudCamera.xyz);
    const float3 P = cloudCamera.xyz + rayDir * (reprojectDepth * CloudMetresPerKm);
    const float4 prevClip = mul(float4(P, 1.0f), cloudPrevViewProjNoJitter);
    if (prevClip.w <= 1.0e-4f)
    {
        Resolved[px] = current;
        ResolvedDepth[px] = currentDepth;
        return;
    }
    const float2 prevUv = (prevClip.xy / prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
    if (any(prevUv < 0.0f) || any(prevUv > 1.0f))
    {
        Resolved[px] = current;
        ResolvedDepth[px] = currentDepth;
        return;
    }

    float4 history = PrevResolved.SampleLevel(gSmpLinear, prevUv, 0);
    history.rgb *= cloudTemporal.z; // last frame's exposure -> this frame's
    const float historyDepth = PrevResolvedDepth.SampleLevel(gSmpLinear, prevUv, 0);
    const bool historyCloud = history.a < 0.99f;
    // NOT a UE rule. UE's reconstruction never rejects history on a CLOUD-depth disagreement: its
    // depth tests (VolumetricRenderTarget.usf:314-340, :431) compare the SCENE depth stored beside
    // the cloud -- geometry edges, "cloud over trees" -- and are skipped outright when everything is
    // further than 5 km (MinimumDistanceKmToDisableDisoclusion). The first version of this shader
    // misread :431 as a cloud-vs-cloud test at 10 %, which is what tore the history on thin parts.
    // What is kept is OURS and deliberately WIDE (half the distance, ramped to the full distance),
    // asked only when both frames hold cloud: the "front depth" is a transmittance-weighted mean
    // through a layer kilometres thick, a 30 % swing between two frames of the same cloud is
    // normal, and only a different cloud altogether should start the history over.
    float agreement = 1.0f;
    if (currentCloud && historyCloud)
    {
        const float relative = abs(historyDepth - currentDepth) / max(currentDepth, 1.0e-3f);
        agreement = 1.0f - saturate((relative - 0.5f) / 0.5f);
    }

    // STILLNESS, as ssr_temporal_cs.hlsl: the reprojection of a still camera is trustworthy, so the
    // neighbourhood clamp relaxes towards the raw history (a 3x3 box of ONE noisy frame is not a
    // bound the converged value must respect) and the frame weight drops by up to 3x. Any camera
    // motion collapses both back to the plain clamp and the full weight, which bounds ghosting.
    const float2 motion = uv - prevUv;
    const float stillness = 1.0f - saturate(length(motion) * 100.0f);
    const float relax = 0.5f * stillness;
    const float4 clamped = lerp(clamp(history, boxMin, boxMax), history, relax);
    const float frameWeight = (1.0f - saturate(cloudTemporal.y)) / (1.0f + 2.0f * stillness);
    const float blendNew = lerp(1.0f, frameWeight, agreement);
    Resolved[px] = lerp(clamped, current, blendNew);
    // The depth channel keeps the cloud's depth across the miss frames of a thin texel, so the next
    // frame reprojects and compares against the cloud, not against the far end of the range.
    ResolvedDepth[px] = currentCloud ? currentDepth : (historyCloud ? historyDepth : currentDepth);
}
