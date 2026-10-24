// Interior lighting plan D: the probe volume's UPDATE (DDGI; RTXGI's ProbeBlendingCS + ProbeBorderUpdateCS
// + ProbeClassificationCS in one group per probe). The 64 rays the trace wrote for this probe are blended
// into its octahedral tiles: the irradiance tile (6x6 + border) by the cosine to each texel's direction,
// the depth tile (14x14 + border) by the cosine to the 50th power -- mean and mean square of the hit
// distance, for the Chebyshev visibility -- each with the volume's hysteresis. A border texel computes
// the interior texel it mirrors, so one pass fills both; every thread reads its PREVIOUS value before
// any thread writes. A probe traced for the first time in its cell (the volume scrolled, or a new
// generation) takes the new rays whole. Thread 0 then records the cell and the classification: more
// than a quarter of the rays on a back face = the probe is inside geometry and lights nothing.
#define DDGI_UPDATE_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

#pragma pack_matrix(row_major)
#include "ddgi_common.hlsli"
#include "ddgi_constants.hlsli"

Texture2D<float4> Rays : register(t0);                 // (radiance, signed hit distance) per ray, a row per probe
RWTexture2D<float4> IrradianceAtlas : register(u0);
RWTexture2D<float2> DepthAtlas : register(u1);
RWStructuredBuffer<uint4> ProbeState : register(u2);  // (cell xyz, generation << 8 | flags)

static const float kDdgiDepthSharpness = 50.0f; // RTXGI probeDistanceExponent
// RTXGI's probeIrradianceThreshold: a large lighting change lowers the hysteresis by 0.75 for that update, so a probe
// follows a light switched off or a clock jumped from noon to night in a few updates instead of hundreds (at 0.98 a
// noon -> 23:00 jump left 12 % of the day's bounce -- thousands of times the moonlight -- glowing at a palm's foot 420
// frames later). RTXGI tests each texel in its gamma-5 encoding against an absolute 0.2; here the units span the
// day's 1e5 and the night's boosted moon, so the test is a RATIO of the probe's mean radiance (64 rays) to its
// previous mean irradiance (the 6x6 interior): 4x = two stops. And it must HOLD: a probe under a countertop that sees
// the lit room through a few of its 64 rays has a mean that leaps past 4x now and then, and taken at once each leap
// was a pulse on the cabinet below (+50..+89 % for 4-24 frames, 12 % frame to frame where 0.98 alone had 0.06 %). So
// the change counts only on its kDdgiLargeChangeUpdates-th update in a row in the same direction (ProbeState bits 2-4);
// noise does not repeat that way, a light that went out does -- 12 frames late at the 1/4 schedule. A confirmed run
// then keeps following down to kDdgiFollowRatio (two thresholds, as any hysteresis switch): stopped at 4x it left the
// last fifth of the day's bounce to the 0.98 crawl (98 -> 81 over 360 frames at the palm's foot).
static const float kDdgiLargeChangeRatio = 4.0f;
static const float kDdgiFollowRatio = 1.25f;
static const uint kDdgiLargeChangeUpdates = 3u;

groupshared float4 sRay[kDdgiRays];
groupshared float3 sDir[kDdgiRays];
groupshared float sPreviousLuminance[kDdgiIrrInterior * kDdgiIrrInterior];
groupshared uint sBackfaces;

[numthreads(16, 16, 1)]
[RootSignature(DDGI_UPDATE_RS)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const uint probeIndex = gid.x * max(schedule.x, 1u) + schedule.y;
    if (gid.x >= schedule.z || probeIndex >= kDdgiProbeCount)
    {
        return; // uniform across the group
    }
    const uint3 slot = DdgiSlotFromIndex(probeIndex);
    const int3 cell = DdgiCellOfSlot(slot, volumeBase.xyz);
    const uint4 previousState = ProbeState[probeIndex];
    const bool fresh = any(int3(asint(previousState.x), asint(previousState.y), asint(previousState.z)) != cell) ||
                       (previousState.w >> 8) != uint(volumeBase.w) || (previousState.w & kDdgiStateValid) == 0u;

    if (gi == 0u) { sBackfaces = 0u; }
    if (gi < kDdgiRays)
    {
        sRay[gi] = Rays[uint2(gi, gid.x)];
        sDir[gi] = DdgiRayDirection(gi);
    }
    const uint2 tile = DdgiTile(slot);
    const uint2 depthOrigin = tile * (kDdgiDepthInterior + 2u);
    const uint2 depthSource = DdgiBorderSource(gtid.xy, kDdgiDepthInterior);
    const float2 previousDepth = DepthAtlas[depthOrigin + depthSource];
    const bool irradianceThread = all(gtid.xy < kDdgiIrrInterior + 2u);
    const uint2 irradianceOrigin = tile * (kDdgiIrrInterior + 2u);
    const uint2 irradianceSource = irradianceThread ? DdgiBorderSource(gtid.xy, kDdgiIrrInterior) : uint2(1u, 1u);
    const float3 previousIrradiance = irradianceThread ? IrradianceAtlas[irradianceOrigin + irradianceSource].rgb : 0.0f.xxx;
    const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
    if (all(gtid.xy >= 1u) && all(gtid.xy <= kDdgiIrrInterior))
    {
        sPreviousLuminance[(gtid.y - 1u) * kDdgiIrrInterior + (gtid.x - 1u)] = dot(previousIrradiance, kLuma);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi < kDdgiRays && sRay[gi].w < 0.0f)
    {
        InterlockedAdd(sBackfaces, 1u);
    }

    // The large-change test (kDdgiLargeChangeRatio), the same answer in every thread of the probe. It is the
    // irradiance's: a light going out moves no geometry, so the distances keep the volume's hysteresis.
    const float depthHysteresis = fresh ? 0.0f : saturate(volumeParams.y);
    float hysteresis = depthHysteresis;
    uint changeFlags = 0u; // the run of large changes this update leaves for the next (bits 2-3 count, bit 4 up)
    if (!fresh)
    {
        float previousMean = 0.0f;
        for (uint t = 0u; t < kDdgiIrrInterior * kDdgiIrrInterior; ++t) { previousMean += sPreviousLuminance[t]; }
        previousMean /= float(kDdgiIrrInterior * kDdgiIrrInterior);
        float newMean = 0.0f;
        for (uint r = 0u; r < kDdgiRays; ++r) { newMean += dot(max(sRay[r].rgb, 0.0f.xxx), kLuma); }
        newMean /= float(kDdgiRays);
        const float lo = min(previousMean, newMean);
        const float hi = max(previousMean, newMean);
        const uint up = newMean > previousMean ? 1u : 0u;
        const uint previousRun = (previousState.w >> kDdgiStateChangeShift) & 3u;
        const uint previousUp = (previousState.w >> (kDdgiStateChangeShift + 2u)) & 1u;
        const bool following = previousRun >= kDdgiLargeChangeUpdates && previousUp == up;
        if (hi > (following ? kDdgiFollowRatio : kDdgiLargeChangeRatio) * lo + 1.0e-6f)
        {
            const uint run = (previousRun > 0u && previousUp == up) ? min(previousRun + 1u, 3u) : 1u;
            changeFlags = (run << kDdgiStateChangeShift) | (up << (kDdgiStateChangeShift + 2u));
            if (run >= kDdgiLargeChangeUpdates)
            {
                hysteresis = max(0.0f, hysteresis - 0.75f);
            }
        }
    }

    {
        const float3 texelDir = DdgiTexelDirection(depthSource, kDdgiDepthInterior);
        float3 acc = 0.0f.xxx;
        for (uint r = 0u; r < kDdgiRays; ++r)
        {
            const float w = pow(saturate(dot(texelDir, sDir[r])), kDdgiDepthSharpness);
            const float d = min(abs(sRay[r].w), volumeParams.w);
            acc += float3(w * d, w * d * d, w);
        }
        const float2 result = acc.z > 1.0e-6f ? acc.xy / acc.z : previousDepth;
        DepthAtlas[depthOrigin + gtid.xy] = lerp(result, previousDepth, depthHysteresis);
    }
    if (irradianceThread)
    {
        const float3 texelDir = DdgiTexelDirection(irradianceSource, kDdgiIrrInterior);
        float4 acc = 0.0f.xxxx;
        for (uint r = 0u; r < kDdgiRays; ++r)
        {
            const float w = saturate(dot(texelDir, sDir[r]));
            acc += float4(sRay[r].rgb * w, w);
        }
        const float3 result = acc.w > 1.0e-6f ? acc.rgb / acc.w : previousIrradiance;
        IrradianceAtlas[irradianceOrigin + gtid.xy] = float4(lerp(result, previousIrradiance, hysteresis), 1.0f);
    }

    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u)
    {
        const uint flags = kDdgiStateValid | (sBackfaces * 4u > kDdgiRays ? kDdgiStateInactive : 0u) | changeFlags;
        ProbeState[probeIndex] = uint4(asuint(cell.x), asuint(cell.y), asuint(cell.z), (uint(volumeBase.w) << 8) | flags);
    }
}
