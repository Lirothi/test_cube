// Editor Content Browser thumbnail preview shader (Step 12E).
//
// A single neutral forward pass used only to generate asset thumbnails: it
// transforms a PosNormTanUV mesh, shades it with one directional key light plus a
// flat ambient term, and optionally modulates by an albedo texture (material
// previews). Compiled at runtime as vs_5_0 / ps_5_0 by EditorPreviewRenderer, so
// it must not depend on the engine's bindless/SM6.6 conventions.

cbuffer PreviewCB : register(b0)
{
    row_major float4x4 gMVP;    // model * view * proj (row-vector convention)
    row_major float4x4 gModel;  // world transform for the normal (identity here)
    float4 gLightDir;           // xyz = world direction toward the key light
    float4 gBaseColor;          // rgb = flat tint, a = hasAlbedo (1 = sample gAlbedo)
    float4 gAmbient;            // rgb = ambient color
};

Texture2D gAlbedo : register(t0);
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
    float3 normalW  : NORMAL;
    float2 uv       : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), gMVP);
    output.normalW = normalize(mul(input.normal, (float3x3)gModel));
    output.uv = input.uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float3 albedo = gBaseColor.rgb;
    if (gBaseColor.a > 0.5)
    {
        albedo = gAlbedo.Sample(gSampler, input.uv).rgb;
    }

    float3 N = normalize(input.normalW);
    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));

    // Key light plus flat ambient; enough to read silhouette and surface without
    // pretending to be the real material pipeline.
    float3 lit = albedo * (gAmbient.rgb + ndl);
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
