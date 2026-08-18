#ifndef SSR_TRACE_UE_HLSLI
#define SSR_TRACE_UE_HLSLI

// Unreal's OWN screen-space reflection ray cast, transcribed: SSRT/SSRTRayCast.ush,
// `InitScreenSpaceRayFromWorldSpace` + `CastScreenSpaceRay`, as called by SSRT/SSRTReflections.usf.
//
// WHY THIS EXISTS AND THE HiZ TRAVERSAL DOES NOT ANSWER THE SAME QUESTION. `TraceHZB`
// (HZBTracing.ush), the stackless mip walk, is NOT what Unreal reflect with: grep the shader tree
// and its only callers are LumenScreenTracing.ush, LumenHairTracing.ush and
// FirstPersonSelfShadow.usf. A Lumen screen trace is SHORT and has a fallback -- miss, and Lumen
// keeps going in voxels / mesh SDFs / hardware RT -- so 50 iterations is generous there. Ours had
// no fallback and had to cross a mirror floor end to end, which is why it ran out of budget on
// grazing views and lost the far half of every trunk reflection.
//
// What Unreal's SSR actually does is far simpler, and this is it:
//   * march the ray in SCREEN space with a FIXED number of steps (8/12/16, capped at 24),
//   * sample the HZB at a FIXED mip -- `StartMipLevel = 1.0`, and the level only creeps upward
//     with roughness, never with distance. The pyramid is used as CHEAP PRE-FILTERED DEPTH, not as
//     a hierarchy to skip empty space,
//   * and it reads the FURTHEST pyramid (`EHZBType::FurthestHZB`, ScreenSpaceRayTracing.cpp) --
//     the same one our GTAO already builds. The closest chain is not needed here at all.
//
// P13 STATUS: THE SLANTED-STREAK DEFECT IS FIXED. The ray geometry was correct; the sampling
// phase was not. This port fed Hash12(pixelCoord * invScreenSize), while UE feed their noise
// function integer pixel coordinates. Adjacent pixels therefore received almost the same phase,
// and 16 sparse samples along long grazing rays lined up into the coherent diagonal comb visible
// in the raw hit mask. Hashing pixelCoord directly decorrelates the miss/error field spatially:
// the comb disappears, vertical reflected trunks stay vertical, and the existing blur/temporal
// resolve can filter the remaining fine-grained noise. This changes no depth taps and measured
// 0.035 ms versus the log march's 0.185 ms on the P13 Release capture.
//
// UE also changes that phase every frame: `InterleavedGradientNoise(SvPosition.xy,
// View.StateFrameIndexMod8)`. A spatial-only hash leaves the same 16 holes in every frame, so the
// temporal pass cannot recover the missing samples and merely preserves a pixel-torn lattice.
// The exact modulo-8 UE sequence below lets eight temporal frames cover complementary positions
// along the ray, again without adding a single depth tap per frame.
//
// Three earlier hypotheses were killed by measurement and remain recorded so they are not retried:
//   1. self-intersection at the origin -> starting the ray offset along the normal (what the log
//      march does) changed agreement by 0.2 points. Not it.
//   2. steps too coarse -> 16 to 32 bought 3 points (68->71). Wrong slope for a parameter fix.
//   3. the ray stretching past its geometric length (bExtendRayToScreenBorder) -> clamping the
//      clip factor to 1 made it WORSE, 68 -> 39. The stretch is load-bearing, not the bug.
//
// HISTORICAL NOTE -- measured, not guessed. Scored against the log march
// as the reference (hit-mask IoU on four grazing viewpoints of ssr_bronze_palms): **68-72% at UE's
// 16 steps, 71-75% at 32**, where the bar was 90%. The mask comes out torn, and ~20% of its hits
// are ones the log march does not make, including hits ABOVE the horizon -- on the palms
// themselves, which read as a tree reflecting itself.
//
// This tracer is still HALF A PAIR in another sense: in SSRTReflections.usf every hit goes through
// `ReprojectHit` and the output is filtered. The existing ssr_temporal_cs.hlsl remains required for
// stability under projection jitter. Temporal filtering hid some of the old comb, but could not
// make a spatially correlated sampling pattern geometrically correct; the phase fix belongs here.
//
// Requires from the host shader: SSRHit / BuildSsrHit / the ssr* constants (ssr_trace_logmarch),
// `HzbFurthest`, `gSmpPoint`, `proj`, `hzbSize`, `hzbInvSize`.

// The host supplies UE's quality permutation (steps/rays/glossy) plus the source constants that
// are useful to expose while diagnosing grazing reflections. Stock UE hard-code StartMipLevel=1
// and SlopeCompareToleranceScale=4. Their quality mappings live in SceneFrameData.h.

// Scale-down factor that makes RayStepScreen end exactly on the viewport edge, so `NumSteps`
// always spans the VISIBLE part of the ray instead of wasting most of them off screen. Verbatim.
float SsrUeStepFactorToClipAtScreenEdge(float2 rayStartScreen, float2 rayStepScreen)
{
    const float rayStepScreenInvFactor = 0.5f * length(rayStepScreen);
    const float2 s = 1.0f - max(abs(rayStepScreen + rayStartScreen * rayStepScreenInvFactor) -
                                rayStepScreenInvFactor, 0.0f) / abs(rayStepScreen);
    return min(s.x, s.y) / rayStepScreenInvFactor;
}

// RandomInterleavedGradientNoise.ush verbatim. The integer pixel coordinate removes spatial
// correlation; FrameId changes the fixed-step phase over UE's eight-frame temporal cycle.
float SsrUeInterleavedGradientNoise(float2 uv, float frameId)
{
    uv += frameId * (float2(47.0f, 17.0f) * 0.695f);
    const float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(uv, magic.xy)));
}

// RandomPCG.ush::Rand3DPCG16, MonteCarlo.ush::Hammersley16 and the Duff tangent basis. These are
// the exact building blocks SSRTReflections.usf use for High/Epic roughness-aware ray directions.
uint3 SsrUeRand3DPCG16(int3 p)
{
    uint3 v = uint3(p);
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v >> 16u;
}

float2 SsrUeHammersley16(uint index, uint numSamples, uint2 random)
{
    const float e1 = frac((float)index / max((float)numSamples, 1.0f) +
                          (float)random.x * (1.0f / 65536.0f));
    const float e2 = (float)((reversebits(index) >> 16u) ^ random.y) * (1.0f / 65536.0f);
    return float2(e1, e2);
}

float3x3 SsrUeTangentBasis(float3 tangentZ)
{
    const float signZ = tangentZ.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -rcp(signZ + tangentZ.z);
    const float b = tangentZ.x * tangentZ.y * a;
    const float3 tangentX = float3(1.0f + signZ * a * tangentZ.x * tangentZ.x,
                                   signZ * b, -signZ * tangentZ.x);
    const float3 tangentY = float3(b, signZ + a * tangentZ.y * tangentZ.y, -tangentZ.y);
    return float3x3(tangentX, tangentY, tangentZ);
}

// MonteCarlo.ush::ImportanceSampleVisibleGGX. SSR only consumes the sampled micronormal; its PDF
// is omitted because UE defer the environment BRDF to composition too.
float3 SsrUeImportanceSampleVisibleGGX(float2 e, float alpha, float3 v)
{
    const float2 alpha2 = max(alpha, 1.0e-4f).xx;
    const float3 vh = normalize(float3(alpha2 * v.xy, v.z));
    const float phi = 6.28318530718f * e.x;

    const float a = saturate(alpha);
    const float s = 1.0f + length(v.xy);
    const float aSq = a * a;
    const float sSq = s * s;
    const float k = (sSq - aSq * sSq) / max(sSq + aSq * v.z * v.z, 1.0e-6f);
    const float z = lerp(1.0f, -k * vh.z, e.y);
    const float sinTheta = sqrt(saturate(1.0f - z * z));
    float3 h = float3(sinTheta * cos(phi), sinTheta * sin(phi), z) + vh;
    h = normalize(float3(alpha2 * h.xy, max(0.0f, h.z)));
    return h;
}

bool SsrUeReadExactRaySample(float3 rayUVz, float3 rayStepUVz, float time,
                             out float3 sampleUVz, out float depthDiff)
{
    sampleUVz = rayUVz + rayStepUVz * time;
    if (any(sampleUVz.xy < 0.0f) || any(sampleUVz.xy > 1.0f))
    {
        depthDiff = 0.0f;
        return false;
    }

    const float exactDepth = ReadDepth(sampleUVz.xy);
    depthDiff = sampleUVz.z - exactDepth;
    return exactDepth != 0.0f;
}

bool SsrUeExactDepthAccept(float depthDiff, float tolerance)
{
    return abs(depthDiff + tolerance) < tolerance;
}

// A coarse HZB tile says that some surface in its footprint may cross the ray. It does not prove
// the returned exact pixel owns that surface. Resolve the candidate on mip0/full depth; a rejected
// candidate is allowed to fall through to later coarse samples instead of becoming a permanent
// hole. `refineSteps` is additional local subdivision; the accepted thickness shrinks with it.
bool SsrUeResolveCoarseCandidate(float3 rayUVz, float3 rayStepUVz,
                                 float time0, float time1,
                                 float coarseDiff0, float coarseDiff1,
                                 float coarseTolerance, uint refineSteps,
                                 out float3 hitUVz, out float acceptedTolerance)
{
    const uint safeRefineSteps = min(refineSteps, 8u);
    acceptedTolerance = coarseTolerance / max((float)safeRefineSteps, 1.0f);

    const float denom = coarseDiff0 - coarseDiff1;
    const float timeLerp = abs(denom) > 1.0e-8f ? saturate(coarseDiff0 / denom) : 1.0f;
    const float coarseTime = lerp(time0, time1, timeLerp);
    float exactDiff = 0.0f;
    if (SsrUeReadExactRaySample(rayUVz, rayStepUVz, coarseTime, hitUVz, exactDiff) &&
        SsrUeExactDepthAccept(exactDiff, acceptedTolerance))
    {
        return true;
    }

    if (safeRefineSteps == 0u)
    {
        return false;
    }

    bool previousValid = false;
    float previousTime = time0;
    float previousDiff = 0.0f;
    [loop] for (uint s = 0u; s <= safeRefineSteps; ++s)
    {
        const float t = lerp(time0, time1, (float)s / (float)safeRefineSteps);
        float3 sampleUVz;
        float sampleDiff = 0.0f;
        const bool valid = SsrUeReadExactRaySample(rayUVz, rayStepUVz, t, sampleUVz, sampleDiff);
        if (valid && SsrUeExactDepthAccept(sampleDiff, acceptedTolerance))
        {
            hitUVz = sampleUVz;
            return true;
        }

        // Reversed Z: positive is in front of scene depth, negative is behind. Interpolate the
        // first exact sign change and verify that point once more to avoid accepting a depth edge.
        if (valid && previousValid && previousDiff >= 0.0f && sampleDiff < 0.0f)
        {
            const float crossingDenom = previousDiff - sampleDiff;
            const float crossingAlpha = crossingDenom > 1.0e-8f
                ? saturate(previousDiff / crossingDenom) : 1.0f;
            const float crossingTime = lerp(previousTime, t, crossingAlpha);
            float crossingDiff = 0.0f;
            if (SsrUeReadExactRaySample(rayUVz, rayStepUVz, crossingTime,
                                        hitUVz, crossingDiff) &&
                SsrUeExactDepthAccept(crossingDiff, acceptedTolerance))
            {
                return true;
            }
        }

        previousValid = valid;
        previousTime = t;
        previousDiff = sampleDiff;
    }
    return false;
}

SSRHit SsrUeBuildHit(float3 Pv, float3 unitPositionFrom, float3 pivot,
                     float3 hitUVz, float compareTolerance)
{
    const float hitViewZ = DepthToViewZ_Fast(hitUVz.z);
    const float behindViewZ = DepthToViewZ_Fast(max(hitUVz.z - 2.0f * compareTolerance, 1e-6f));
    const float thicknessVS = max(abs(behindViewZ - hitViewZ), 1e-4f);
    return BuildSsrHit(pivot, unitPositionFrom, Pv, hitUVz.xy, hitUVz.z,
                       thicknessVS, 0.0f);
}

SSRHit TraceSSR_UeHzbRay(float3 Pv, float3 unitPositionFrom, float3 pivot, float roughness,
                         float2 startUv, float2 pixelCoord, uint frameIndexMod8, uint requestedSteps)
{
    SSRHit outv;
    outv.uv = startUv;
    outv.deviceZ = 0.0f;
    outv.visibility = 0.0f;
    outv.hit = 0;

    if (pivot.z <= 0.0f)
    {
        return outv; // same rejection the other tracer applies, so an A/B compares the SEARCH only
    }

    // --- InitScreenSpaceRayFromWorldSpace -----------------------------------------------------
    // We are already in view space, so their TranslatedWorldToView is the identity here and
    // TranslatedWorldToClip is just `proj`.
    //
    // UE passes WorldTMax = SceneDepth: the ray budget scales with the reflector's distance from
    // the camera. Keeping the generic 100-unit LogMarch budget here made the 16 fixed samples far
    // too permissive on high, oblique views and produced the long false-hit curtains reported by
    // the user. This technique is the UE technique, so retain its coupled distance assumption.
    const float sceneDepth = Pv.z;
    const float worldTMax = sceneDepth;
    const float rayEndDistance = (pivot.z < 0.0f)
        ? min(-0.95f * sceneDepth / pivot.z, worldTMax)
        : worldTMax;

    const float3 rayEndView = Pv + pivot * rayEndDistance;

    const float4 rayStartClip = mul(float4(Pv, 1.0f), proj);
    const float4 rayEndClip = mul(float4(rayEndView, 1.0f), proj);
    const float3 rayStartScreen = rayStartClip.xyz / rayStartClip.w;
    const float3 rayEndScreen = rayEndClip.xyz / rayEndClip.w;

    // A point at the same screen XY but pushed back along view Z by the ray's length. The gap
    // between its device Z and the start's is how much depth ONE ray's worth of distance covers
    // here, which is what the comparison tolerance is derived from.
    const float4 rayDepthClip = rayStartClip + mul(float4(0.0f, 0.0f, rayEndDistance, 0.0f), proj);
    const float3 rayDepthScreen = rayDepthClip.xyz / rayDepthClip.w;

    float3 rayStepScreen = rayEndScreen - rayStartScreen;
    rayStepScreen *= SsrUeStepFactorToClipAtScreenEdge(rayStartScreen.xy, rayStepScreen.xy);

    float compareTolerance = max(abs(rayStepScreen.z),
                                 (rayStartScreen.z - rayDepthScreen.z) *
                                 ueSlopeCompareToleranceScale);

    // --- CastScreenSpaceRay -------------------------------------------------------------------
    // Their HZB covers a sub-rect of its texture and needs UvFactor/InvFactor; ours covers the
    // whole target, so both factors are 1 and drop out.
    float3 rayStartUVz = float3(rayStartScreen.xy * float2(0.5f, -0.5f) + 0.5f, rayStartScreen.z);
    float3 rayStepUVz = float3(rayStepScreen.xy * float2(0.5f, -0.5f), rayStepScreen.z);

    const uint numSteps = clamp(requestedSteps, 4u, 64u);
    const float step = 1.0f / (float)numSteps;
    compareTolerance *= step;
    rayStepUVz *= step;

    // Dither the phase so the fixed step pattern does not band. UE feeds integer PIXEL coordinates
    // and StateFrameIndexMod8 to this noise. Normalising the coordinates by screen size correlates
    // adjacent pixels; omitting the frame index repeats the same sparse holes forever.
    const float stepOffset = SsrUeInterleavedGradientNoise(pixelCoord, (float)frameIndexMod8) - 0.5f;
    const float3 rayUVz = rayStartUVz + rayStepUVz * stepOffset;

    float level = min(ueStartMipLevel, (float)max((int)hzbMipCount - 1, 0));

    float lastDiff = 0.0f;
    float4 sampleDepthDiff = 0.0f.xxxx;
    bool4 sampleHit = bool4(false, false, false, false);
    uint rejectedCandidates = 0u;
    float2 lastUv = startUv;

    // Batches of four, exactly as UE do -- four samples issued together hide each other's latency.
    [loop] for (uint i = 0u; i < numSteps; i += 4u)
    {
        float4 samplesZ;
        float2 samplesUV[4];
        float4 samplesMip;
        [unroll] for (uint j = 0u; j < 4u; ++j)
        {
            const float t = (float)i + (float)(j + 1u);
            samplesUV[j] = rayUVz.xy + t * rayStepUVz.xy;
            samplesZ[j] = rayUVz.z + t * rayStepUVz.z;
        }

        samplesMip.xy = level;
        level += (8.0f / (float)numSteps) * saturate(roughness);
        samplesMip.zw = level;
        level += (8.0f / (float)numSteps) * saturate(roughness);

        float4 sampleDepth;
        [unroll] for (uint k = 0u; k < 4u; ++k)
        {
            sampleDepth[k] = HzbFurthest.SampleLevel(gSmpPoint, samplesUV[k],
                min(samplesMip[k], (float)max((int)hzbMipCount - 1, 0))).r;
        }

        sampleDepthDiff = samplesZ - sampleDepth;
        // A hit is `-2*tolerance < diff < 0`, written as UE write it. Device Z 0 is the far plane
        // under reversed-Z, and a "hit" on the far plane is the sky, not geometry.
        sampleHit = and(abs(sampleDepthDiff + compareTolerance) < compareTolerance,
                        sampleDepth != 0.0f);

        [unroll] for (uint candidate = 0u; candidate < 4u; ++candidate)
        {
            if (!sampleHit[candidate])
            {
                continue;
            }

            const float time0 = (float)(i + candidate);
            const float time1 = time0 + 1.0f;
            const float depthDiff0 = candidate == 0u ? lastDiff : sampleDepthDiff[candidate - 1u];
            const float depthDiff1 = sampleDepthDiff[candidate];
            float3 hitUVz;
            float acceptedTolerance = compareTolerance;

            if (ueConfirmRetries == 0u)
            {
                const float denom = depthDiff0 - depthDiff1;
                const float timeLerp = abs(denom) > 1.0e-8f
                    ? saturate(depthDiff0 / denom) : 1.0f;
                hitUVz = rayUVz + rayStepUVz * lerp(time0, time1, timeLerp);
                return SsrUeBuildHit(Pv, unitPositionFrom, pivot, hitUVz, acceptedTolerance);
            }

            if (SsrUeResolveCoarseCandidate(rayUVz, rayStepUVz, time0, time1,
                                            depthDiff0, depthDiff1, compareTolerance,
                                            ueRefineSteps, hitUVz, acceptedTolerance))
            {
                return SsrUeBuildHit(Pv, unitPositionFrom, pivot, hitUVz, acceptedTolerance);
            }

            ++rejectedCandidates;
            if (rejectedCandidates > ueConfirmRetries)
            {
                return outv;
            }
        }

        lastDiff = sampleDepthDiff.w;
        lastUv = samplesUV[3];
    }

    outv.uv = lastUv;
    return outv;
}

#endif // SSR_TRACE_UE_HLSLI
