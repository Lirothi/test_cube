// Volumetric clouds, plan C3: the cloud shadow map's READ side, for every pass that lights the
// scene with the sun -- the deferred lighting, the volumetric fog (and later the water).
// Transcribed from UE VolumetricCloudCommon.ush:59-75 GetCloudVolumetricShadow.
//
// The map holds, per texel, three numbers along the light ray from the map's near plane:
//   r: the depth (km) at which the ray first meets cloud   (their ShadowFrontDepthKm)
//   g: the mean extinction (1/m) of the medium it crossed   (MeanExtinction)
//   b: the total optical depth the ray accumulated          (MaxOpticalDepth)
// A receiver at depth d behind the front gets optical depth min(b, g * (d - r)), so a point just
// under the cloud is lit through the cloud's actual thickness and a point far below it gets the
// whole column. One projection convention differs from UE's reversed-Z ortho: OURS is the engine's
// forward ortho, so clip z runs 0 at the near plane to 1 at the far one and the receiver's depth is
// z * farDepthKm outright (theirs is (1 - z) * far).
#ifndef CLOUD_SHADOW_COMMON_HLSLI
#define CLOUD_SHADOW_COMMON_HLSLI

float CloudShadowTransmittance(float3 worldPos, float4x4 shadowViewProj, float farDepthKm,
                               Texture2D<float4> shadowMap, SamplerState linearClamp)
{
    const float4 clip = mul(float4(worldPos, 1.0f), shadowViewProj);
    const float2 uv = clip.xy * float2(0.5f, -0.5f) + 0.5f;
    // Outside the map's footprint nothing is known: unshadowed, as a clamped sampler's edge texel
    // would otherwise smear the last column of cloud to the horizon.
    if (any(uv < 0.0f) || any(uv > 1.0f)) { return 1.0f; }
    const float3 data = shadowMap.SampleLevel(linearClamp, uv, 0).rgb;
    const float sampleDepthKm = saturate(clip.z) * farDepthKm;
    float opticalDepth = data.g * (max(0.0f, sampleDepthKm - data.r) * 1000.0f);
    opticalDepth = min(data.b, opticalDepth);
    return saturate(exp(-opticalDepth));
}

#endif // CLOUD_SHADOW_COMMON_HLSLI
