// Photographic camera, step P2: percentile solve + temporal eye adaptation.
//
// Reads the 256-bin log2-luminance histogram, clips both tails at the authored percentiles, turns
// what is left into an EV100, and adapts the persistent value towards it at independent up/down
// rates. Percentile clipping is what rejects a sun glint or a patch of sky WITHOUT any hard-coded
// notion of water or sky (plan P2 item 4).
//
// t0: histogram (raw, 256 uint bins)
// u0: persistent exposure record (raw, 4 floats): adapted EV100, low percentile luminance,
//     high percentile luminance, target EV100. (The same buffer carries the sun probe behind
//     them -- sun_probe.hlsli -- which this pass never touches.) Time-of-day E1 adds a record
//     behind the probe (kExposureCompensationOffset): the compensation in effect, and the EYE's
//     adaptation before the curve, which is what adapts from frame to frame.

#include "sun_probe.hlsli"

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
    // P16.13: MANUAL mode's own compensation. `compensationEv` above is the AUTO one. They were a
    // single field, which meant trimming a metered shot silently re-trimmed every fixed-exposure
    // shot in the same level and back again -- the two are different jobs. In auto it offsets what
    // the meter decided; in manual it offsets a number the author already chose by hand, so it is
    // a quick +/- on top of a solved shutter rather than a correction to a measurement.
    float manualCompensationEv;
    // Time-of-day E1: the compensation CURVE (CameraExposureSettings::exposureCurve*), up to six
    // points: x = the EV100 the eye is adapted to, ascending, y = compensation EV. 0 points = none.
    uint  curvePoints;
    float _padComp1;
    float _padComp2;
    float4 curveEvA;      // points 0-3
    float4 curveEvB;      // points 4-5 in xy
    float4 curveCompA;
    float4 curveCompB;
};

static const uint kBins = 256u;

groupshared uint gBins[kBins];

// Time-of-day E1: piecewise linear through the curve's points, flat beyond its ends, 0 with none.
// The GPU twin of render::EvaluateExposureCurve (PhotographicSettings.h); the two must agree. The
// loop bound is the literal maximum -- a bound read from the constant buffer is a trap here.
float ExposureCurve(float ev)
{
    const uint n = min(curvePoints, 6u);
    if (n == 0u)
    {
        return 0.0f;
    }
    const float xs[6] = { curveEvA.x, curveEvA.y, curveEvA.z, curveEvA.w, curveEvB.x, curveEvB.y };
    const float ys[6] = { curveCompA.x, curveCompA.y, curveCompA.z, curveCompA.w, curveCompB.x, curveCompB.y };
    if (ev <= xs[0])
    {
        return ys[0];
    }
    float result = ys[n - 1u];
    bool found = false;
    [unroll]
    for (uint i = 1u; i < 6u; ++i)
    {
        if (!found && i < n && ev <= xs[i])
        {
            const float span = xs[i] - xs[i - 1u];
            const float t = span > 1e-6f ? (ev - xs[i - 1u]) / span : 1.0f;
            result = ys[i - 1u] + t * (ys[i] - ys[i - 1u]);
            found = true;
        }
    }
    return result;
}

// Writes the frame's record. `eyeEv` is the adaptation BEFORE the curve (what the next frame adapts
// from), `cameraEv` what the frame is exposed at, `compensation` everything between the scene the eye
// is adapted to and the camera (the static trim plus the curve, EV, positive brightens) -- night
// vision adds it back to the camera's exposure to find the eye (tonemap_cs.hlsl).
void StoreExposure(float cameraEv, float targetEv, float eyeEv, float compensation)
{
    ExposureValue.Store(0, asuint(cameraEv));
    ExposureValue.Store(12, asuint(targetEv));
    ExposureValue.Store(kExposureCompensationOffset, asuint(compensation));
    ExposureValue.Store(kExposureCompensationOffset + 4u, asuint(eyeEv));
}

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

    // E1: the frame-to-frame state is the EYE's adaptation, before the curve -- the curve is applied on
    // top of it every frame, so it never feeds back into the adaptation. Without a curve the two are
    // the same number and this is what offset 0 always held.
    const float prevEv = asfloat(ExposureValue.Load(kExposureCompensationOffset + 4u));
    const bool prevValid = !isnan(prevEv) && !isinf(prevEv);
    const float prevCameraEv = asfloat(ExposureValue.Load(0));
    const float prevCompensation = asfloat(ExposureValue.Load(kExposureCompensationOffset));
    const bool prevCameraValid = prevValid && !isnan(prevCameraEv) && !isinf(prevCameraEv)
        && !isnan(prevCompensation) && !isinf(prevCompensation);

    if (total == 0u)
    {
        // Nothing valid was sampled. Hold the previous value rather than inventing one; if even
        // that is unusable, fall back to the middle of the authored clamp.
        const float held = prevValid ? prevEv : clamp(0.0f, minEv100, maxEv100);
        const float heldCamera = prevCameraValid ? prevCameraEv : held;
        StoreExposure(heldCamera, heldCamera, held, prevCameraValid ? prevCompensation : 0.0f);
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
        const float heldCamera = prevCameraValid ? prevCameraEv : held;
        StoreExposure(heldCamera, heldCamera, held, prevCameraValid ? prevCompensation : 0.0f);
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
        // P16.6b: compensation applies in MANUAL too. It used to be an auto-only trim, which left
        // a fixed-exposure level with no quick +/- at all -- changing the look meant re-solving the
        // shutter by hand, which is the arithmetic the camera controls exist to remove.
        // P16.13: and it is its OWN field, so trimming one mode does not move the other. Same sign
        // convention: positive brightens, hence the subtraction.
        targetEv = manualEv100 - manualCompensationEv;
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

    // Time-of-day E1: the compensation curve, keyed on the light the eye is ADAPTED to (the smoothed
    // value plus the static trim it carries, i.e. the scene's own EV) and applied OUTSIDE the
    // adaptation, like UE's EyeAdaptationCommon. Unlike UE it reads the smoothed value, not this
    // frame's meter: walking into a dark room then darkens the picture as the eye adapts instead of
    // jumping four stops on the frame the meter changed. Auto only -- a manual exposure is the author's.
    // Clamped again so minEv100/maxEv100 keep bounding the CAMERA, as they always did.
    float cameraEv = adapted;
    float cameraTargetEv = targetEv;
    float compensation = 0.0f;
    if (autoExposure != 0u)
    {
        cameraEv = clamp(adapted - ExposureCurve(adapted + compensationEv), minEv100, maxEv100);
        cameraTargetEv = clamp(targetEv - ExposureCurve(targetEv + compensationEv), minEv100, maxEv100);
        // Everything between the scene the eye is adapted to (adapted + compensationEv) and the
        // camera, as the camera finally applies it -- clamps included.
        compensation = adapted + compensationEv - cameraEv;
    }

    StoreExposure(cameraEv, cameraTargetEv, adapted, compensation);
    ExposureValue.Store(4, asuint(exp2(loLogLum)));
    ExposureValue.Store(8, asuint(exp2(hiLogLum)));
}
