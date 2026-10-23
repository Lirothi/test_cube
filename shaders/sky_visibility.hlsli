// RT sky visibility (interior lighting plan S; photographic plan P9 item 2) -- the CONSUMER half,
// shared by lighting_cs.hlsl and compose_cs.hlsl. The producer is rt_sky_visibility_cs.hlsl.
//
// UE's Distance Field AO gives the sky light a BENT NORMAL: the mean of the unoccluded directions
// over the hemisphere, scaled so that an open hemisphere has length 1 -- its LENGTH is the sky's
// visibility, its DIRECTION where the sky is seen from. We trace it with inline RayQuery against
// the TLAS instead of marching mesh distance fields (this engine has none), and apply it with UE's
// own formulas, transcribed below with their file references. The one point of the exercise: the
// sky stops lighting a room's walls from inside, and the grove's floor from under its crowns.
//
// lighting_cs ADDS the sky specular and compose SUBTRACTS it again where a reflection hit; both
// read the same texel through the same functions, so the two stay each other's exact inverse.
#ifndef SKY_VISIBILITY_HLSLI
#define SKY_VISIBILITY_HLSLI

// UE ApproximateConeConeIntersection (SkyLightingShared.ush:9): the fraction of a cone of half-angle
// arc0 inside one of arc1, their axes `angle` apart.
float SkyVisConeConeIntersection(float arc0, float arc1, float angle)
{
    const float difference = abs(arc0 - arc1);
    return smoothstep(0.0f, 1.0f, 1.0f - saturate((angle - difference) / max(arc0 + arc1 - difference, 1.0e-4f)));
}

// UE UpsampleDFAO (DistanceFieldAOShared.ush:113-162), BILATERAL_UPSAMPLE: the half-resolution
// result's 2x2 around `uv`, bilinear weights times 1 / depth difference. Our .w is the DEVICE depth
// (reverse-Z) the trace saw, compared RELATIVELY -- reverse-Z depth is ~1/distance, so a relative
// step is a relative step in distance. A sky texel (.w 0) never contributes. Load, not Sample: both
// consumers declare their samplers differently and this needs none. xyz = bent normal, w = 1 when
// any corner was a surface (0: nothing to apply, the caller keeps the open sky).
float4 SkyVisUpsample(Texture2D<float4> tex, float2 uv, float sceneDeviceZ)
{
    uint w, h;
    tex.GetDimensions(w, h);
    const float2 pos = uv * float2(w, h) - 0.5f;
    const float2 base = floor(pos);
    const float2 f = pos - base;
    const int2 maxTexel = int2(w, h) - 1;
    const int2 p00 = clamp(int2(base), 0, maxTexel);
    const int2 p11 = clamp(int2(base) + 1, 0, maxTexel);
    const float4 t00 = tex.Load(int3(p00.x, p00.y, 0));
    const float4 t10 = tex.Load(int3(p11.x, p00.y, 0));
    const float4 t01 = tex.Load(int3(p00.x, p11.y, 0));
    const float4 t11 = tex.Load(int3(p11.x, p11.y, 0));
    const float4 bilinear = float4((1.0f - f.y) * (1.0f - f.x), (1.0f - f.y) * f.x, f.y * (1.0f - f.x), f.y * f.x);
    const float4 cornerZ = float4(t00.w, t10.w, t01.w, t11.w);
    const float4 depthWeight = 1.0f / (abs(cornerZ - sceneDeviceZ) / max(sceneDeviceZ, 1.0e-7f) + 1.0e-3f);
    const float4 weight = bilinear * depthWeight * float4(cornerZ > 0.0f);
    const float total = dot(weight, 1.0f.xxxx);
    if (total <= 0.0f)
    {
        return 0.0f.xxxx;
    }
    return float4((weight.x * t00.xyz + weight.y * t10.xyz + weight.z * t01.xyz + weight.w * t11.xyz) / total, 1.0f);
}

// UE GetSkyLightVisibilityData (SkyLightingDiffuseShared.ush:32-81), APPLY_SKY_SHADOWING with
// USE_DIRECTIONAL_OCCLUSION_ON_SKY_DIFFUSE: the sky diffuse is looked up toward the bent normal
// where little sky is seen ("more bent normal in corners") and dimmed by the visibility. UE's
// default contrast (0.01, SkyLightComponent) makes its remap curve the identity to < 0.1 %, exponent
// 1 and min occlusion 0 make the rest identities, so they are left out. `visibility` is UE's
// SkyVisibility before the AO combine (the caller takes min() with its AO, UE's OcclusionCombineMode
// Minimum), `dotFactor` its DotProductFactor, which scales the sky lookup alone. `strength` lerps
// both to the open sky (0 = this whole term off, 1 = UE).
void SkyVisDiffuse(float3 bent, float3 N, float strength, out float3 lookupN, out float visibility, out float dotFactor)
{
    const float skyVisibility = saturate(length(bent));
    const float3 bentDir = bent / max(skyVisibility, 1.0e-5f);
    lookupN = NormalizeSafe(lerp(bentDir, N, skyVisibility), N);
    const float s = saturate(strength);
    dotFactor = lerp(1.0f, lerp(saturate(dot(bentDir, N)), 1.0f, skyVisibility), s);
    visibility = lerp(1.0f, skyVisibility, s);
}

// UE GetDistanceFieldAOSpecularOcclusion (SkyLightingShared.ush:25-64), the cone mode
// (r.DistanceFieldAO.SpecularOcclusionMode 1, the default): the reflection lobe as a cone of
// roughness * PI against the unoccluded cone of length(bent) * PI around the bent normal. Faded to
// fully shadowed where so little sky is seen that the bent normal's direction means nothing.
float SkyVisSpecular(float3 bent, float3 R, float roughness, float strength)
{
    const float bentLength = length(bent);
    const float reflectionCone = max(roughness, 0.1f) * kPi;
    const float unoccludedCone = bentLength * kPi;
    const float angleBetween = acos(clamp(dot(bent, R) / max(bentLength, 0.001f), -1.0f, 1.0f));
    float occlusion = SkyVisConeConeIntersection(reflectionCone, unoccludedCone, angleBetween);
    occlusion = lerp(0.0f, occlusion, saturate((unoccludedCone - 0.1f) / 0.2f));
    return lerp(1.0f, occlusion, saturate(strength));
}

#endif // SKY_VISIBILITY_HLSLI
