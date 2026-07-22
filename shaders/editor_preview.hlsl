// Editor Content Browser thumbnail preview shader (Step 12E).
//
// A compact material-aware forward pass for asset thumbnails and editor mini-scenes.
// It consumes the real albedo/MR/normal textures and material flags, including
// alpha-test foliage, while keeping the preview scene isolated and lit by one
// directional key light. Compiled at runtime as vs_5_0 / ps_5_0.

#include "utils.hlsli"

cbuffer PreviewCB : register(b0)
{
    row_major float4x4 gMVP;    // model * view * proj (row-vector convention)
    float4 gLightDir;           // xyz = light ray direction, w = exposure
    float4 gEyePosition;        // xyz = preview camera position
    float4 gBaseColor;          // real material baseColor factor
    float4 gMetalRoughAlpha;    // xy = metal/rough, z = alpha cutoff, w = MR multiply
    float4 gTexOffsScale;       // xy = UV offset, zw = UV scale
    float4 gTexFlags;           // xyz = use albedo/MR/normal, w = normal strength
    float4 gMaterialFlags;      // x = glTF MR, y = normal RG, z = double-sided, w = albedo exists
    float4 gSurfaceParams;      // rgb = subsurface color, w = transmission strength
    float4 gSurfaceFlags;       // x = shading model ID, yzw reserved
    float4 gAmbient;            // rgb = light color, w = ambient intensity
};

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1);
Texture2D gNormalMap : register(t2);
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
    // Preview meshes are drawn with an identity model transform. Keeping world-space data
    // directly in the vertex payload leaves room in the 256-byte CB for shading-model params.
    output.worldPos = input.position;
    output.normalW = normalize(input.normal);
    output.tangentW = float4(normalize(input.tangent.xyz), input.tangent.w);
    output.uv = input.uv;
    return output;
}

float4 PSMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    float2 uv = input.uv * gTexOffsScale.zw + gTexOffsScale.xy;
    float4 albedoSample = float4(1.0, 1.0, 1.0, 1.0);
    if (gMaterialFlags.w > 0.5)
    {
        albedoSample = gAlbedo.Sample(gSampler, uv);
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
        float4 packedMR = gMR.Sample(gSampler, uv);
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
        if (gMaterialFlags.y > 0.5)
        {
            float2 xy = gNormalMap.Sample(gSampler, uv).rg * 2.0 - 1.0;
            xy *= gTexFlags.w;
            normalTS = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
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
    const uint shadingModel = (uint)round(gSurfaceFlags.x);
    if (shadingModel == kShadingModelTwoSidedFoliage)
    {
        const float3 subsurfacePayload =
            gSurfaceParams.rgb * gSurfaceParams.w;
        FoliageResult foliage = EvalFoliageBRDF(bi, subsurfacePayload);
        lit += ((foliage.diffBRDF + foliage.specBRDF) * foliage.NdotL +
            foliage.transBRDF) * radiance;
    }
    else
    {
        BRDFResult brdf = EvalBRDF(bi);
        lit += (brdf.diffBRDF + brdf.specBRDF) * brdf.NdotL * radiance;
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

TextureCubeArray gCubeArray : register(t0);

float4 CubeArrayPSMain(CubeVSOutput input) : SV_TARGET
{
    const float2 faceUv = input.uv * 2.0 - 1.0;
    const float3 direction = normalize(float3(1.0, -faceUv.y, faceUv.x));
    return float4(gCubeArray.SampleLevel(gSampler, float4(direction, 0.0), 0.0).rgb, 1.0);
}
