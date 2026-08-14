// Editor Content Browser thumbnail preview shader (Step 12E).
//
// A compact material-aware forward pass for asset thumbnails and editor mini-scenes.
// It consumes the real albedo/MR/normal textures and material flags, including
// alpha-test foliage, while keeping the preview scene isolated and lit by one
// directional key light. Compiled at runtime as vs_5_0 / ps_5_0.

#include "utils.hlsli"
#include "terrain_tiling.hlsli"

cbuffer PreviewCB : register(b0)
{
    row_major float4x4 gMVP;    // model * view * proj (row-vector convention)
    row_major float4x4 gModel;  // object to preview-world
    float4 gLightDir;           // xyz = light ray direction, w = exposure
    float4 gEyePosition;        // xyz = preview camera position
    float4 gBaseColor;          // real material baseColor factor
    float4 gMetalRoughAlpha;    // xy = metal/rough, z = alpha cutoff, w = MR multiply
    float4 gTexOffsScale;       // xy = UV offset, zw = UV scale
    float4 gTexFlags;           // xyz = use albedo/MR/normal, w = normal strength
    float4 gMaterialFlags;      // x = glTF MR, y = normal RG, z = double-sided, w = albedo exists
    float4 gSurfaceParams;      // rgb = subsurface color, w = transmission strength
    float4 gSurfaceFlags;       // x = shading model ID, y = albedo power, z = normal weight,
                                // w = highlight 0..1 (Mesh Editor hover over a slot's controls)
    float4 gTerrainTiling;      // x = zone size, y = rotation radians, z = scale variance, w = blend
    float4 gTerrainEdgeParams;  // x = edge breakup, y = edge detail, zw reserved
    float4 gAmbient;            // rgb = light color, w = ambient intensity
    float4 gEnvironmentParams;  // x = available, y = exposure, z = max mip, w reserved
    float4 gDebugParams;        // x = normal length, yzw = diagnostic line color
    float4 gMarkerParams;       // x = light-position marker, yzw = marker color
    float4 gSkyboxRight;
    float4 gSkyboxUp;
    float4 gSkyboxForward;
    float4 gSkyboxParams;       // xy = horizontal/vertical tan half-FOV, z = exposure
};

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1);
Texture2D gNormalMap : register(t2);
TextureCube gEnvironment : register(t3);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPos : POSITION;
    float3 normalW  : NORMAL;
    float4 tangentW : TANGENT;
    float2 uv       : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), gMVP);
    output.worldPos = mul(float4(input.position, 1.0), gModel).xyz;
    output.normalW = normalize(mul(float4(input.normal, 0.0), gModel).xyz);
    output.tangentW = float4(
        normalize(mul(float4(input.tangent.xyz, 0.0), gModel).xyz), input.tangent.w);
    output.uv = input.uv;
    return output;
}

struct VertexNormalVSOutput
{
    float3 worldPos : POSITION;
    float3 normalW : NORMAL;
};

VertexNormalVSOutput VertexNormalVS(VSInput input)
{
    VertexNormalVSOutput output;
    // Normal diagnostics are only emitted for the untransformed asset mesh. The light marker uses
    // the regular triangle pipeline and must not make this GS path apply its model matrix twice.
    output.worldPos = input.position;
    output.normalW = normalize(input.normal);
    return output;
}

struct VertexNormalGSOutput
{
    float4 position : SV_POSITION;
};

[maxvertexcount(2)]
void VertexNormalGS(point VertexNormalVSOutput input[1],
    inout LineStream<VertexNormalGSOutput> stream)
{
    VertexNormalGSOutput output;
    output.position = mul(float4(input[0].worldPos, 1.0), gMVP);
    stream.Append(output);
    output.position = mul(float4(
        input[0].worldPos + input[0].normalW * gDebugParams.x, 1.0), gMVP);
    stream.Append(output);
}

float4 DebugPS(float4 position : SV_POSITION) : SV_TARGET
{
    return float4(gDebugParams.yzw, 1.0);
}

float4 PSMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    if (gMarkerParams.x > 0.5)
    {
        const float3 markerNormal = normalize(input.normalW);
        const float3 markerView = normalize(gEyePosition.xyz - input.worldPos);
        const float facing = saturate(dot(markerNormal, markerView));
        const float3 markerColor = max(gMarkerParams.yzw, float3(0.08, 0.08, 0.08));
        const float sphereShade = 0.2 + 0.8 * sqrt(facing);
        const float rim = pow(1.0 - facing, 3.0);
        return float4(markerColor * sphereShade + rim * 0.25, 1.0);
    }

    float2 uv = input.uv * gTexOffsScale.zw + gTexOffsScale.xy;
    const uint shadingModel = (uint)round(gSurfaceFlags.x);
    const bool terrain = shadingModel == kShadingModelTerrain;

    TerrainTileSample terrainTile0 = TerrainIdentityTileSample(uv, 1.0);
    TerrainTileSample terrainTile1 = TerrainIdentityTileSample(uv, 0.0);
    TerrainTileSample terrainTile2 = TerrainIdentityTileSample(uv, 0.0);
    if (terrain)
    {
        TerrainBuildTileSamples(
            uv, gTerrainTiling, gTerrainEdgeParams,
            terrainTile0, terrainTile1, terrainTile2);
    }

    float4 albedoSample = float4(1.0, 1.0, 1.0, 1.0);
    if (gMaterialFlags.w > 0.5)
    {
        albedoSample = terrain
            ? TerrainSampleTextureColor(
                gAlbedo, gSampler, terrainTile0, terrainTile1, terrainTile2)
            : gAlbedo.Sample(gSampler, uv);
    }
    if (gMetalRoughAlpha.z >= 0.0)
    {
        clip(albedoSample.a * gBaseColor.a - gMetalRoughAlpha.z);
    }

    float3 albedo = gBaseColor.rgb;
    if (gTexFlags.x > 0.5)
    {
        albedo = gMaterialFlags.x > 0.5
            ? albedoSample.rgb * gBaseColor.rgb
            : albedoSample.rgb;
    }

    float2 mr = gMetalRoughAlpha.xy;
    if (gTexFlags.y > 0.5)
    {
        float4 packedMR = terrain
            ? TerrainSampleTextureColor(
                gMR, gSampler, terrainTile0, terrainTile1, terrainTile2)
            : gMR.Sample(gSampler, uv);
        float2 texturedMR = gMaterialFlags.x > 0.5
            ? packedMR.bg
            : packedMR.rg;
        texturedMR = lerp(texturedMR,
            texturedMR * gMetalRoughAlpha.xy,
            gMetalRoughAlpha.w);
        mr = texturedMR;
    }
    float metallic = saturate(mr.x);
    float roughness = clamp(mr.y, 0.04, 1.0);

    float3 N = normalize(input.normalW);
    if (gMaterialFlags.z > 0.5 && !isFrontFace)
    {
        N = -N;
    }
    if (gTexFlags.z > 0.5)
    {
        float3 normalTS;
        if (terrain)
        {
            const float2 blendedDerivative = TerrainBlendNormalDerivatives(
                gNormalMap, gSampler, terrainTile0, terrainTile1, terrainTile2,
                gMaterialFlags.y > 0.5);
            normalTS = normalize(float3(-blendedDerivative * gTexFlags.w, 1.0));
        }
        else if (gMaterialFlags.y > 0.5)
        {
            float2 xy = gNormalMap.Sample(gSampler, uv).rg * 2.0 - 1.0;
            float z = sqrt(saturate(1.0 - dot(xy, xy)));
            normalTS = normalize(float3(xy * gTexFlags.w, max(z, 1e-4)));
        }
        else
        {
            normalTS = gNormalMap.Sample(gSampler, uv).xyz * 2.0 - 1.0;
            normalTS.xy *= gTexFlags.w;
            normalTS = normalize(normalTS);
        }
        float3 T = normalize(input.tangentW.xyz);
        float3 B = normalize(cross(N, T) * input.tangentW.w);
        N = normalize(T * normalTS.x + B * normalTS.y + N * normalTS.z);
    }

    float3 L = normalize(-gLightDir.xyz);
    float3 V = normalize(gEyePosition.xyz - input.worldPos);
    BRDFInput bi;
    bi.albedo = albedo;
    bi.rough = roughness;
    bi.metal = metallic;
    bi.N = N;
    bi.V = V;
    bi.L = L;

    const float3 radiance = gAmbient.rgb * gLightDir.w;
    float3 lit = albedo * (1.0 - metallic) * gAmbient.w * radiance;
    if (shadingModel == kShadingModelTwoSidedFoliage)
    {
        const float albedoPower = max(gSurfaceFlags.y, 0.0f);
        const float3 albedoTransmission =
            pow(max(saturate(albedo), 1.0e-3), albedoPower);
        const float3 subsurfacePayload =
            gSurfaceParams.rgb * gSurfaceParams.w * albedoTransmission;
        FoliageResult foliage = EvalFoliageBRDF(
            bi, subsurfacePayload, 0.0f, saturate(gSurfaceFlags.z));
        lit += ((foliage.diffBRDF + foliage.specBRDF) * foliage.NdotL +
            foliage.transBRDF) * radiance;
    }
    else
    {
        BRDFResult brdf = EvalBRDF(bi);
        lit += (brdf.diffBRDF + brdf.specBRDF) * brdf.NdotL * radiance;
    }

    // Metals have no diffuse ambient term, so without an environment they appear black except
    // for the narrow direct-light highlight. Use the active scene skybox as a lightweight
    // specular IBL; its mip chain supplies the roughness blur and Schlick Fresnel preserves the
    // material's coloured metallic response.
    if (gEnvironmentParams.x > 0.5)
    {
        const float3 reflectionDir = reflect(-V, N);
        const float environmentMip = roughness * max(gEnvironmentParams.z, 0.0);
        const float3 environmentRadiance = gEnvironment.SampleLevel(
            gSampler, reflectionDir, environmentMip).rgb * gEnvironmentParams.y;
        const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
        const float fresnelWeight = pow(1.0 - saturate(dot(N, V)), 5.0);
        const float3 fresnel = f0 + (1.0 - f0) * fresnelWeight;
        lit += environmentRadiance * fresnel * saturate(1.0 - roughness);
    }

    // Mesh Editor: tint the submesh whose material slot / wind-foliage control is hovered, so it is
    // obvious WHICH part of the model a control affects. A rim term makes it readable even on a
    // submesh that is mostly facing away or in shadow.
    const float highlight = saturate(gSurfaceFlags.w);
    if (highlight > 0.0)
    {
        const float rim = pow(saturate(1.0 - saturate(dot(N, V))), 2.0);
        const float3 tint = float3(0.15, 0.55, 1.0);
        lit = lerp(lit, lit * 0.45 + tint * 0.55, highlight * 0.65);
        lit += tint * (rim * highlight * 1.4);
    }
    return float4(lit, 1.0);
}

// Cubemap thumbnails intentionally show the +X face. A fullscreen triangle
// keeps the result in the same 2D target layout as mesh/material previews.
TextureCube gCube : register(t0);

struct CubeVSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD;
};

CubeVSOutput CubeVSMain(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    const float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    CubeVSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}

float4 CubePSMain(CubeVSOutput input) : SV_TARGET
{
    const float2 faceUv = input.uv * 2.0 - 1.0;
    const float3 direction = normalize(float3(1.0, -faceUv.y, faceUv.x));
    return float4(gCube.SampleLevel(gSampler, direction, 0.0).rgb, 1.0);
}

float4 PreviewSkyboxPSMain(CubeVSOutput input) : SV_TARGET
{
    const float2 viewPlane = float2(
        (input.uv.x * 2.0 - 1.0) * gSkyboxParams.x,
        (1.0 - input.uv.y * 2.0) * gSkyboxParams.y);
    const float3 direction = normalize(
        gSkyboxForward.xyz +
        gSkyboxRight.xyz * viewPlane.x +
        gSkyboxUp.xyz * viewPlane.y);
    const float3 color = gEnvironment.SampleLevel(gSampler, direction, 0.0).rgb *
        max(gSkyboxParams.z, 0.0);
    return float4(color, 1.0);
}

TextureCubeArray gCubeArray : register(t0);

float4 CubeArrayPSMain(CubeVSOutput input) : SV_TARGET
{
    const float2 faceUv = input.uv * 2.0 - 1.0;
    const float3 direction = normalize(float3(1.0, -faceUv.y, faceUv.x));
    return float4(gCubeArray.SampleLevel(gSampler, float4(direction, 0.0), 0.0).rgb, 1.0);
}
