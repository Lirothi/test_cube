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
    // The clipmap's ResolutionLodBias (vsm::g_clipmapFrameBias), for the dither's scale.
    float clipResBias;
    // Rotates the per-pixel sample set once per frame (UE's View.StateFrameIndex). 0 = off, which
    // is what a single-frame comparison wants; non-zero pays off only through a temporal pass.
    uint frameIndex;
    // r.Shadow.Virtual.SMRT.AdaptiveRayCount (UE default 1). 0 = always shoot the full count.
    // Only honoured where wave ops mean what UE assume -- see VSM_SMRT_COMPUTE at the use site.
    uint adaptiveRayCount;
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
    p.clipResBias = 0.0f;
    p.frameIndex = 0u;
    p.adaptiveRayCount = 0u;
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

// UE's UniformSampleDiskConcentric, via ConcentricDiskSamplingHelper (MonteCarlo.ush) --
// transcribed rather than re-derived, because the details are load-bearing:
//   * the input is rescaled by 0.99999994, not 1, so the radius stays in [0,1);
//   * the 2^-64 epsilon avoids 0/0 at the origin WITHOUT bending the rest of the mapping;
//   * the sign bits are copied from `p` onto the circle point, which is what puts the sample in
//     the right quadrant. My first cut instead carried a SIGNED radius, which lands every sample
//     from a negative quadrant on its antipode -- still a uniform disc, but no longer the mapping
//     the quasirandom sequence was stratified for.
float2 VsmSmrtDiskConcentric(float2 e)
{
    const float2 p = 2.0f * e - 0.99999994f;
    const float2 a = abs(p);
    const float lo = min(a.x, a.y);
    const float hi = max(a.x, a.y);
    const float eps = 5.42101086243e-20f; // 2^-64
    const float phi = (3.14159265f / 4.0f) * (lo / (hi + eps) + 2.0f * (float)(a.y >= a.x));
    const uint signMask = 0x80000000u;
    const float2 disk = asfloat((asuint(float2(cos(phi), sin(phi))) & ~signMask) | (asuint(p) & signMask));
    return disk * hi; // uniform sampling: point on the circle, scaled by the radius
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

// UE's FSMRTClipmapRayState: the ray stays in the level its receiver landed on for the whole march, and a sample whose page
// is not mapped there is read from the coarser level that holds it, its depth brought back into this level's range
// (SampleVirtualShadowMapClipmap's DepthLevelScale / Bias). So the samples next to the receiver read the receiver's own level
// however far up the march went.
struct VsmSmrtRayState
{
    uint   level;             // clipmap LEVEL index; the view slot is VSM_CLIPMAP_FIRST + level
    float3 originWS;          // the ray's start, in world space -- a fallback sample is projected into its own level
    float3 vecWS;             // full ray vector (direction * length)
    float  rayLength;
    float  extrapolateMaxSlopeWS; // world-unit clamp; converted to this level's NDC in SetLevel
    // The projection of the ray into its level.
    float3 startUVZ;
    float3 stepUVZ;
    float  extrapolateSlope;  // the above, in this level's NDC per unit ray parameter
};

// Project the ray into `level`.
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

    // Clipmap views have no mips: the clipmap LEVEL is the LOD.
    const uint axis = VSM_CLIPMAP_AXIS;
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
    const float gx = (float)(phys % VSM_POOL_PAGES_X);
    const float gy = (float)(phys / VSM_POOL_PAGES_X);
    const float2 uvInPage = saturate(uv * (float)axis - float2(px, py));
    // Clamped INSIDE the page, so a read can never wander into the neighbouring page's physical
    // texels -- the same guarantee the PCF path gets from its pmin/pmax clamp.
    const float2 inPageTexel = clamp(uvInPage * (float)VSM_PAGE_SIZE,
                                     0.0f, (float)VSM_PAGE_SIZE - 1.0f);
    const int2 texel = int2(float2(gx, gy) * (float)VSM_PAGE_SIZE + inPageTexel);
    depth = Pool.Load(int3(texel, 0)).r;
    return true;
}

// UE's SMRTFindSample, plus the fallback their SampleVirtualShadowMapClipmap does internally.
VsmSmrtSample VsmSmrtFindSample(inout VsmSmrtRayState st, float sampleTime,
                                float4x4 clipVP[VSM_NUM_CLIPMAP_LEVELS],
                                StructuredBuffer<uint> PageTable, Texture2D Pool)
{
    const float3 sampleUVZ = st.startUVZ + st.stepUVZ * sampleTime;

    VsmSmrtSample s;
    s.valid = false;
    s.sampleDepth = 0.0f;
    s.referenceDepth = sampleUVZ.z;   // set even when invalid -- UE call this out explicitly
    s.extrapolateSlope = st.extrapolateSlope;

    float d = 1.0f;
    uint hintOffset = 0u;
    if (VsmSmrtLoadDepth(VSM_CLIPMAP_FIRST + st.level, sampleUVZ.xy, PageTable, Pool, d,
                         hintOffset))
    {
        s.valid = true;
        s.sampleDepth = d;
        return s;
    }

    // The chain's fast path: the page exists in this level but is not mapped, and the propagate
    // pass already worked out which coarser level owns it. When the ray left the level there is no
    // entry to consult and hintOffset is 0, which starts the walk at the next level.
    const uint firstStep = max(hintOffset, 1u);

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
        if (!VsmSmrtLoadDepth(VSM_CLIPMAP_FIRST + L, uv, PageTable, Pool, dl, dummyOffset))
        {
            continue;
        }

        // UE's DepthLevelScale / Bias: every level's depth is z = a_L dot(P, light) + b_L with a_L halving per level (its
        // range is proportional to its radius), so z in this ray's level is (z_L - b_L) 2^(L - level) + b_level.
        s.valid = true;
        s.sampleDepth = (dl - clipVP[L]._43) * exp2((float)L - (float)st.level) + clipVP[st.level]._43;
        return s;
    }

    // Nothing resident at any level: invalid, and the march skips it.
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

        const VsmSmrtSample s = VsmSmrtFindSample(st, sampleTime, clipVP, PageTable, Pool);
        const float referenceDepth = s.referenceDepth;

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
