// Interior lighting plan D: the probe volume's UPDATE (DDGI; RTXGI's ProbeBlendingCS + ProbeBorderUpdateCS
// + ProbeClassificationCS in one group per probe). The 64 blended rays the trace wrote for this probe go
// into its octahedral tiles: the irradiance tile (6x6 + border) by the cosine to each texel's direction,
// the depth tile (14x14 + border) by the cosine to the 50th power -- mean and mean square of the hit
// distance, for the Chebyshev visibility -- the irradiance as E^(1/5) with the volume's hysteresis, the depth
// by each update's weight with a slower one (depthHysteresis). A border texel computes
// the interior texel it mirrors, so one pass fills both; every thread reads its PREVIOUS value before
// any thread writes. A probe traced for the first time in its cell (the volume scrolled, or a new
// generation) takes the new rays whole. Thread 0 then records the cell and the classification: more
// than a quarter of the 32 FIXED rays on a back face = the probe is inside geometry and lights nothing
// (RTXGI ProbeClassificationCS: fixed directions, so a probe beside a wall gets the same answer each update),
// and the probe's relocation from the same rays (DdgiRelocate); a probe that moved starts over next update.
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
RWStructuredBuffer<uint4> ProbeState : register(u2);  // per slot: (cell xyz, generation << 8 | flags), (offset xyz, 0)

static const float kDdgiDepthSharpness = 50.0f; // RTXGI probeDistanceExponent
// The weight one update puts into a depth texel on average: the cos^n lobe is 2 pi / (n + 1) of the sphere's 4 pi.
static const float kDdgiDepthExpectedWeight = float(kDdgiBlendRays) / (2.0f * (kDdgiDepthSharpness + 1.0f));
// RTXGI's probeNumRays: what its visibility (14x14 depth texels, cos^50 lobes) is tuned for. With kDdgiBlendRays the
// depth keeps a texel's samples over its memory the same: 1 - h_depth = (1 - h) x kDdgiBlendRays / this.
static const float kDdgiRtxgiRays = 256.0f;
// RTXGI's probeIrradianceThreshold: a large lighting change lowers the hysteresis by 0.75 for that update, so a probe
// follows a light switched off or a clock jumped from noon to night in a few updates instead of hundreds (at 0.98 a
// noon -> 23:00 jump left 12 % of the day's bounce -- thousands of times the moonlight -- glowing at a palm's foot 420
// frames later). RTXGI tests each texel in its gamma-5 encoding against an absolute 0.2; here the units span the
// day's 1e5 and the night's boosted moon, so the test is a RATIO of the mean of this update's estimate over the 6x6
// interior texels to the mean of the previous texels: 4x = two stops. Both means are taken as the atlas stores light,
// of E^(1/5) (ddgi_common.hlsli kDdgiIrradianceGamma), so the ratio is 4^(1/5) there: the arithmetic mean of the rays
// it used before let one ray on the hot spot beside a candle (1000x) read as a 16x brighter probe, and in
// the_bathroom_free's corner cabinet such runs walked the bounce by +-10 % an update (13 % frame to frame, candles
// steady); a light that really changes moves every texel. And it must HOLD: a probe under a countertop that sees
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
groupshared float sPreviousLuminance[kDdgiIrrInterior * kDdgiIrrInterior]; // luminance^(1/5) of each interior texel
groupshared float sNewLuminance[kDdgiIrrInterior * kDdgiIrrInterior];      // the same of this update's estimate
groupshared uint sBackfaces;

// RTXGI DDGIProbeRelocationCS over this update's FIXED rays (their directions are the same every update, so the
// answer is too, and a probe that has found its place stays there). Inside geometry (more than a quarter of them on
// back faces): through the nearest back face and half the minimum distance beyond. Nearer a front face than
// kDdgiMinFrontfaceSpacing: toward the farthest one, if that leads away from the near face, by just what clears it
// (RTXGI steps a fixed world unit), and only below 0.9 of the minimum -- a probe moved to exactly the minimum would
// otherwise read it back a hair short from the half-float distances and step again every update. RTXGI also moves a
// probe with room around it back toward its grid point; that is left out here, a probe moved out of a wall would walk
// back into it and out again. The offset is kept inside kDdgiMaxOffsetSpacing of the grid point, and a cell allows
// kDdgiMaxMoves (ddgi_common.hlsli: in a cramped corner a step clear of one face can land near one no ray saw).
float3 DdgiRelocate(float3 offset, float spacing)
{
    const float minFront = kDdgiMinFrontfaceSpacing * spacing;
    int closestBack = -1;
    int closestFront = -1;
    int farthestFront = -1;
    float closestBackDistance = 1.0e27f;
    float closestFrontDistance = 1.0e27f;
    float farthestFrontDistance = 0.0f;
    uint backfaces = 0u;
    for (uint k = 0u; k < kDdgiFixedRays; ++k)
    {
        const float d = sRay[k].w;
        if (d < 0.0f)
        {
            ++backfaces;
            const float full = -5.0f * d; // the trace stores a back face as -0.2 t
            if (full < closestBackDistance) { closestBackDistance = full; closestBack = int(k); }
        }
        else
        {
            if (d < closestFrontDistance) { closestFrontDistance = d; closestFront = int(k); }
            if (d > farthestFrontDistance) { farthestFrontDistance = d; farthestFront = int(k); }
        }
    }
    float3 moved = offset;
    if (closestBack >= 0 && backfaces * 4u > kDdgiFixedRays)
    {
        moved = offset + sDir[closestBack] * (closestBackDistance + 0.5f * minFront);
    }
    else if (closestFront >= 0 && farthestFront >= 0 && closestFrontDistance < 0.9f * minFront)
    {
        const float away = -dot(sDir[closestFront], sDir[farthestFront]); // the near face recedes this much per step
        if (away > 0.0f)
        {
            const float step = min((minFront - closestFrontDistance) / away, farthestFrontDistance - minFront);
            if (step > 0.0f) { moved = offset + sDir[farthestFront] * step; }
        }
    }
    const float limit = kDdgiMaxOffsetSpacing * spacing;
    const float length2 = dot(moved, moved);
    return length2 > limit * limit ? moved * (limit * rsqrt(length2)) : moved;
}

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
    const uint4 previousState = ProbeState[DdgiStateIndex(probeIndex)];
    // Traced for this cell in this generation before: its offset holds (the trace started this update's rays there).
    const bool placed = all(int3(asint(previousState.x), asint(previousState.y), asint(previousState.z)) == cell) &&
                        (previousState.w >> 8) == uint(volumeBase.w) && (previousState.w & kDdgiStateValid) != 0u;
    const float3 previousOffset = placed ? asfloat(ProbeState[DdgiOffsetIndex(probeIndex)].xyz) : 0.0f.xxx;
    // A probe new to its cell, or one the last update moved, starts over: what it held was seen from elsewhere.
    const bool fresh = !placed || (previousState.w & kDdgiStateMoved) != 0u;

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
        sPreviousLuminance[(gtid.y - 1u) * kDdgiIrrInterior + (gtid.x - 1u)] =
            pow(max(dot(DdgiDecodeIrradiance(previousIrradiance), kLuma), 0.0f), 1.0f / kDdgiIrradianceGamma);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi < kDdgiFixedRays && sRay[gi].w < 0.0f)
    {
        InterlockedAdd(sBackfaces, 1u);
    }

    const float irradianceHysteresis = fresh ? 0.0f : saturate(volumeParams.y);
    // The distances move only with geometry, so they can afford RTXGI's sample density at a quarter of its rays:
    // 0.995 at the volume's 0.98 (a door moved settles its visibility in ~200 updates instead of ~50).
    const float depthHysteresis = fresh ? 0.0f
                                        : 1.0f - (1.0f - saturate(volumeParams.y)) * float(kDdgiBlendRays) / kDdgiRtxgiRays;
    {
        const float3 texelDir = DdgiTexelDirection(depthSource, kDdgiDepthInterior);
        float3 acc = 0.0f.xxx;
        for (uint r = kDdgiFixedRays; r < kDdgiRays; ++r)
        {
            const float w = pow(saturate(dot(texelDir, sDir[r])), kDdgiDepthSharpness);
            const float d = min(abs(sRay[r].w), volumeParams.w);
            acc += float3(w * d, w * d * d, w);
        }
        const float2 result = acc.z > 1.0e-6f ? acc.xy / acc.z : previousDepth;
        // The update enters by its weight: a cos^50 lobe holds 1 / (2 * 51) of the sphere, so 64 rays put 0.63 of
        // weight into a texel on average and often none -- normalised, its "estimate" was then one ray 40 degrees
        // off with a weight of 1e-6, blended in as fully as a good one. Taken at (1 - hysteresis) x its weight over
        // that mean, the history converges as fast on average and a texel no ray reached does not move: the
        // Chebyshev visibility of a probe beside the corner cabinet's wall had walked its bounce +-10 % an update
        // (8.9 % frame to frame, candles steady; 1.3 % with the depth frozen). Weighted, and at depthHysteresis:
        // the largest step of a frame 10 -> 2.7-4 %, the 99th percentile 6-7.6 -> 1.7-2 %.
        const float rate = fresh ? 1.0f : saturate((1.0f - depthHysteresis) * acc.z / kDdgiDepthExpectedWeight);
        DepthAtlas[depthOrigin + gtid.xy] = lerp(previousDepth, result, rate);
    }
    // This update's irradiance estimate per texel, blended as E^(1/5) (ddgi_common.hlsli kDdgiIrradianceGamma): the
    // previous value is stored that way.
    float3 result = previousIrradiance;
    if (irradianceThread)
    {
        const float3 texelDir = DdgiTexelDirection(irradianceSource, kDdgiIrrInterior);
        float4 acc = 0.0f.xxxx;
        for (uint r = kDdgiFixedRays; r < kDdgiRays; ++r)
        {
            const float w = saturate(dot(texelDir, sDir[r]));
            acc += float4(sRay[r].rgb * w, w);
        }
        if (acc.w > 1.0e-6f) { result = DdgiEncodeIrradiance(acc.rgb / acc.w); }
    }
    if (all(gtid.xy >= 1u) && all(gtid.xy <= kDdgiIrrInterior))
    {
        sNewLuminance[(gtid.y - 1u) * kDdgiIrrInterior + (gtid.x - 1u)] =
            pow(max(dot(DdgiDecodeIrradiance(result), kLuma), 0.0f), 1.0f / kDdgiIrradianceGamma);
    }
    GroupMemoryBarrierWithGroupSync();

    // The large-change test (kDdgiLargeChangeRatio), the same answer in every thread of the probe: the mean of the
    // new texels against the mean of the previous ones (RTXGI compares texel with texel). It is the irradiance's: a
    // light going out moves no geometry, so the distances keep their own hysteresis.
    float hysteresis = irradianceHysteresis;
    uint changeFlags = 0u; // the run of large changes this update leaves for the next (bits 2-3 count, bit 4 up)
    if (!fresh)
    {
        float previousMean = 0.0f;
        float newMean = 0.0f;
        for (uint t = 0u; t < kDdgiIrrInterior * kDdgiIrrInterior; ++t)
        {
            previousMean += sPreviousLuminance[t];
            newMean += sNewLuminance[t];
        }
        previousMean /= float(kDdgiIrrInterior * kDdgiIrrInterior);
        newMean /= float(kDdgiIrrInterior * kDdgiIrrInterior);
        const float lo = min(previousMean, newMean);
        const float hi = max(previousMean, newMean);
        const uint up = newMean > previousMean ? 1u : 0u;
        const uint previousRun = (previousState.w >> kDdgiStateChangeShift) & 3u;
        const uint previousUp = (previousState.w >> (kDdgiStateChangeShift + 2u)) & 1u;
        const bool following = previousRun >= kDdgiLargeChangeUpdates && previousUp == up;
        const float ratio = pow(following ? kDdgiFollowRatio : kDdgiLargeChangeRatio, 1.0f / kDdgiIrradianceGamma);
        if (hi > ratio * lo + 1.0e-6f)
        {
            const uint run = (previousRun > 0u && previousUp == up) ? min(previousRun + 1u, 3u) : 1u;
            changeFlags = (run << kDdgiStateChangeShift) | (up << (kDdgiStateChangeShift + 2u));
            if (run >= kDdgiLargeChangeUpdates)
            {
                hysteresis = max(0.0f, hysteresis - 0.75f);
            }
        }
    }

    if (irradianceThread)
    {
        IrradianceAtlas[irradianceOrigin + gtid.xy] = float4(lerp(result, previousIrradiance, hysteresis), 1.0f);
    }

    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u)
    {
        const uint movesSoFar = placed ? (previousState.w >> kDdgiStateMovesShift) & 3u : 0u;
        const float3 offset = movesSoFar < kDdgiMaxMoves ? DdgiRelocate(previousOffset, volumeParams.x) : previousOffset;
        const bool moved = any(offset != previousOffset);
        const uint moves = movesSoFar + (moved ? 1u : 0u);
        const uint flags = kDdgiStateValid | (sBackfaces * 4u > kDdgiFixedRays ? kDdgiStateInactive : 0u) | changeFlags |
                           (moved ? kDdgiStateMoved : 0u) | (moves << kDdgiStateMovesShift);
        ProbeState[DdgiStateIndex(probeIndex)] =
            uint4(asuint(cell.x), asuint(cell.y), asuint(cell.z), (uint(volumeBase.w) << 8) | flags);
        ProbeState[DdgiOffsetIndex(probeIndex)] = uint4(asuint(offset), 0u);
    }
}
