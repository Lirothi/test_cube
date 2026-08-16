// Photographic camera, step P2: percentile solve + temporal eye adaptation.
//
// Reads the 256-bin log2-luminance histogram, clips both tails at the authored percentiles, turns
// what is left into an EV100, and adapts the persistent value towards it at independent up/down
// rates. Percentile clipping is what rejects a sun glint or a patch of sky WITHOUT any hard-coded
// notion of water or sky (plan P2 item 4).
//
// t0: histogram (raw, 256 uint bins)
// u0: persistent exposure record (raw, 4 floats): adapted EV100, low percentile luminance,
//     high percentile luminance, target EV100.

#define EXPOSURE_SOLVE_CS_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

ByteAddressBuffer Histogram : register(t0);
RWByteAddressBuffer ExposureValue : register(u0);
SamplerState gSmp : register(s0);

cbuffer ExposureSolveCB : register(b0)
{
    float minLogLum;
    float logLumRange;
    float lowPercentile;
    float highPercentile;

    float compensationEv;
    float minEv100;
    float maxEv100;
    float deltaTime;      // already capped on the CPU

    float speedUp;        // stops/second when the scene gets BRIGHTER (target EV rises)
    float speedDown;      // stops/second when the scene gets DARKER
    float manualEv100;
    uint  autoExposure;   // 0 = hold manualEv100

    uint  resetHistory;   // 1 = seed from this frame's target instead of adapting
    float startDistance;  // stops; beyond this the adaptation is linear, inside it exponential
    // Slope-match factors, computed on the CPU because they depend only on speed and
    // startDistance. They make the exponential's slope equal the linear's at the switch point, so
    // the two halves join smoothly instead of visibly changing rate mid-transition.
    float exponentialUpM;
    float exponentialDownM;
    float blackBucketInfluence;
};

static const uint kBins = 256u;

groupshared uint gBins[kBins];

[numthreads(8, 8, 1)]
[RootSignature(EXPOSURE_SOLVE_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    // Single 8x8 group: 64 threads, 4 bins each.
    const uint thread = dtid.y * 8u + dtid.x;
    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        const uint bin = thread * 4u + i;
        gBins[bin] = Histogram.Load(bin * 4u);
    }
    GroupMemoryBarrierWithGroupSync();

    if (thread != 0u)
    {
        return;
    }

    // 256 serial iterations on one thread. Trivial next to the histogram build, and a parallel
    // prefix sum here would be more code than the work it saves.
    // The darkest bucket can be scaled down: large regions of pure black (an unlit interior,
    // letterboxing) would otherwise drag the meter toward exposing for nothing.
    gBins[0] = (uint)((float)gBins[0] * saturate(blackBucketInfluence));

    uint total = 0u;
    for (uint b = 0u; b < kBins; ++b)
    {
        total += gBins[b];
    }

    const float prevEv = asfloat(ExposureValue.Load(0));
    const bool prevValid = !isnan(prevEv) && !isinf(prevEv);

    if (total == 0u)
    {
        // Nothing valid was sampled. Hold the previous value rather than inventing one; if even
        // that is unusable, fall back to the middle of the authored clamp.
        const float held = prevValid ? prevEv : clamp(0.0f, minEv100, maxEv100);
        ExposureValue.Store(0, asuint(held));
        ExposureValue.Store(12, asuint(held));
        return;
    }

    const float totalF = (float)total;
    // A collapsed or inverted percentile window selects no samples at all, which used to leave the
    // weighted average at minLogLum and drive the exposure to the clamp -- a pure white frame. That
    // is trivially reachable by dragging the two sliders together, so widen a degenerate window
    // here rather than trusting every caller to keep them apart.
    float lowPct = min(lowPercentile, highPercentile);
    float highPct = max(lowPercentile, highPercentile);
    if (highPct - lowPct < 0.01f)
    {
        const float mid = 0.5f * (lowPct + highPct);
        lowPct = saturate(mid - 0.005f);
        highPct = saturate(mid + 0.005f);
    }
    const float loWanted = totalF * lowPct;
    const float hiWanted = totalF * highPct;

    // Walk the distribution once, accumulating only the part between the two percentiles. Bins are
    // weighted by their count so a wide flat region does not count the same as a narrow spike.
    float running = 0.0f;
    float weightedLogLum = 0.0f;
    float weight = 0.0f;
    float loLogLum = minLogLum;
    float hiLogLum = minLogLum + logLumRange;
    bool haveLo = false;
    for (uint bin = 0u; bin < kBins; ++bin)
    {
        const float count = (float)gBins[bin];
        if (count <= 0.0f)
        {
            continue;
        }
        const float binStart = running;
        const float binEnd = running + count;
        running = binEnd;

        // Fraction of this bin that falls inside the percentile window.
        const float lo = max(binStart, loWanted);
        const float hi = min(binEnd, hiWanted);
        const float inside = max(hi - lo, 0.0f);
        if (inside <= 0.0f)
        {
            continue;
        }

        const float binLogLum = minLogLum + (((float)bin + 0.5f) / (float)(kBins - 1u)) * logLumRange;
        weightedLogLum += binLogLum * inside;
        weight += inside;
        if (!haveLo)
        {
            loLogLum = binLogLum;
            haveLo = true;
        }
        hiLogLum = binLogLum;
    }

    // Falling back to minLogLum on an empty window would ask the camera to expose pure black, i.e.
    // open to the clamp and white out the frame. Hold the previous exposure instead.
    if (weight <= 0.0f)
    {
        const float held = prevValid ? prevEv : clamp(0.0f, minEv100, maxEv100);
        ExposureValue.Store(0, asuint(held));
        ExposureValue.Store(12, asuint(held));
        return;
    }
    const float meteredLogLum = weightedLogLum / weight;

    // EV100 = log2(L * S / K) with S = 100 and K = 12.5, i.e. log2(L) + 3. Mirrors
    // render::Ev100FromLuminance in PhotographicSettings.h; the two must not drift apart.
    const float meteredEv = meteredLogLum + 3.0f;

    // Positive compensation must BRIGHTEN the image, and a brighter image is a LOWER EV, hence the
    // subtraction. Getting this sign backwards is the classic way to make the slider feel inverted.
    float targetEv = meteredEv - compensationEv;
    if (autoExposure == 0u)
    {
        targetEv = manualEv100;
    }
    targetEv = clamp(targetEv, minEv100, maxEv100);

    float adapted;
    if (resetHistory != 0u || !prevValid || autoExposure == 0u)
    {
        // Plan section 6.4: the first frame after a reset uses the current metered target, never an
        // arbitrary default. Manual mode is instantaneous by definition.
        adapted = targetEv;
    }
    else
    {
        // Hybrid, matching UE's ComputeEyeAdaptation. FAR from the target (more than startDistance
        // stops) run linear, so a big transition takes a predictable, bounded time at the authored
        // stops/second. CLOSE to it run exponential, so the last fraction of a stop eases in
        // instead of arriving at full rate and stopping dead -- which is what a purely linear
        // adaptation looks like, and it reads as a mechanical snap rather than as vision.
        const float dt = max(deltaTime, 0.0f);
        const float diff = targetEv - prevEv;
        const float rate = (diff > 0.0f) ? speedUp : speedDown;
        const float m    = (diff > 0.0f) ? exponentialUpM : exponentialDownM;

        // NOTE: `linear` is an HLSL interpolation modifier and cannot be used as an identifier --
        // it fails with "modifiers must appear before type", which reads nothing like a name
        // clash. Same trap waits for `sample`, `centroid` and `precise`.
        const float maxStep = max(rate, 0.0f) * dt;
        const float linearStep = prevEv + clamp(diff, -maxStep, maxStep);

        const float factor = 1.0f - exp2(-dt * max(rate, 0.0f));
        const float exponentialStep = prevEv + diff * factor * m;

        adapted = (abs(diff) > startDistance) ? linearStep : exponentialStep;
    }
    adapted = clamp(adapted, minEv100, maxEv100);

    ExposureValue.Store(0, asuint(adapted));
    ExposureValue.Store(4, asuint(exp2(loLogLum)));
    ExposureValue.Store(8, asuint(exp2(hiLogLum)));
    ExposureValue.Store(12, asuint(targetEv));
}
