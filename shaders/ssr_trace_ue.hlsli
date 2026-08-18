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
// STATUS: NOT THE DEFAULT, AND THREE HYPOTHESES FOR WHY HAVE BEEN KILLED BY MEASUREMENT:
//   1. self-intersection at the origin -> starting the ray offset along the normal (what the log
//      march does) changed agreement by 0.2 points. Not it.
//   2. steps too coarse -> 16 to 32 bought 3 points (68->71). Wrong slope for a parameter fix.
//   3. the ray stretching past its geometric length (bExtendRayToScreenBorder) -> clamping the
//      clip factor to 1 made it WORSE, 68 -> 39. The stretch is load-bearing, not the bug.
// The buffer itself shows the shape of the failure: hits land on the PALMS (short ray, fine step,
// so it finds a neighbouring frond) while the mirror floor gets a ray stretched across the frame
// with 16 samples on it -- the diagonal streaks. Whatever is missing is structural and has not been
// found; do not resume this by turning knobs.
//
// ORIGINAL NOTE -- measured, not guessed. Scored against the log march
// as the reference (hit-mask IoU on four grazing viewpoints of ssr_bronze_palms): **68-72% at UE's
// 16 steps, 71-75% at 32**, where the bar was 90%. The mask comes out torn, and ~20% of its hits
// are ones the log march does not make, including hits ABOVE the horizon -- on the palms
// themselves, which read as a tree reflecting itself.
//
// The cause is NOT in the transcription. It is that this tracer is HALF A PAIR: in
// SSRTReflections.usf every hit goes straight into `ReprojectHit` (velocity-reprojected into the
// PREVIOUS frame) and the whole output is flagged `SSR_OUTPUT_FOR_DENOISER`. Unreal never show this
// buffer; a torn mask is its normal state and the second half is what makes it whole. Our log
// march is self-contained and its mask is dense, so comparing the two raw is comparing a half to a
// whole. Finishing this means the SSR temporal resolve (reprojection + clamp, the shape
// gtao_temporal_cs.hlsl already has), not more steps -- 16->32 bought 3 points of agreement, which
// is the wrong slope for a parameter fix.
//
// Requires from the host shader: SSRHit / BuildSsrHit / the ssr* constants (ssr_trace_logmarch),
// `HzbFurthest`, `gSmpPoint`, `proj`, `hzbSize`, `hzbInvSize`.

// UE's shipping values. NumSteps is their highest single-ray quality; SlopeCompareToleranceScale is
// 4 for SSR (2 for SSGI). The mip creep per two samples is `(8/NumSteps) * Roughness`.
static const uint  ssrUeNumSteps = 16u;
static const float ssrUeStartMipLevel = 0.0f;
static const float ssrUeSlopeCompareToleranceScale = 4.0f;

// Scale-down factor that makes RayStepScreen end exactly on the viewport edge, so `NumSteps`
// always spans the VISIBLE part of the ray instead of wasting most of them off screen. Verbatim.
float SsrUeStepFactorToClipAtScreenEdge(float2 rayStartScreen, float2 rayStepScreen)
{
    const float rayStepScreenInvFactor = 0.5f * length(rayStepScreen);
    const float2 s = 1.0f - max(abs(rayStepScreen + rayStartScreen * rayStepScreenInvFactor) -
                                rayStepScreenInvFactor, 0.0f) / abs(rayStepScreen);
    return min(s.x, s.y) / rayStepScreenInvFactor;
}

SSRHit TraceSSR_UeHzb(float3 Pv, float3 Nv, float2 startUv, float startDeviceZ, float2 pixelCoord)
{
    SSRHit outv;
    outv.uv = startUv;
    outv.visibility = 0.0f;
    outv.hit = 0;

    const float3 unitPositionFrom = normalize(Pv);
    const float3 pivot = normalize(reflect(unitPositionFrom, Nv));
    if (pivot.z <= 0.0f)
    {
        return outv; // same rejection the other tracer applies, so an A/B compares the SEARCH only
    }

    // --- InitScreenSpaceRayFromWorldSpace -----------------------------------------------------
    // We are already in view space, so their TranslatedWorldToView is the identity here and
    // TranslatedWorldToClip is just `proj`.
    //
    // NOTE THE RAY LENGTH: UE pass `WorldTMax = SceneDepth`, i.e. THE RAY IS AS LONG AS THE PIXEL
    // IS FAR. A reflection on a surface 40 units away searches 40 units, not a fixed 100. That one
    // choice is most of why 16 steps is enough for them -- the step size scales with the scene.
    // THE RAY LENGTH IS UE'S ONE ASSUMPTION THAT DOES NOT TRANSFER, and the aim probe is what
    // showed it: their `WorldTMax = SceneDepth` says a surface searches as far as it is from the
    // camera. That is reasonable when reflections are a supporting effect -- a near surface only
    // reflects near things. Here the reflector IS the scene: floor a metre from the camera has to
    // reflect palms thirty metres away, and with WorldTMax = 1m the ray stops long before them.
    // Measured: the UE march's hits sat systematically CLOSER than the log march's (median vertical
    // aim -0.233 vs -0.288) and its spurious downward hits clustered in the near-floor bands.
    // So the search distance comes from the same budget the log march uses.
    const float sceneDepth = Pv.z;
    const float worldTMax = ssrMaxDistanceVS;
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
                                 (rayStartScreen.z - rayDepthScreen.z) * ssrUeSlopeCompareToleranceScale);

    // --- CastScreenSpaceRay -------------------------------------------------------------------
    // Their HZB covers a sub-rect of its texture and needs UvFactor/InvFactor; ours covers the
    // whole target, so both factors are 1 and drop out.
    float3 rayStartUVz = float3(rayStartScreen.xy * float2(0.5f, -0.5f) + 0.5f, rayStartScreen.z);
    float3 rayStepUVz = float3(rayStepScreen.xy * float2(0.5f, -0.5f), rayStepScreen.z);

    const float step = 1.0f / (float)ssrUeNumSteps;
    compareTolerance *= step;
    rayStepUVz *= step;

    // Dither the phase so the fixed step pattern does not band; UE take this from a blue-noise
    // table, we reuse the log march's hash so the two techniques share the same noise character.
    const float stepOffset = Hash12(pixelCoord * SSR_TRACE_INV_SCREEN_SIZE) - 0.5f;
    const float3 rayUVz = rayStartUVz + rayStepUVz * stepOffset;

    // Roughness would creep the mip upward by (8/NumSteps)*Roughness every two samples. The SSR
    // pass has no roughness bound (it reads GB1's normal only) and the reflection is blurred by
    // roughness afterwards anyway, so this stays at the mirror case UE take for Roughness < 0.1.
    const float level = ssrUeStartMipLevel;

    float lastDiff = 0.0f;
    bool foundHit = false;
    float4 sampleDepthDiff = 0.0f.xxxx;
    bool4 sampleHit = bool4(false, false, false, false);
    uint i = 0u;

    // Batches of four, exactly as UE do -- four samples issued together hide each other's latency.
    for (i = 0u; i < ssrUeNumSteps; i += 4u)
    {
        float4 samplesZ;
        float2 samplesUV[4];
        [unroll] for (uint j = 0u; j < 4u; ++j)
        {
            const float t = (float)i + (float)(j + 1u);
            samplesUV[j] = rayUVz.xy + t * rayStepUVz.xy;
            samplesZ[j] = rayUVz.z + t * rayStepUVz.z;
        }

        float4 sampleDepth;
        [unroll] for (uint k = 0u; k < 4u; ++k)
        {
            sampleDepth[k] = HzbFurthest.SampleLevel(gSmpPoint, samplesUV[k], level).r;
        }

        sampleDepthDiff = samplesZ - sampleDepth;
        // A hit is `-2*tolerance < diff < 0`, written as UE write it. Device Z 0 is the far plane
        // under reversed-Z, and a "hit" on the far plane is the sky, not geometry.
        sampleHit = and(abs(sampleDepthDiff + compareTolerance) < compareTolerance,
                        sampleDepth != 0.0f);

        [unroll] for (uint m = 0u; m < 4u; ++m)
        {
            foundHit = foundHit || sampleHit[m];
        }
        if (foundHit)
        {
            break;
        }
        lastDiff = sampleDepthDiff.w;
    }

    if (!foundHit)
    {
        outv.uv = (rayUVz + rayStepUVz * (float)i).xy;
        return outv;
    }

    // Which of the four hit first, and the pair of depth differences that straddle the surface.
    float depthDiff0 = sampleDepthDiff[2];
    float depthDiff1 = sampleDepthDiff[3];
    float time0 = 3.0f;
    if (sampleHit[2]) { depthDiff0 = sampleDepthDiff[1]; depthDiff1 = sampleDepthDiff[2]; time0 = 2.0f; }
    if (sampleHit[1]) { depthDiff0 = sampleDepthDiff[0]; depthDiff1 = sampleDepthDiff[1]; time0 = 1.0f; }
    if (sampleHit[0]) { depthDiff0 = lastDiff;           depthDiff1 = sampleDepthDiff[0]; time0 = 0.0f; }
    time0 += (float)i;

    // Line-segment intersection between the two straddling samples: one lerp instead of a binary
    // search. UE tried a binary search here and left it disabled -- the `#if 0` is still in their
    // source -- because the interpolation is as good and costs four fewer taps.
    const float timeLerp = saturate(depthDiff0 / (depthDiff0 - depthDiff1));
    const float3 hitUVz = rayUVz + rayStepUVz * (time0 + timeLerp);

    // Hand the result to the SHARED fade so a technique A/B differs only in the search. The
    // thickness the fade wants is in view units; the tolerance is in device Z, so convert the
    // interval this hit was accepted within.
    const float hitViewZ = DepthToViewZ_Fast(hitUVz.z);
    const float behindViewZ = DepthToViewZ_Fast(max(hitUVz.z - 2.0f * compareTolerance, 1e-6f));
    const float thicknessVS = max(abs(behindViewZ - hitViewZ), 1e-4f);
    return BuildSsrHit(pivot, unitPositionFrom, Pv, hitUVz.xy, hitUVz.z, thicknessVS, 0.0f);
}

#endif // SSR_TRACE_UE_HLSLI
