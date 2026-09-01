// CONTACT SHADOWS -- transcribed from Unreal's ScreenSpaceShadowRayCast.ush
// (`CastScreenSpaceShadowRay`) and its use in DeferredLightingCommon.ush (`ShadowRayCast` /
// `GetShadowTermsBase`).
//
// WHAT IT IS FOR. A shadow map has a texel size, and below that size it has nothing to say. Far
// cascades are where this bites: a cascade covering hundreds of metres cannot resolve a blade of
// grass touching the ground, so distant geometry floats. This marches the CAMERA DEPTH BUFFER for
// a short distance toward the light and recovers exactly that scale -- and because the trace length
// is scaled by scene depth, it covers the same number of SCREEN pixels near and far, which is what
// makes it keep working at distance instead of shrinking to nothing.
//
// SHADOW-MODE INDEPENDENT by construction: it reads the camera depth buffer, not any shadow map, so
// the same term serves Legacy CSM and VSM alike.
//
// DISTINCT FROM vsm_screen_ray.hlsli, which is a different UE function for a different job: that
// one returns where the SMRT ray should START (a bias), this one returns an occluder DISTANCE (a
// shadow). Both march screen depth; do not merge them.
//
// NO DEPTH FLIPS HERE. Like the VSM screen ray, this reads the CAMERA depth buffer, and ours is
// already reverse-Z as UE's is (cleared to 0.0; Material.h's default graphics DepthFunc is
// GREATER_EQUAL). Only the shadow maps run direct Z. So the comparisons below are literal.
#ifndef CONTACT_SHADOW_HLSLI
#define CONTACT_SHADOW_HLSLI
#include "utils.hlsli" // ReconstructPosWS, for the world-space thickness reject

static const uint CONTACT_SHADOW_MAX_STEPS = 32u;

// Returns the distance along the ray at which the first hit occurred, or NEGATIVE on a miss.
//
// `worldToClip` is the camera view-projection; `viewToClip` is needed for UE's compare tolerance,
// which is built from how much DEVICE depth the ray spans -- a tolerance derived from the march
// itself rather than a tuned constant, the same principle SMRT uses.
// THICKNESS. UE's window is `abs(depthDiff + tol) < tol` with `tol` derived from the ray's own
// DEVICE-depth step, and that is the whole test -- there is no world-space limit on how far behind
// the surface a "hit" may be. Close up that costs nothing, because a ray a few centimetres long
// cannot get far behind anything. At range it is the whole problem: the length is a multiple of
// view depth (which is right -- it keeps the ray a constant number of PIXELS), so at 350 m the ray
// is 17.5 metres of world, and "behind the surface" starts meaning the far side of a dune. That is
// not a contact, and reported per pixel as a binary hit it reads as a speckle field.
//
// `maxThicknessFrac` rejects exactly that: a hit whose occluder is further behind the ray point
// than this FRACTION OF THE RAY LENGTH is not a contact. 0 disables the check (UE behaviour).
//
// A FRACTION, not metres, and the first version got that wrong too. The ray length is itself a
// multiple of view depth, so it grows with distance; a thickness fixed in metres cannot be right
// at 10 m and at 3 km at once -- it is either useless near or destroys every real contact far.
// Tied to the ray, it scales with it automatically and means something stable: "an occluder more
// than half a ray-length behind the ray point is not what this effect is for".
float CastScreenSpaceShadowRay(float3 originWS, float3 rayDir, float rayLength, uint numSteps,
                               float dither, float compareToleranceScale, float maxThicknessFrac,
                               float3 camPosWS,
                               float4x4 worldToClip, float4x4 viewToClip,
                               float4x4 invProj, float4x4 invView,
                               Texture2D depthTex, SamplerState pointSampler)
{
    if (rayLength <= 0.0f || numSteps == 0u) { return -1.0f; }

    const float4 rayStartClip = mul(float4(originWS, 1.0f), worldToClip);
    const float4 rayDirClip   = mul(float4(rayDir * rayLength, 0.0f), worldToClip);
    const float4 rayEndClip   = rayStartClip + rayDirClip;
    if (rayStartClip.w <= 0.0f || rayEndClip.w <= 0.0f) { return -1.0f; }

    const float3 rayStart = rayStartClip.xyz / rayStartClip.w;
    const float3 rayEnd   = rayEndClip.xyz / rayEndClip.w;
    const float3 rayStep  = rayEnd - rayStart;

    // How much DEVICE depth a ray of this length spans if it went straight away from the camera.
    // UE build the compare tolerance from this, so the tolerance follows the depth buffer's own
    // non-linearity instead of being a world-space guess.
    const float4 rayDepthClip = rayStartClip + mul(float4(0.0f, 0.0f, rayLength, 0.0f), viewToClip);
    if (rayDepthClip.w <= 0.0f) { return -1.0f; }
    const float3 rayDepth = rayDepthClip.xyz / rayDepthClip.w;

    const uint n = min(numSteps, CONTACT_SHADOW_MAX_STEPS);
    const float stepT = 1.0f / (float)n;
    const float stepOffset = dither - 0.5f;

    // UE's comment on the x2: "to get less moire pattern in extreme cases, larger values make
    // object appear not grounded in reflections".
    const float compareTolerance = abs(rayDepth.z - rayStart.z) * stepT * compareToleranceScale;

    float sampleTime = stepOffset * stepT + stepT;

    // NDC -> UV, with the y flip. UE fold this into View.ScreenPositionScaleBias.
    const float2 startUV = float2(rayStart.x * 0.5f + 0.5f, 0.5f - rayStart.y * 0.5f);
    // The receiver's own depth, to skip self-intersection. The exact comparison is only sound
    // because the depth is POINT sampled -- a filtered tap would never compare equal.
    const float startDepth = depthTex.SampleLevel(pointSampler, startUV, 0).r;

    // Bounded by a literal; `n` can only end it early (see vsm_smrt.hlsli on CB-driven bounds).
    [loop] for (uint i = 0u; i < CONTACT_SHADOW_MAX_STEPS; ++i)
    {
        if (i >= n) { break; }

        const float3 samplePos = rayStart + rayStep * sampleTime; // NDC
        const float2 sampleUV = float2(samplePos.x * 0.5f + 0.5f, 0.5f - samplePos.y * 0.5f);
        const float sampleDepth = depthTex.SampleLevel(pointSampler, sampleUV, 0).r;

        if (sampleDepth != startDepth)
        {
            const float depthDiff = samplePos.z - sampleDepth;
            // Same windowed test SMRT uses: accepts depthDiff in (-2*tolerance, 0), i.e. the ray
            // has passed just behind the stored surface. A plain `<` would report every pixel
            // farther than the surface, including ones that are behind it by a mile.
            const bool hit = abs(depthDiff + compareTolerance) < compareTolerance;
            if (hit && maxThicknessFrac > 0.0f)
            {
                // Reconstruct what the depth buffer actually holds here and measure, in METRES,
                // how far behind the ray point sits.
                //
                // ALONG THE VIEW AXIS, and the first version got this wrong: it projected onto the
                // LIGHT direction, because that is the direction the ray travels. But "behind the
                // surface" is a statement about the DEPTH BUFFER -- it means farther from the
                // CAMERA. Under a low sun those two axes are nearly perpendicular, so the
                // projection came out near zero and the test almost never fired: the knob moved
                // 0.7 % of pixels against a 4 pp artifact, i.e. it did nothing.
                const float3 surfaceWS = ReconstructPosWS(sampleUV, sampleDepth, invProj, invView);
                const float3 rayPosWS = originWS + rayDir * (rayLength * sampleTime);
                const float behind = distance(camPosWS, rayPosWS) - distance(camPosWS, surfaceWS);
                if (behind > maxThicknessFrac * rayLength) { sampleTime += stepT; continue; }
            }
            if (hit)
            {
                // Off-screen masking against the NDC boundary: a hit found outside the view is not
                // information, it is the edge of the depth buffer.
                const bool validPos = all(samplePos.xy > -1.0f) && all(samplePos.xy < 1.0f);
                return validPos ? (rayLength * sampleTime) : -1.0f;
            }
        }
        sampleTime += stepT;
    }

    return -1.0f;
}

// ---- The whole effect, shared by every light pass -------------------------------------------
// One function, three callers (sun, spot, point). UE do the same: GetShadowTermsBase runs for
// every light type, with `ContactShadowLength` living on the light and `L` being whatever points
// at THAT light -- a constant for the sun, `normalize(lightPos - P)` for a local light. Nothing
// else differs, so nothing else is duplicated.
struct ContactShadowParams
{
    float length;          // metres, or a multiple of view depth (see lengthInWS)
    float intensity;
    uint  steps;
    uint  lengthInWS;
    float normalOffset;    // fraction of the ray length
    float grazingFade;     // NdotL below which the term fades out
    float minDist;         // metres from the camera
    float maxDist;         // metres, 0 = no far limit
    float fadeBand;        // metres
    float thickness;       // fraction of the ray length
    uint  frameId;         // StateFrameIndexMod8 + 1, 0 = static dither
};

float ApplyContactShadow(float shadow, float3 P, float3 N, float3 dirToLight, float ndl,
                         uint2 pixel, float3 camPosWS,
                         float4x4 viewProj, float4x4 projMatrix, float4x4 invProj, float4x4 invView,
                         Texture2D depthTex, SamplerState pointSampler, ContactShadowParams cp)
{
    if (cp.length <= 0.0f || cp.intensity <= 0.0f) { return shadow; }
    // Facing away from the light: already fully shadowed by NdotL, and a trace there only ever
    // finds the surface it started on.
    if (ndl <= 0.0f) { return shadow; }

    const float viewDepth = length(P - camPosWS);

    // ---- OURS: distance window (UE have none) ------------------------------------------------
    if (viewDepth < cp.minDist) { return shadow; }
    float distFade = 1.0f;
    if (cp.maxDist > 0.0f)
    {
        if (viewDepth > cp.maxDist) { return shadow; }
        distFade = saturate((cp.maxDist - viewDepth) / max(cp.fadeBand, 0.1f));
    }

    // ---- OURS: grazing guard -----------------------------------------------------------------
    // As NdotL falls the ray runs ever more parallel to the surface it started on and the march
    // measures depth-buffer quantisation instead of geometry. Faded, not cut.
    const float grazeFade = (cp.grazingFade > 0.0f) ? smoothstep(0.0f, cp.grazingFade, ndl) : 1.0f;
    const float weight = distFade * grazeFade;
    if (weight <= 0.001f) { return shadow; }

    // LENGTH. UE support both readings and encode the choice in the SIGN of their value; split
    // into a flag here. Screen scale = a multiple of view depth, which keeps the trace the same
    // size in SCREEN pixels at any distance -- what lets it keep working far away. NOT capped in
    // metres: that was tried and it broke the image (the ray must keep spanning enough DEVICE
    // depth for the compare tolerance to mean anything).
    const float rayLength = (cp.lengthInWS != 0u) ? cp.length : cp.length * viewDepth;

    // ---- OURS: start off the surface, as a fraction of the ray so it scales with distance ----
    const float3 origin = P + N * (cp.normalOffset * rayLength);

    // UE's InterleavedGradientNoise(PixelPos, StateFrameIndexMod8), verbatim. FrameId 0 reduces
    // it to the static IGN. SCREEN-space noise on purpose: the banding it hides is a screen-space
    // artifact of a screen-space march.
    const float frameId = (cp.frameId > 0u) ? (float)(cp.frameId - 1u) : 0.0f;
    const float2 px = (float2)pixel + frameId * (float2(47.0f, 17.0f) * 0.695f);
    const float dither = frac(52.9829189f * frac(0.06711056f * px.x + 0.00583715f * px.y));

    const float hit = CastScreenSpaceShadowRay(origin, dirToLight, rayLength, cp.steps,
                                               dither, 2.0f, // UE's CompareToleranceScale
                                               cp.thickness, camPosWS,
                                               viewProj, projMatrix, invProj, invView,
                                               depthTex, pointSampler);
    if (hit <= 0.0f) { return shadow; } // miss
    return shadow * (1.0f - cp.intensity * weight);
}

#endif // CONTACT_SHADOW_HLSLI
