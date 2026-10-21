#ifndef RT_LIGHTS_HLSLI
#define RT_LIGHTS_HLSLI

// Spot / point light evaluation for RT reflection hit shading. The structs and
// attenuation/cone math mirror spotlight_cs.hlsl / pointlight_cs.hlsl exactly so a
// reflected off-screen surface is lit identically to its on-screen counterpart.
// (LightManager::SpotLightGpu / PointLightGpu provide the matching C++ layout.)

struct SpotLightData
{
    float4   positionRange;     // xyz = position, w = range
    float4   directionCosOuter; // xyz = direction, w = cos(outer)
    float4   colorIntensity;    // xyz = color, w = intensity
    float4   shadowParams;      // x = cos(inner), y = shadow index, z = invAngleRange, w = depth bias
    float4   shadowParams2;     // x = normal bias, y = volumetric scattering, z = UE's shadow fade (S3), w = texel-bias normal texels
    float4x4 viewProj;
    float4   shadowParams3;     // local-light shadows S2: x = near, y = soft-transition scale, z = sharpen, w = filter
    float4   shadowAtlas;       // S9: its square in its atlas page, page UV: xy = corner, z = size, w = page
    float4   shadowParams4;     // S10e: x = the light's VSM mip bias; S4: y = 1 / (f - n) of a LINEAR atlas depth (0 = perspective)
};

struct PointLightData
{
    float3 position;
    float  radius;
    float3 color;
    float  intensity;
    float4 shadowParams; // x = shadow slot (-1 = none), y = bias, z = nearPlane, w = farPlane (radius)
    float4 volumetric;   // x = volumetric scattering intensity (plan A4d), y = UE slope scale (S1), z = sharpen, w = filter (S2)
    float4 shadowFade;   // x = UE's shadow fade (S3), y = the atlas page (S9), z = 1 / its face res (S4), w = texel-bias normal texels
    float4 shadowAtlas;  // S9: its 3f x 2f block in the page, page UV: xy = corner, zw = one face -- mirrors LightManager::PointLightGpu
    float4 vsm;          // S10e: x = the light's VSM mip bias (unread here)
};

// Returns the spot light's radiance (color*intensity*distAtten*coneAtten) and the
// direction/distance to it; returns 0 when the surface is out of range or cone.
// The caller applies the BRDF and a shadow ray. Matches spotlight_cs.hlsl.
float3 RtEvalSpotLight(SpotLightData light, float3 P, out float3 L, out float dist)
{
    L = float3(0.0f, 0.0f, 1.0f); dist = 0.0f;
    float3 Lvec = light.positionRange.xyz - P;
    float d = length(Lvec);
    if (d >= light.positionRange.w || light.positionRange.w <= kEpsilon) { return float3(0.0f, 0.0f, 0.0f); }

    float3 dir = Lvec / max(d, kEpsilon);
    float spotCos = dot(-dir, light.directionCosOuter.xyz);
    if (spotCos <= light.directionCosOuter.w) { return float3(0.0f, 0.0f, 0.0f); }

    float angleAtten = saturate((spotCos - light.directionCosOuter.w) * light.shadowParams.z);
    angleAtten = angleAtten * angleAtten;
    const float distAtten = LightDistanceAttenuation(d, light.positionRange.w); // P16.5

    L = dir; dist = d;
    return light.colorIntensity.xyz * light.colorIntensity.w * distAtten * angleAtten;
}

// Returns the point light's radiance (color*intensity*distAtten) and direction/
// distance; 0 when out of range. Matches pointlight_cs.hlsl (no shadow).
float3 RtEvalPointLight(PointLightData light, float3 P, out float3 L, out float dist)
{
    L = float3(0.0f, 0.0f, 1.0f); dist = 0.0f;
    float3 Lvec = light.position - P;
    float d = length(Lvec);
    if (d > light.radius || light.radius <= kEpsilon) { return float3(0.0f, 0.0f, 0.0f); }

    L = Lvec / max(d, kEpsilon); dist = d;
    return light.color * light.intensity * LightDistanceAttenuation(d, light.radius); // P16.5
}

#endif // RT_LIGHTS_HLSLI
