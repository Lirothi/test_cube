// Shore foam dissipation — variant A of docs/ocean_shore_foam_breakup_plan.md.
//
// Depth-driven contact foam draws a constant-width band along every isobath: on a flat beach at
// low wind the shoreline reads as a static outlined rim. This layer breaks the band into patches
// that decay and regrow IN PLACE: two low-frequency samples of the breakup texture drift along
// two FIXED world directions (not wind — the shape must not collapse at dead calm), and because
// the drifts oppose each other their sum never reads as a current, the field just breathes.
// The caller multiplies the coverage DEPTH THRESHOLD by the factor, not the coverage itself:
// patches thin out and geometrically vanish instead of alpha-fading.
//
// Self-contained on purpose: the caller passes the texture and every parameter, so this include
// has no dependency on the legacy cbuffer layout and can be reused by the modern surface later.
//
// params: x = patch scale (metres), y = drift speed (m/s), z = amount (0 = OFF, exact identity),
//         w = contrast (steepness of the patch remap around mid-grey; the averaged samples
//             cluster near 0.5, so > 1 is needed for patches to reach full kill/keep).
float ShoreFoamDissipationFactor(
    Texture2D noiseTex, SamplerState wrapSampler, float2 worldPos, float time, float4 params)
{
    [branch]
    if (params.z <= 0.0f)
    {
        return 1.0f;
    }

    float invScale = 1.0f / max(params.x, 1.0f);
    float drift = time * params.y;
    const float2 kDirA = float2(0.8f, 0.6f);
    const float2 kDirB = float2(-0.6f, 0.8f);
    // Slightly different scale and speed for the second layer so the beat between them stays
    // aperiodic; the constant offset decorrelates it from the first layer's texels.
    float n1 = noiseTex.SampleLevel(wrapSampler, (worldPos + kDirA * drift) * invScale, 0).r;
    float n2 = noiseTex.SampleLevel(
        wrapSampler, (worldPos + kDirB * (drift * 0.73f)) * (invScale * 0.61f) + 11.17f, 0).r;

    float field = saturate(((n1 + n2) * 0.5f - 0.5f) * max(params.w, 0.01f) + 0.5f);
    return lerp(1.0f, field, saturate(params.z));
}

// Variant C helper: 0..1 wind amount from the shared shoreFoamWindParams contract
// (x: wind force 0..1, y: calm threshold - no contact foam below, z: full-strength threshold).
// A parameterized copy of the modern surface's ContactFoamWindAmount: LINEAR between the two
// thresholds, not smoothstep, so the slider reads as "how much foam".
float ShoreFoamWindAmount(float4 windParams)
{
    float windForce = saturate(windParams.x);
    float calmWind = saturate(windParams.y);
    float fullWind = max(windParams.z, calmWind + 1e-3f);
    return saturate((windForce - calmWind) / (fullWind - calmWind));
}

// NOTE: variant B (surf phase - a crest running shoreward over the depth field) was implemented
// twice and REJECTED both times: any phase that is a function of water depth draws concentric
// isobath rings, fires next to vertical walls (depth sweeps through every value in a pixel or
// two), and a solid travelling crest fundamentally needs the modern stack's geometry (SDF anchor
// + material vertex travel), not a pixel threshold. The classic mode stays A + C.

// The combined threshold factor the caller multiplies the tail's depth threshold by:
// variant A (dissipation patches) x variant C (wind thinning: at dead calm the band starves
// toward zero, full wind is untouched).
float ShoreFoamBreakupThresholdFactor(
    Texture2D noiseTex, SamplerState wrapSampler, float2 worldPos, float time,
    float windAmount, float windThinning, float4 dissipationParams)
{
    float factor =
        ShoreFoamDissipationFactor(noiseTex, wrapSampler, worldPos, time, dissipationParams);
    factor *= lerp(1.0f, saturate(windAmount), saturate(windThinning));
    return factor;
}
