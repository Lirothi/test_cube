#ifndef SKY_VIEW_MAPPING_HLSLI
#define SKY_VIEW_MAPPING_HLSLI
// UE SkyAtmosphereCommon.ush:37-38,194-225 and SkyAtmosphere.usf:228-267.
// Fixed world-aligned Z-up referential (world x,z,y), no camera yaw dependence.
static const float SkyViewPi = 3.14159265358979323846f;
static const float2 SkyViewSize = float2(192, 104);
float3 SkyViewUvToDir(float2 uv, float height, float bottom)
{
    uv = saturate((uv - 0.5f / SkyViewSize) * SkyViewSize / (SkyViewSize - 1.0f));
    float beta = acos(saturate(sqrt(max(0.0f, height * height - bottom * bottom)) / height));
    float horizon = SkyViewPi - beta;
    float angle = uv.y < 0.5f ? horizon * (1.0f - pow(1.0f - 2.0f * uv.y, 2.0f))
        : horizon + beta * pow(2.0f * uv.y - 1.0f, 2.0f);
    float longitude = uv.x * 2.0f * SkyViewPi;
    return float3(sin(angle) * cos(longitude), sin(angle) * sin(longitude), cos(angle));
}
float2 SkyViewDirToUv(float3 dir, float height, float bottom, bool ground)
{
    float beta = acos(saturate(sqrt(max(0.0f, height * height - bottom * bottom)) / height));
    float horizon = SkyViewPi - beta;
    float angle = acos(clamp(dir.z, -1.0f, 1.0f));
    float2 uv;
    uv.y = ground ? 0.5f + 0.5f * sqrt(saturate((angle - horizon) / max(beta, 1.e-6f)))
        : 0.5f * (1.0f - sqrt(saturate(1.0f - angle / horizon)));
    uv.x = (atan2(-dir.y, -dir.x) + SkyViewPi) / (2.0f * SkyViewPi);
    // Preserve UE's asymmetric unit/sub-UV transforms, including the +1 denominator.
    return (uv + 0.5f / SkyViewSize) * SkyViewSize / (SkyViewSize + 1.0f);
}
// The sky in EVERY direction, with the planet taken out of the picture: below the horizon the ray
// is mirrored back into the upper hemisphere and the LUT's own upper rows answer for it.
//
// Why not use the LUT's lower half, which is what UE do. Their below-horizon rows are honest -- the
// ray stops at the planet (SkyAtmosphere.usf:451-470 bound tMax at the bottom sphere) and integrates
// the few hundred metres of air in front of the ground -- and honest, at 22 m of altitude, means
// BLACK: measured (23,34,53) just under the horizon falling to (0,1,1) further down. UE never show
// that because a UE world has a landscape there. Ours does not always: on a level whose terrain
// ends, or with the water off, the lower hemisphere is what fills the bottom of the frame.
//
// Clamping to the horizon row was the first answer and it is worse than it looks. It freezes the
// whole lower hemisphere onto ONE row of the LUT: nothing varies with elevation any more, only the
// 192 azimuth texels do, so the region reads as a flat plate ruled with vertical streaks and cut off
// by a razor-straight line where the clamp begins -- exactly the line and the streaking that were
// being blamed on aerial perspective. (AP measured innocent: on/off differs by one code value there,
// and compose only ever applies it where z > 0.)
//
// Mirroring keeps the whole thing continuous. The reflected angle equals the incident one AT the
// horizon, so there is no step to see, and elevation keeps varying below it, so the streaks have
// something to hide behind. It is also what the engine's world actually is -- flat, with no planet
// to curve away -- and what a plain cubemap does, which is what was asked for.
//
// Everyone who draws the sky uses this, so the drawn sky and the IBL capture cannot land on
// different texels; they did while one clamped the direction to eye level and the other to v = 0.5.
// (v = 0.5 was never a valid clamp on its own: it is the BOUNDARY between the last above-horizon row
// and the first below-horizon one, so a bilinear fetch there returns a hard 50/50 blend of two rows
// a factor of ~500 apart in path length -- 50.6% of the row above it at 1 m, 80.9% at 377 m.)
float2 SkyViewDirToUvNoPlanet(float3 dir, float height, float bottom)
{
    const float beta = acos(saturate(sqrt(max(0.0f, height * height - bottom * bottom)) / height));
    const float horizon = SkyViewPi - beta;
    float angle = acos(clamp(dir.z, -1.0f, 1.0f));
    // Reflected about the HORIZON, not about eye level. The two are 0.15 degrees apart at 22 m and
    // 0.62 at 377 m, and that gap is where the LUT keeps its sharpest, most compressed rows -- a
    // mirror at eye level cannot reach the last 3% of them, which is the same horizon step (4.3
    // levels at 32 m, 18.3 at 377 m) that clamping to eye level used to produce. Reflecting here
    // makes the reflected angle equal the incident one exactly AT the horizon, so the sky is
    // continuous across it and every row stays reachable.
    angle = angle > horizon ? max(2.0f * horizon - angle, 0.0f) : angle;
    float2 uv;
    uv.y = 0.5f * (1.0f - sqrt(saturate(1.0f - angle / max(horizon, 1.e-6f))));
    uv.x = (atan2(-dir.y, -dir.x) + SkyViewPi) / (2.0f * SkyViewPi);
    // UE's asymmetric unit/sub-UV transform, including the +1 denominator, as in SkyViewDirToUv.
    return (uv + 0.5f / SkyViewSize) * SkyViewSize / (SkyViewSize + 1.0f);
}
bool SkyViewIntersectsGround(float3 dir, float height, float bottom)
{
    return dir.z < 0.0f && height * height * (dir.z * dir.z - 1.0f) + bottom * bottom >= 0.0f;
}
#endif
