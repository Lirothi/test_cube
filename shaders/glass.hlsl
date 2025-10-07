// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3)) TABLE(SRV(t4) SRV(t5)) TABLE(SAMPLER(s0) SAMPLER(s1) SAMPLER(s2))
#pragma pack_matrix(row_major)
#include "utils.hlsl"

struct PointLightData
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

struct SpotLightData
{
    float4 positionRange;      // xyz = position, w = range
    float4 directionCosOuter;  // xyz = direction, w = cos(outer)
    float4 colorIntensity;     // xyz = color, w = intensity
    float4 shadowParams;       // x = cos(inner), y = shadow index, z = invAngleRange, w = depth bias
    float4 shadowParams2;      // x = normal bias (world units)
    float4x4 viewProj;
};

cbuffer GlassParams : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
    float4x4 invView;
    float4x4 invProj;
    float4 cameraPosIor;          // xyz = camera position, w = IOR
    float4 absorptionThickness;   // xyz = absorption, w = thickness
    float4 tintRoughness;         // xyz = tint color, w = roughness
    float4 reflectionRefraction;  // x = reflection strength, y = refraction distortion, z = sky intensity, w = unused
    float4 sunDirAmbient;         // xyz = sun direction, w = ambient intensity
    float4 sunColorExposure;      // xyz = sun color, w = sun exposure
    float4 camDirWS;              // xyz = camera forward, w unused
    float4 screenSizeInv;         // xy = screen size, zw = inverse screen size
    float4 shadowAtlasSizeInv;    // xy = atlas size, zw = inverse atlas size
    float4 shadowBiasNDC;         // cascade depth bias
    float4 normalBiasWS;          // cascade normal bias
    float4 cascadeSplitsVS;       // cascade splits in view space
    float4 cascadeScaleBias[4];   // xy = scale, zw = bias per cascade
    float4 spotShadowInfo;        // xy = spot shadow size, zw = inverse size
    float4 lightCounts;           // x = point lights, y = spot lights
    float4x4 lightViewProj[4];
};

Texture2D SceneOpaque : register(t0);
Texture2D ShadowAtlas : register(t1);
Texture2DArray SpotShadowAtlas : register(t2);
TextureCube SkyboxTex : register(t3);
StructuredBuffer<PointLightData> PointLights : register(t4);
StructuredBuffer<SpotLightData> SpotLights : register(t5);

SamplerState LinearSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
SamplerState EnvSampler : register(s2);

struct VSIn
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT;
    float2 UV : TEXCOORD0;
};

struct VSOut
{
    float4 posH : SV_POSITION;
    float3 posWS : TEXCOORD0;
    float3 normalWS : TEXCOORD1;
};

VSOut VSMain(VSIn input)
{
    VSOut o;
    float4 worldPos = mul(float4(input.P, 1.0f), world);
    o.posWS = worldPos.xyz;
    o.posH = mul(mul(worldPos, view), proj);
    float3x3 w3 = (float3x3) world;
    o.normalWS = NormalizeSafe(mul(input.N, w3), float3(0.0f, 1.0f, 0.0f));
    return o;
}

int ChooseCascadeIndex(float3 Pws)
{
    float3 camPos = cameraPosIor.xyz;
    float3 camDir = normalize(camDirWS.xyz);
    float z = dot(Pws - camPos, camDir);
    float3 gt = saturate(sign(z.xxx - cascadeSplitsVS.yzw));
    return (int)(gt.x + gt.y + gt.z);
}

float ShadowPCF3x3Texture(Texture2D atlas, float2 uv, float depth, float2 texel)
{
    float s = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texel;
            s += atlas.SampleCmpLevelZero(ShadowSampler, uv + offset, depth);
        }
    }
    return s / 9.0f;
}

float SampleShadowCSM(float3 Pws, float3 Nws, float NdotL)
{
    int idx = ChooseCascadeIndex(Pws);
    float4 sb = cascadeScaleBias[idx];
    float2 scale = sb.xy;
    float2 bias = sb.zw;

    float normalBias = normalBiasWS[idx];
    float depthBias = shadowBiasNDC[idx];

    float3 Poff = Pws + Nws * normalBias;
    float4 clip = mul(float4(Poff, 1.0f), lightViewProj[idx]);
    float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    float2 uv = ndc.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    float z = ndc.z;

    uv = uv * scale + bias;

    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return 1.0f;
    }

    float biasFactor = depthBias + (1.0f - NdotL) * depthBias;
    float2 texel = shadowAtlasSizeInv.zw;
    return ShadowPCF3x3Texture(ShadowAtlas, uv, z - biasFactor, texel);
}

float ShadowPCF3x3Array(Texture2DArray atlas, float3 uvw, float depth, float2 texel)
{
    float s = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texel;
            s += atlas.SampleCmpLevelZero(ShadowSampler, float3(uvw.xy + offset, uvw.z), depth);
        }
    }
    return s / 9.0f;
}

float SampleSpotShadow(const SpotLightData light, float3 P, float3 N, float NdotL)
{
    float normalBias = light.shadowParams2.x;
    float depthBias = light.shadowParams.w;

    float3 Poff = P + N * normalBias;
    float4 clip = mul(float4(Poff, 1.0f), light.viewProj);
    float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    float2 uv = ndc.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    float depth = ndc.z;
    if (any(uv < 0.0f) || any(uv > 1.0f) || depth <= 0.0f || depth >= 1.0f)
    {
        return 1.0f;
    }

    float3 uvw = float3(uv, light.shadowParams.y);
    float2 texel = spotShadowInfo.zw;
    return ShadowPCF3x3Array(SpotShadowAtlas, uvw, depth - depthBias, texel);
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 N = normalize(i.normalWS);
    float3 camPos = cameraPosIor.xyz;
    float3 V = normalize(camPos - i.posWS);
    float ior = max(cameraPosIor.w, 1.0f);
    float3 absorption = max(absorptionThickness.xyz, 0.0f.xxx);
    float thickness = max(absorptionThickness.w, 0.0f);
    float3 tint = saturate(tintRoughness.xyz);
    float rough = saturate(tintRoughness.w);
    float reflectionStrength = max(reflectionRefraction.x, 0.0f);
    float refractionDistortion = reflectionRefraction.y;
    float skyIntensity = reflectionRefraction.z;

    float3 sunDir = normalize(sunDirAmbient.xyz);
    float ambientIntensity = sunDirAmbient.w;
    float3 sunColor = sunColorExposure.xyz * sunColorExposure.w;

    float3 diffuseAccum = tint * ambientIntensity;
    float3 specAccum = 0.0f.xxx;

    // Directional light
    {
        BRDFInput bi;
        bi.albedo = tint;
        bi.rough = rough;
        bi.metal = 0.0f;
        bi.N = N;
        bi.V = V;
        bi.L = normalize(-sunDir);

        BRDFResult br = EvalBRDF(bi);
        if (br.NdotL > 0.0f)
        {
            float shadow = SampleShadowCSM(i.posWS, N, br.NdotL);
            float3 radiance = sunColor;
            diffuseAccum += br.diffBRDF * br.NdotL * radiance * shadow;
            specAccum += br.specBRDF * br.NdotL * radiance * shadow;
        }
    }

    uint pointCount = (uint)lightCounts.x;
    for (uint idx = 0; idx < pointCount; ++idx)
    {
        PointLightData light = PointLights[idx];
        float3 Lvec = light.position - i.posWS;
        float dist = length(Lvec);
        if (dist > light.radius || light.radius <= kEpsilon)
        {
            continue;
        }
        float3 L = Lvec / max(dist, kEpsilon);
        float atten = saturate(1.0f - dist / light.radius);
        atten *= atten;

        BRDFInput bi;
        bi.albedo = tint;
        bi.rough = rough;
        bi.metal = 0.0f;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        BRDFResult br = EvalBRDF(bi);
        if (br.NdotL <= 0.0f)
        {
            continue;
        }

        float3 radiance = light.color * light.intensity * atten;
        diffuseAccum += br.diffBRDF * br.NdotL * radiance;
        specAccum += br.specBRDF * br.NdotL * radiance;
    }

    uint spotCount = (uint)lightCounts.y;
    for (uint idx = 0; idx < spotCount; ++idx)
    {
        SpotLightData light = SpotLights[idx];
        float3 Lvec = light.positionRange.xyz - i.posWS;
        float dist = length(Lvec);
        if (dist >= light.positionRange.w || light.positionRange.w <= kEpsilon)
        {
            continue;
        }
        float3 L = Lvec / max(dist, kEpsilon);

        float spotCos = dot(-L, light.directionCosOuter.xyz);
        if (spotCos <= light.directionCosOuter.w)
        {
            continue;
        }

        float angleAtten = saturate((spotCos - light.directionCosOuter.w) * light.shadowParams.z);
        angleAtten *= angleAtten;

        float distAtten = saturate(1.0f - dist / light.positionRange.w);
        distAtten *= distAtten;

        BRDFInput bi;
        bi.albedo = tint;
        bi.rough = rough;
        bi.metal = 0.0f;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        BRDFResult br = EvalBRDF(bi);
        if (br.NdotL <= 0.0f)
        {
            continue;
        }

        float shadow = SampleSpotShadow(light, i.posWS, N, br.NdotL);
        float3 radiance = light.colorIntensity.xyz * light.colorIntensity.w * distAtten * angleAtten;
        diffuseAccum += br.diffBRDF * br.NdotL * radiance * shadow;
        specAccum += br.specBRDF * br.NdotL * radiance * shadow;
    }

    float3 reflectionDir = reflect(-V, N);
    float3 envRefl = SkyboxTex.SampleLevel(EnvSampler, reflectionDir, rough * 5.0f).rgb * skyIntensity;
    //return float4(envRefl, 1.0f);

    //ior = 1.0f;
    float eta = 1.0f / ior;
    float3 refrDir = refract(-V, N, eta);
    bool totalInternal = dot(refrDir, refrDir) < 1e-6f;

    //thickness = 0.01f;
    float3 refrColor = 0.0f.xxx;
    if (!totalInternal)
    {
        float3 refrPosWS = i.posWS + refrDir * thickness;
        float4 clip = mul(mul(float4(refrPosWS, 1.0f), view), proj);
        float2 uv = clip.xy / max(clip.w, 1e-6f);
        uv = uv * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        uv += N.xy * refractionDistortion;
        uv = saturate(uv);
        refrColor = SceneOpaque.Sample(LinearSampler, uv).rgb;
    }
    //return float4(refrColor, 1.0f);
    //absorption = float3(0.16f, 0.07f, 0.03f) * 1.0f;
    float3 transmittance = exp(-absorption * thickness);
    refrColor *= transmittance;
    //return float4(refrColor, 1.0f);

    float cosTheta = saturate(dot(N, V));
    float r0 = (ior - 1.0f) / (ior + 1.0f);
    float3 F0 = float3(r0 * r0, r0 * r0, r0 * r0);
    float3 F = F_Schlick(cosTheta, F0);
    if (totalInternal)
    {
        F = 1.0f.xxx;
    }

    float3 lightingColor = diffuseAccum + specAccum;
    float3 color = lightingColor + refrColor * (1.0f - F) + envRefl * (reflectionStrength * F);

    //color = diffuseAccum;

	color = max(color, 0.0f.xxx);

    float transAvg = (transmittance.x + transmittance.y + transmittance.z) * (1.0f / 3.0f);
    float Favg = (F.x + F.y + F.z) * (1.0f / 3.0f);
    float alpha = saturate(1.0f - (1.0f - Favg) * transAvg);
    alpha = saturate(alpha);

    //color = alpha.xxx;
    //alpha = 1.0f;
    return float4(color, alpha);
}
