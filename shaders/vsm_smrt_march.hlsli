// SMRT's march (UE's VirtualShadowMapSMRTTemplate.ush): no include guard, it is included once per kind of ray, as theirs, because the flips in it
// cost a measurement each and must exist once. Before including it define:
//   VSM_SMRT_MARCH_NAME             the function's name
//   VSM_SMRT_RAY_STRUCT             the ray's state
//   VSM_SMRT_MARCH_PARAMS           what the finder needs, each with its leading comma
//   VSM_SMRT_FIND_SAMPLE(st, time)  a VsmSmrtSample; its referenceDepth is set even when it is not valid
// UE's SMRTRayCast, with the three direct-Z flips marked.
//
// The march walks from the LIGHT back to the receiver: sampleTime runs from ~1 down to 0, and 0 is
// the receiver itself (UE force the last step to exactly 0). The squaring of the parameter packs
// the samples toward the receiver, which is where contact detail lives.
VsmSmrtResult VSM_SMRT_MARCH_NAME(inout VSM_SMRT_RAY_STRUCT st, int numSteps, float stepOffset VSM_SMRT_MARCH_PARAMS)
{
    // UE's magic initializer. Cannot be a flag: they need the register to disappear under DCE, and
    // depthHistory can legitimately go negative (to about -2) when a ray crosses a clipmap boundary
    // and the depth range expands.
    // Clamped BEFORE anything uses it: the hang guard described at the top of vsm_smrt.hlsli. 0 steps is one sample, at the
    // receiver (UE's for a light of no size: every ray of it goes to one point).
    const uint steps = min((uint)max(numSteps, 0), VSM_SMRT_MAX_SAMPLES_PER_RAY);

    const float kDepthHistoryNotSet = -10000.0f;
    float depthHistory = kDepthHistoryNotSet;
    float depthHistoryTime = -1.0f;
    float depthSlope = 0.0f;

    const float timeScale = (steps > 0u) ? -1.0f / (float)steps : 0.0f;
    const float timeBias  = 1.0f + (1.0f - stepOffset) * timeScale;

    // Not used on the first iteration, and always written before the iteration that reads it.
    float prevReferenceDepth = -1.0f;

    VsmSmrtResult result;
    result.validHit = false;
    result.hitDepth = -1.0f;

    // Bounded by a LITERAL, with the CB-derived count only able to end it early -- see the caps at
    // the top. `<=` because UE's march takes one extra sample, forced to the receiver itself.
    [loop] for (uint i = 0u; i <= VSM_SMRT_MAX_SAMPLES_PER_RAY; ++i)
    {
        if (i > steps) { break; }
        const float tRaw = timeScale * (float)i + timeBias;
        const float sampleTime = (i == steps) ? 0.0f : (tRaw * tRaw); // UE's Pow2

        const VsmSmrtSample s = VSM_SMRT_FIND_SAMPLE(st, sampleTime);
        const float referenceDepth = s.referenceDepth;

        // FLIP (UE: = Sample.ExtrapolateSlope). Depth falls toward the light here, so a surface along the ray has a negative slope in time.
        if (s.resetExtrapolation) { depthSlope = -s.extrapolateSlope; }

        if (!s.valid) { continue; }

        const float sampleDepth = s.sampleDepth;
        if (depthHistory == kDepthHistoryNotSet)
        {
            // First valid sample: a plain depth compare, no tolerance yet (there is no previous
            // reference depth to build one from).
            depthHistory = sampleDepth;
            depthHistoryTime = sampleTime;
            if (sampleDepth < referenceDepth) // FLIP (UE: >). Occluder nearer the light.
            {
                result.validHit = true;
                result.hitDepth = sampleDepth;
                return result;
            }
        }
        else
        {
            // THE TOLERANCE. This is what replaces our constant depth bias, and the reason it can:
            // it is this pixel's own depth step along the ray, so it is a property of the geometry
            // and the march rather than a number someone tuned. UE's comment: "Add a small relative
            // error to the comparison to avoid missing surfaces due to numeric precision issues.
            // Without this there are occasionally flickering fireflies in fully shadowed regions".
            const float deltaReferenceDepth = referenceDepth - prevReferenceDepth;
            const float kEpsScale = 1.05f;
            const float compareTolerance = abs(deltaReferenceDepth) * kEpsScale;

            // FLIP (UE: sampleDepth - referenceDepth). The ray has walked behind the sampled
            // surface, i.e. that surface sits substantially nearer the light than the ray point.
            const bool behind = (referenceDepth - sampleDepth) > compareTolerance;
            const float deltaHistoryTime = sampleTime - depthHistoryTime;
            float depthForComparison = sampleDepth;

            if (behind)
            {
                // Extrapolate the occluder's surface behind itself, so a penumbra keeps widening
                // instead of ending at the silhouette. With extrapolateMaxSlope == 0 the slope is
                // clamped to 0 below and this collapses to depthHistory, which is UE's own
                // "extrapolation disabled" behaviour.
                depthForComparison = depthSlope * deltaHistoryTime + depthHistory;
            }
            else if (sampleDepth != depthHistory)
            {
                const float slopeClamp = s.extrapolateSlope;
                depthSlope = clamp((sampleDepth - depthHistory) / deltaHistoryTime,
                                   -slopeClamp, slopeClamp);
                depthHistory = sampleDepth;
                depthHistoryTime = sampleTime;
            }

            // FLIP (UE: referenceDepth - depthForComparison). The test `abs(d + h) < h` with
            // h = tolerance/2 accepts d in (-tolerance, 0) -- i.e. the compared surface is nearer
            // the light than the ray point, but by less than one step. Flipping the subtraction is
            // what keeps that window on the occluding side under direct Z.
            const float depthDiff = depthForComparison - referenceDepth;
            const float halfCompareTolerance = 0.5f * compareTolerance;
            if (abs(depthDiff + halfCompareTolerance) < halfCompareTolerance)
            {
                result.validHit = true;
                result.hitDepth = depthForComparison;
                return result;
            }
        }

        prevReferenceDepth = referenceDepth;
    }

    return result;
}
