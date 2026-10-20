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
static const float kPointNormalBias = 0.05f; // world units, along the surface normal (B4; the old bias only)

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
