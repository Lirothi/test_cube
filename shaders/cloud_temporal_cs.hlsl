// Volumetric clouds, plan C2: the temporal resolve of the half-res trace. The shape of
// ssr_temporal_cs.hlsl and of UE's VolumetricRenderTarget reconstruction (VolumetricRenderTarget.usf
// :174-300): the CLOUD'S OWN FRONT DEPTH reprojects the pixel (the sky's velocity buffer only knows
// the camera's rotation, and a cloud 3 km out has parallax the sky does not), the history is read
// where that point was last frame, a 3x3 neighbourhood clamp bounds the ghosting, a depth
// disagreement of more than a tenth of the distance rejects the history (usf:431), and the new frame
// is blended in with weight 1 - historyWeight. Depth is NOT filtered (the new trace's depth is kept),
// which is what UE do for the cloud shadow map for precision reasons and what keeps the next frame's
// reprojection honest.
//
// Deltas: UE trace ONE of four checkerboard texels per frame and reconstruct the rest from history;
// we trace every half-res texel every frame with a jittered start and accumulate. The pre-exposure
// moved between frames: the history is rescaled by preExposure / previous preExposure.
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

    // Reproject the cloud's front point through last frame's camera.
    const float2 uv = (float2(px) + 0.5f) / cloudOutput.xy;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 onRay = mul(float4(ndc, 0.5f, 1.0f), cloudInvViewProjNoJitter);
    const float3 rayDir = normalize(onRay.xyz / onRay.w - cloudCamera.xyz);
    const float3 P = cloudCamera.xyz + rayDir * (currentDepth * CloudMetresPerKm);
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
    // UE VolumetricRenderTarget.usf:431: a tenth of the distance is the disagreement that means
    // "not the same cloud"; ramp rather than step so an edge does not flicker between the two.
    const float relative = abs(historyDepth - currentDepth) / max(currentDepth, 1.0e-3f);
    const float agreement = 1.0f - saturate((relative - 0.1f) / 0.4f);

    const float4 clamped = clamp(history, boxMin, boxMax);
    const float blendNew = lerp(1.0f, 1.0f - saturate(cloudTemporal.y), agreement);
    Resolved[px] = lerp(clamped, current, blendNew);
    ResolvedDepth[px] = currentDepth;
}
