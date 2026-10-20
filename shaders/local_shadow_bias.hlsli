#ifndef LOCAL_SHADOW_BIAS_HLSLI
#define LOCAL_SHADOW_BIAS_HLSLI

// Local-light shadow plan S1 (docs/local_light_shadows_plan.md): the point light's receiver bias, ONE copy for every
// pass that samples the cube atlas (pointlight_cs, glass, fog_scatter). PointLightGpu carries it:
//   shadowParams.y < 0  = UE's bias (render::g_localShadowBiasUe): -y = DepthBias, volumetric.y = the slope scale.
//                         UE biases a point light at projection time only (ShadowProjectionPixelShader.usf:487-511,
//                         ShadowProjectionCommon.ush:193-194): (DepthBias + (1 - NoL) * SlopeDepthBias) / w in its
//                         reverse-Z device depth, i.e. the compare distance moves toward the light by that FRACTION
//                         of itself -- about 5 texels at any distance. No normal offset.
//   shadowParams.y >= 0 = the old bias: y metres toward the light, kPointNormalBias along the normal.
//   shadowFade.w > 0    = the TEXEL bias (render::g_localShadowBiasTexel, the atlas modes' default since 2026-10-20):
//                         shadowFade.w = normal texels, volumetric.y = push texels, shadowParams.y = 0 (no fraction).
//                         The receiver moves, the compare does not (LocalShadowTexelOffset below).
static const float kPointNormalBias = 0.05f; // world units, along the surface normal (B4; the old bias only)

// The TEXEL bias, spot and point alike -- our VSM local-light model (spotlight_cs ComputeSpotShadow's VSM branch,
// vsm::g_localLateralTexels / g_localDepthPushTexels) on the atlases. The receiver is pushed `normalTexels` shadow
// texels along its normal and `pushTexels` x slope (1 / NoL, 1..6) toward the light. Why not UE's: its projection
// bias is a FRACTION of the distance (~5 texels at bias 0.5, its slope part at most 2.5x through 1 - NoL), sized for
// grazing receivers, so it detaches the shadow from every caster's foot -- the peter panning the owner saw. A normal
// push covers the filter on a slope instead: a tap o texels out finds the plane o sin(theta) texels nearer the light,
// a push of n texels along the normal lifts the receiver n / cos(theta) along the ray -- so n >= o clears every angle,
// while a vertical caster's foot moves by a fraction of a texel. `texelWorld` = one shadow texel in metres at the
// receiver (a cube face spans 2m over its res; a spot 2 w tan(outer) over its res).
// The spot tent's soft-occlusion ramp under the TEXEL bias, in shadow texels at the receiver (UE's is (f - n) / 60 m).
static const float kSpotRampTexels = 2.0f;

// `N` here is the GEOMETRIC normal (LocalShadowGeometricNormal in a light pass, the interpolated vertex normal in glass).
float3 LocalShadowTexelOffset(float3 P, float3 N, float3 toLight, float texelWorld, float normalTexels, float pushTexels)
{
    const float ndl = saturate(dot(N, toLight));
    const float slope = clamp(1.0f / max(ndl, 0.15f), 1.0f, 6.0f);
    return P + N * (texelWorld * normalTexels) + toLight * (texelWorld * slope * pushTexels);
}

// The GEOMETRIC normal the texel bias pushes along, rebuilt from the depth buffer the light pass reads (gtao_cs's
// ViewNormalFromDepth, in world space). The G-buffer normal carries NORMAL MAPPING, while the shadow map holds bare
// geometry: a push along a detail normal tilted away from the light buys no depth on a grazing receiver, and wind_test's
// sand kept a stipple of acne in the pattern of its ripple map at every normal push up to 8 texels. Each axis takes the
// neighbour with the smaller depth step (the other may be another surface) and never one past the frame's edge (the clamp
// hands back the edge pixel's own depth, the "smallest step" of all, and the bottom row darkened); the result faces the
// camera, and the shading normal `fallbackN` stands in when the rebuild has nothing to work with (a one-pixel sliver,
// opposite hemispheres).
float3 LocalShadowGeometricNormal(Texture2D depthTex, SamplerState pointSmp, float2 uv, float2 invSize, float z, float3 P,
                                  float3 camPos, float4x4 invProj, float4x4 invView, float3 fallbackN)
{
    const float2 dx = float2(invSize.x, 0.0f);
    const float2 dy = float2(0.0f, invSize.y);
    const float zL = depthTex.SampleLevel(pointSmp, uv - dx, 0).r;
    const float zR = depthTex.SampleLevel(pointSmp, uv + dx, 0).r;
    const float zT = depthTex.SampleLevel(pointSmp, uv - dy, 0).r;
    const float zB = depthTex.SampleLevel(pointSmp, uv + dy, 0).r;
    const bool useR = (uv.x + dx.x < 1.0f) && ((uv.x - dx.x <= 0.0f) || abs(zR - z) < abs(z - zL));
    const bool useB = (uv.y + dy.y < 1.0f) && ((uv.y - dy.y <= 0.0f) || abs(zB - z) < abs(z - zT));
    const float3 px = useR ? ReconstructPosWS(uv + dx, zR, invProj, invView) - P
                           : P - ReconstructPosWS(uv - dx, zL, invProj, invView);
    const float3 py = useB ? ReconstructPosWS(uv + dy, zB, invProj, invView) - P
                           : P - ReconstructPosWS(uv - dy, zT, invProj, invView);
    const float3 c = cross(px, py);
    const float len2 = dot(c, c);
    if (!(len2 > 1e-20f))
    {
        return fallbackN;
    }
    float3 n = c * rsqrt(len2);
    n = (dot(n, camPos - P) < 0.0f) ? -n : n;
    return (dot(n, fallbackN) > 0.0f) ? n : fallbackN;
}

float PointShadowNormalOffset(float4 shadowParams)
{
    return shadowParams.y < 0.0f ? 0.0f : kPointNormalBias;
}

// The biased major-axis distance the cube is compared at. `m` = the face's view depth of the receiver, `NoL` = its
// normal against the direction to the light (1 for a point in the air).
float PointShadowCompareDistance(float m, float4 shadowParams, float slopeScale, float NoL)
{
    const float nearP = shadowParams.z;
    if (shadowParams.y < 0.0f)
    {
        const float rel = -shadowParams.y * (1.0f + slopeScale * (1.0f - saturate(NoL)));
        return max(m / (1.0f + rel), nearP);
    }
    return max(m - shadowParams.y, nearP);
}

#endif // LOCAL_SHADOW_BIAS_HLSLI
