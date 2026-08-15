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
    uint  pad0, pad1, pad2;
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
    const float loWanted = totalF * lowPercentile;
    const float hiWanted = totalF * highPercentile;

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

    const float meteredLogLum = weight > 0.0f ? (weightedLogLum / weight) : minLogLum;

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
        // Linear in stops/second, which is what the setting claims to be. speedUp applies when the
        // scene gets brighter (target EV rises) -- the eye's light adaptation is the fast direction.
        const float rate = (targetEv > prevEv) ? speedUp : speedDown;
        const float maxStep = max(rate, 0.0f) * max(deltaTime, 0.0f);
        adapted = prevEv + clamp(targetEv - prevEv, -maxStep, maxStep);
    }
    adapted = clamp(adapted, minEv100, maxEv100);

    ExposureValue.Store(0, asuint(adapted));
    ExposureValue.Store(4, asuint(exp2(loLogLum)));
    ExposureValue.Store(8, asuint(exp2(hiLogLum)));
    ExposureValue.Store(12, asuint(targetEv));
}
