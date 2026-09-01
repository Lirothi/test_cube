// SMRT -- Shadow Map Ray Tracing for the directional clipmap.
// Transcribed from Unreal 5.6: VirtualShadowMapSMRTTemplate.ush (the march),
// VirtualShadowMapSMRTCommon.ush (the sample/result structs) and
// VirtualShadowMapProjectionDirectional.ush (the ray setup).
//
// WHY: our clipmap is sampled with ONE SampleCmp per level, and self-shadowing is suppressed by a
// constant NDC depth bias. UE have no such constant. They march a ray from the receiver toward the
// light and derive the only "bias" there is from the march itself -- a tolerance proportional to
// the ray's own depth step. A ray that STARTS at the receiver cannot mistake the receiver for its
// own occluder, which is the entire job the constant was doing.
//
// ============================ THE ONE THING THAT DOES NOT TRANSCRIBE ============================
// UE run REVERSE-Z in their shadow maps: depth 1 = at the light, 0 = far. We run DIRECT Z: the
// atlas/pool is cleared to 1.0 = far = lit, and the depth pass uses LESS_EQUAL (see
// shadow_depth_common.hlsli). So "the sampled surface is nearer the light than the ray point",
// which is what every comparison in the march actually asks, is
//      UE:   sampleDepth  >  referenceDepth
//      here: sampleDepth  <  referenceDepth
// Three comparisons carry that flip and they are marked FLIP below. Copying them verbatim gives a
// march that reports a hit exactly where there is none -- it does not "look slightly wrong", it
// inverts the shadow.
// ===============================================================================================
#ifndef VSM_SMRT_HLSLI
#define VSM_SMRT_HLSLI
#include "vsm_addressing.hlsli"

// HARD CAPS, and they are a SAFETY mechanism, not tuning.
//
// Both loop counts below arrive from a constant buffer. A GPU loop whose bound is unvalidated CB
// data is a hang generator: if the CB field ever fails to resolve (a renamed field, a stale
// reflection, a mismatched mirror) the slot holds whatever was in that memory, an enormous uint
// reads as billions of iterations per pixel, and the lighting dispatch takes the device down with
// it -- with no bad pointer and nothing for the debug layer to complain about. "Off by default"
// does not protect against this, because the garbage IS the default when the write never lands.
//
// So the loops are written against these literals and the CB value can only ever cut them SHORT.
// Whatever the constant buffer holds, the shader is bounded by a number the compiler can see.
static const uint VSM_SMRT_MAX_RAYS            = 16u; // > UE's RayCountDirectional default of 7
static const uint VSM_SMRT_MAX_SAMPLES_PER_RAY = 32u; // > UE's SamplesPerRayDirectional default of 8

// Mirrors UE's FSMRTTraceSettings (the directional half). rayCount 0 = SMRT off, and the caller
// keeps the single-tap SampleCmp path -- the A/B arm and the fallback, exactly as csm.filterMode:0
// stayed for the CSM tent kernels.
struct VsmSmrtParams
{
    uint  rayCount;            // r.Shadow.Virtual.SMRT.RayCountDirectional (UE default 7)
    uint  samplesPerRay;       // r.Shadow.Virtual.SMRT.SamplesPerRayDirectional (UE default 8)
    float rayLengthScale;      // r.Shadow.Virtual.SMRT.RayLengthScaleDirectional (UE default 1.5)
    // r.Shadow.Virtual.SMRT.ExtrapolateMaxSlopeDirectional (UE default 5.0). 0 disables slope
    // extrapolation, which is UE's own documented way to switch it off ("Setting to 0 will disable
    // slope extrapolation") -- with the clamp at 0 the extrapolated depth collapses to depthHistory,
    // which is bit-for-bit their SMRT_EXTRAPOLATE_SLOPE=0 branch. No separate define needed.
    float extrapolateMaxSlope;
    // Sin of the light's angular RADIUS. UE's directional default SourceAngle is 0.5357 degrees,
    // i.e. a radius of 0.268 deg -> 0.00468. 0 collapses every ray onto the light axis, which makes
    // rayCount pure cost -- the state Step 1 and 2 shipped in.
    float sourceRadius;
    // r.Shadow.Virtual.SMRT.TexelDitherScaleDirectional (UE default 2.0), in TEXELS of the level
    // being sampled. Hides the resolution staircase; too high leaks light near contacts.
    float texelDitherScale;
    // Fraction of a clipmap level's square within which a receiver is accepted. 1.0 = the finest
    // level that contains it at all (the single-tap rule); 0.5 = UE's arrangement, where the level
    // covers twice the radius its selection tests. A margin costs a LEVEL OF SHADOW RESOLUTION,
    // which is why this is a knob and not a constant.
    float levelMargin;
};

VsmSmrtParams VsmSmrtParamsOff()
{
    VsmSmrtParams p;
    p.rayCount = 0u;
    p.samplesPerRay = 0u;
    p.rayLengthScale = 0.0f;
    p.extrapolateMaxSlope = 0.0f;
    p.sourceRadius = 0.0f;
    p.texelDitherScale = 0.0f;
    p.levelMargin = 1.0f;
    return p;
}

// ---- Step 3: many rays ----------------------------------------------------------------------
// One ray answers "is the receiver occluded along the light's centre axis" and nothing more. A
// PENUMBRA is the fraction of the light's DISC that is occluded, so it needs rays spread over that
// disc -- which is why UE ship RayCountDirectional at 7 and why a single ray is only ever a
// hard-shadow test dressed up as a march.

// UE's R2 low-discrepancy sequence (VirtualShadowMapSMRTCommon.ush), used to decorrelate the rays
// of one pixel from each other.
float2 VsmSmrtR2(uint n)
{
    return frac((float)n * float2(0.754877669f, 0.569840296f));
}

// Per-pixel decorrelation. UE read blue noise here; this is a plain hash of the receiver's world
// position, which has one property blue noise does not: it is stable in WORLD space, so the dither
// does not crawl across a surface as the camera moves. The cost is a slightly worse spectrum.
float2 VsmSmrtHash2(float3 p)
{
    const float3 q = floor(p * 512.0f);
    const float h = dot(q, float3(127.1f, 311.7f, 74.7f));
    return frac(sin(float2(h, h + 1.0f)) * 43758.5453f);
}

// UE's UniformSampleDiskConcentric: maps the unit square onto the unit disc preserving area
// without the clumping a naive polar mapping produces.
float2 VsmSmrtDiskConcentric(float2 e)
{
    const float2 o = 2.0f * e - 1.0f;
    if (all(o == 0.0f)) { return float2(0.0f, 0.0f); }
    float r, theta;
    if (abs(o.x) > abs(o.y)) { r = o.x; theta = (3.14159265f / 4.0f) * (o.y / o.x); }
    else                     { r = o.y; theta = (3.14159265f / 2.0f) - (3.14159265f / 4.0f) * (o.x / o.y); }
    return r * float2(cos(theta), sin(theta));
}

// UE's GetRandomDirectionalLightRayDir: jitter the direction within the light's angular disc.
// `sourceRadius` is sin of the light's angular RADIUS -- for the real sun about 0.00465.
float3 VsmSmrtRayDir(float3 dirToLight, float2 e, float sourceRadius)
{
    if (sourceRadius <= 0.0f) { return dirToLight; }
    const float2 disk = VsmSmrtDiskConcentric(e) * sourceRadius;
    const float3 n = dirToLight;
    const float3 dPdu = cross(n, (abs(n.x) > 1e-6f) ? float3(1, 0, 0) : float3(0, 1, 0));
    const float3 dPdv = cross(dPdu, n);
    return normalize(n + dPdu * disk.x + dPdv * disk.y);
}

// UE's FSMRTSample.
struct VsmSmrtSample
{
    bool  valid;
    float sampleDepth;      // what the shadow map stores at this step
    float referenceDepth;   // where the ray is at this step. Always set, valid or not (UE's note).
    float extrapolateSlope;
};

// UE's FSMRTResult.
struct VsmSmrtResult
{
    bool  validHit;
    float hitDepth;
};

// UE's FSMRTClipmapRayState. It is a STATE rather than plain parameters for exactly one reason:
// a ray that walks out of the clipmap level it started in must CONTINUE IN THE COARSER ONE, and
// the level it ends up in has to persist across the march.
//
// HOW OURS DIFFERS FROM UE'S, AND WHY. UE keep one depth space for the whole march: they sample the
// coarser level and convert the depth back into the starting level's range with a scale/bias pair
// (`Result.Depth = (physDepth - DepthLevelBias) / DepthLevelScale`, SampleVirtualShadowMapClipmap).
// That works because their level-to-level relation is a clean power of two in BOTH the UV scale and
// the depth range.
//
// Here each clipmap level is a separate view with its OWN camera-centred, texel-snapped matrix, so
// the ray is RE-PROJECTED into the new level instead: recompute this sample point through that
// level's `clipVP` and carry on. It costs one extra matrix multiply on a level change (rare -- only
// at a page edge) and it does not depend on the two levels' depth ranges staying in an exact ratio,
// which is an invariant our matrices are built to satisfy but nothing in the shader could check.
//
// The price is that the depth HISTORY is in the old level's units the moment we cross, so it must
// be dropped -- which is what UE's `bResetExtrapolation` flag exists for, used here for the whole
// history rather than just the slope.
struct VsmSmrtRayState
{
    uint   level;             // clipmap LEVEL index; the view slot is VSM_NUM_LOCAL_VIEWS + level
    float3 originWS;          // the ray's start, in world space -- kept so it can be re-projected
    float3 vecWS;             // full ray vector (direction * length), likewise
    float  rayLength;
    float  extrapolateMaxSlopeWS; // world-unit clamp; converted to this level's NDC in SetLevel
    // Cached projection of the ray into the CURRENT level. Rebuilt by VsmSmrtSetLevel.
    float3 startUVZ;
    float3 stepUVZ;
    float  extrapolateSlope;  // the above, in this level's NDC per unit ray parameter
};

// (Re)project the ray into `level` and cache the per-level terms.
void VsmSmrtSetLevel(inout VsmSmrtRayState st, uint level,
                     float4x4 clipVP[VSM_NUM_CLIPMAP_LEVELS])
{
    st.level = level;
    const float4 clip = mul(float4(st.originWS, 1.0f), clipVP[level]);
    const float3 ndc = clip.xyz / max(clip.w, 1e-8f);
    // Direction, not position: an ortho clipmap matrix has a zero w column, so this is the pure
    // linear part -- no perspective divide and no translation.
    const float3 ndcStep = mul(float4(st.vecWS, 0.0f), clipVP[level]).xyz;

    // UV is y-flipped against NDC, so the step must be too.
    st.startUVZ = float3(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y, ndc.z);
    st.stepUVZ  = float3(0.5f * ndcStep.x, -0.5f * ndcStep.y, ndcStep.z);

    // UE clamp the extrapolated slope in world units and convert it with their projection's z
    // scale "so that it doesn't change based on ZRangeScale". The same scale here is this level's
    // NDC-per-world-unit along the light, which the ray step already carries -- taking it from the
    // matrix means it cannot drift out of step with g_clipmapZRangeScale the way a transcribed
    // constant would, and it is re-derived correctly for whatever level the ray ends up in.
    st.extrapolateSlope = st.extrapolateMaxSlopeWS * abs(ndcStep.z) / max(st.rayLength, 1e-6f);
}

// Point-read of the clipmap's stored depth at a shadow UV.
//
// NOT SampleCmp, and not even a filtered sample: the march wants the DEPTH, and it wants it at one
// texel. Using Load also means SMRT adds no sampler and no descriptor to any table -- root
// signature and descriptor-table layout are untouched, which is worth a great deal given how
// positional those tables are.
bool VsmSmrtLoadDepth(uint view, float2 uv, StructuredBuffer<uint> PageTable, Texture2D Pool,
                      out float depth, out uint lodOffset)
{
    depth = 1.0f;
    lodOffset = 0u;
    if (any(uv < 0.0f) || any(uv > 1.0f)) { return false; } // ray left this level

    // Clipmap views live at page level 0 (the clipmap LEVEL is the LOD), same as VsmSampleNDC's
    // distCam=0 start.
    const uint axis = VSM_L0_AXIS;
    const uint px = min((uint)(uv.x * (float)axis), axis - 1u);
    const uint py = min((uint)(uv.y * (float)axis), axis - 1u);
    const uint entry = PageTable[VsmPageId(view, 0u, px, py)];
    // MAPPED HERE, deliberately -- not merely resolvable. A fallback entry's physical page belongs
    // to a COARSER level, which covers 2^k times the area, so the sub-page position computed below
    // from THIS level's grid would be wrong for it. The fallback is honoured by re-projecting into
    // the level that owns the page (VsmSmrtFindSample), which yields the right UV by construction;
    // `lodOffset` is returned so the caller can jump straight there instead of searching.
    lodOffset = (entry & VSM_LOD_MASK) >> VSM_LOD_SHIFT;
    if ((entry & VSM_RESIDENT_BIT_C) == 0u) { return false; }

    const uint phys = entry & VSM_PHYS_MASK_C;
    const float gx = (float)(phys % VSM_POOL_PAGES_AXIS);
    const float gy = (float)(phys / VSM_POOL_PAGES_AXIS);
    const float2 uvInPage = saturate(uv * (float)axis - float2(px, py));
    // Clamped INSIDE the page, so a read can never wander into the neighbouring page's physical
    // texels -- the same guarantee the PCF path gets from its pmin/pmax clamp.
    const float2 inPageTexel = clamp(uvInPage * (float)VSM_PAGE_SIZE,
                                     0.0f, (float)VSM_PAGE_SIZE - 1.0f);
    const int2 texel = int2(float2(gx, gy) * (float)VSM_PAGE_SIZE + inPageTexel);
    depth = Pool.Load(int3(texel, 0)).r;
    return true;
}

// UE's SMRTFindSample, plus the level-crossing search their SampleVirtualShadowMapClipmap does
// internally. `resetExtrapolation` is UE's FSMRTSample.bResetExtrapolation.
VsmSmrtSample VsmSmrtFindSample(inout VsmSmrtRayState st, float sampleTime,
                                float4x4 clipVP[VSM_NUM_CLIPMAP_LEVELS],
                                StructuredBuffer<uint> PageTable, Texture2D Pool,
                                out bool resetExtrapolation)
{
    resetExtrapolation = false;
    const float3 sampleUVZ = st.startUVZ + st.stepUVZ * sampleTime;

    VsmSmrtSample s;
    s.valid = false;
    s.sampleDepth = 0.0f;
    s.referenceDepth = sampleUVZ.z;   // set even when invalid -- UE call this out explicitly
    s.extrapolateSlope = st.extrapolateSlope;

    float d = 1.0f;
    uint hintOffset = 0u;
    if (VsmSmrtLoadDepth(VSM_NUM_LOCAL_VIEWS + st.level, sampleUVZ.xy, PageTable, Pool, d,
                         hintOffset))
    {
        s.valid = true;
        s.sampleDepth = d;
        return s;
    }

    // The chain's fast path: the page exists in this level but is not mapped, and the propagate
    // pass already worked out which coarser level owns it. Start the walk there instead of at
    // level+1. When the ray left the level entirely there is no entry to consult and hintOffset is
    // 0, which starts the walk at the next level -- the general case, and the one our ray hits
    // most, because our clipmap level for a receiver is the TIGHTEST square containing it.
    const uint firstStep = max(hintOffset, 1u);

    // LEVEL CROSSING. The ray walked off this level's UV range, or onto a page that is not
    // resident. Rather than dropping the sample -- which is what makes a long ray silently stop
    // finding occluders near the level edge -- continue in a COARSER level, which covers the same
    // world position and, being coarser, is far more likely to be resident.
    //
    // Bounded by a compile-time constant, and only ever walks OUTWARD: a finer level cannot contain
    // a point this one does not.
    [loop] for (uint L = st.level + firstStep; L < VSM_NUM_CLIPMAP_LEVELS; ++L)
    {
        const float3 posWS = st.originWS + st.vecWS * sampleTime;
        const float4 clip = mul(float4(posWS, 1.0f), clipVP[L]);
        if (clip.w <= 0.0f) { continue; }
        const float3 ndc = clip.xyz / clip.w;
        if (any(abs(ndc.xy) > 1.0f)) { continue; }
        const float2 uv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);

        float dl = 1.0f;
        uint dummyOffset = 0u;
        if (!VsmSmrtLoadDepth(VSM_NUM_LOCAL_VIEWS + L, uv, PageTable, Pool, dl, dummyOffset))
        {
            continue;
        }

        // Adopt this level for the REST of the march, not just this sample: the ray is leaving the
        // finer level for good, and re-testing it every step would cost a projection per sample to
        // rediscover the same answer.
        VsmSmrtSetLevel(st, L, clipVP);
        resetExtrapolation = true;
        s.valid = true;
        s.sampleDepth = dl;
        s.referenceDepth = ndc.z;              // this level's Z space, matching sampleDepth
        s.extrapolateSlope = st.extrapolateSlope;
        return s;
    }

    // Nothing resident at any level: invalid, and the march skips it. referenceDepth stays in the
    // old level's space, which is harmless because an invalid sample updates no history.
    return s;
}

// UE's SMRTRayCast, with the three direct-Z flips marked.
//
// The march walks from the LIGHT back to the receiver: sampleTime runs from ~1 down to 0, and 0 is
// the receiver itself (UE force the last step to exactly 0). The squaring of the parameter packs
// the samples toward the receiver, which is where contact detail lives.
VsmSmrtResult VsmSmrtRayCast(inout VsmSmrtRayState st, int numSteps, float stepOffset,
                             float4x4 clipVP[VSM_NUM_CLIPMAP_LEVELS],
                             StructuredBuffer<uint> PageTable, Texture2D Pool)
{
    // UE's magic initializer. Cannot be a flag: they need the register to disappear under DCE, and
    // depthHistory can legitimately go negative (to about -2) when a ray crosses a clipmap boundary
    // and the depth range expands.
    // Clamped BEFORE anything uses it: the lower bound stops a 1/0 in timeScale (a zero from the
    // CB would make every sampleTime NaN and the march meaningless rather than merely wrong), the
    // upper bound is the hang guard described at the top of this file.
    const uint steps = clamp((uint)max(numSteps, 1), 1u, VSM_SMRT_MAX_SAMPLES_PER_RAY);

    const float kDepthHistoryNotSet = -10000.0f;
    float depthHistory = kDepthHistoryNotSet;
    float depthHistoryTime = -1.0f;
    float depthSlope = 0.0f;

    const float timeScale = -1.0f / (float)steps;
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

        bool resetExtrapolation = false;
        const VsmSmrtSample s = VsmSmrtFindSample(st, sampleTime, clipVP, PageTable, Pool,
                                                  resetExtrapolation);
        const float referenceDepth = s.referenceDepth;

        // The ray changed clipmap level, so every accumulated quantity is expressed in a depth
        // space that no longer exists. UE only reset the SLOPE here, because their level fallback
        // converts the sampled depth back into the starting level's range and their history stays
        // valid; ours re-projects instead (see VsmSmrtRayState), so the history has to go too.
        // Dropping prevReferenceDepth matters most: leaving it would compute the next step's
        // CompareTolerance across the discontinuity, producing one enormous tolerance and a
        // spurious hit right at the level boundary.
        if (resetExtrapolation)
        {
            depthHistory = kDepthHistoryNotSet;
            depthHistoryTime = -1.0f;
            depthSlope = 0.0f;
            prevReferenceDepth = -1.0f;
        }

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

#endif // VSM_SMRT_HLSLI
