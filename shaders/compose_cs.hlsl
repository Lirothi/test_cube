#define COMPOSE_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=7, flags=DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2))"
// t0: LightTarget (HDR)
// t1: GB2 (Emissive)
// t2: GB0 (Albedo+Metal encoded in A)
// t3: GB1 (Normal01 + Rough encoded in A)
// t4: Depth (R32F SRV created from the DSV)
// t5: Skybox cubemap
// t6: SSR blurred (premultiplied)
// u0: Scene color (HDR)

#pragma pack_matrix(row_major)

#include "utils.hlsl"

Texture2D LightTarget : register(t0);
Texture2D GB2 : register(t1);
Texture2D GB0 : register(t2);
Texture2D GB1 : register(t3);
Texture2D DepthT : register(t4);
TextureCube SkyboxTex : register(t5);
Texture2D SSRBlur : register(t6);

RWTexture2D<float4> SceneColor : register(u0);

SamplerState gSmp : register(s0); // LinearClamp (color)
SamplerState gSmpPoint : register(s1); // PointClamp (depth)

cbuffer PerFrame : register(b0)
{
    float4x4 invView; // view  -> world
    float4x4 invProj; // clip  -> view
    float skyboxIntensity; // 1.0
    float3 camPosWS;
    float  _padding0;
    float2 screenSize;
    float2 invScreenSize;
}

static const float kEps = 1e-6;

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0.xxx - F0) * pow(1.0 - cosTheta, 5.0);
}

inline float ReadDepth(float2 uv)
{
    return DepthT.SampleLevel(gSmpPoint, uv, 0).r; // Always sample LOD0, no bilinear
}

[numthreads(8,8,1)]
[RootSignature(COMPOSE_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = (uint)screenSize.x;
    uint height = (uint)screenSize.y;
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) * invScreenSize;

    float3 lit = LightTarget.SampleLevel(gSmp, uv, 0).rgb;
    float3 emi = GB2.SampleLevel(gSmp, uv, 0).rgb;
    float3 color = lit + emi;

    float z = ReadDepth(uv);
    if (z > kEps)
    {
        float4 gb0 = GB0.SampleLevel(gSmp, uv, 0);
        float4 gb1 = GB1.SampleLevel(gSmp, uv, 0);

        float3 albedo = gb0.rgb;
        float2 rm = UnpackRM(gb0.a);
        float rough = saturate(rm.x);
        float metal = saturate(rm.y);

        float3 N_ws = normalize(gb1.rgb * 2.0 - 1.0);
        float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);

        float3 Pw = ReconstructPosWS(uv, z, invProj, invView);
        float3 Vw = NormalizeSafe(camPosWS - Pw, float3(0.0, 0.0, 1.0));
        float3 Rw = NormalizeSafe(reflect(-Vw, N_ws), N_ws);

        float4 ssrT = SSRBlur.SampleLevel(gSmp, uv, 0); // premultiplied
        float ssrA = ssrT.a;
        float3 ssrRGB = ssrT.rgb;

        float3 skyCol = SkyboxTex.SampleLevel(gSmp, Rw, 0).rgb * skyboxIntensity;

        // Skybox as fallback: (ssrColor*α + sky*(1-α))
        float3 refl = ssrRGB + skyCol * (1.0 - ssrA);

        float cosT = saturate(dot(N_ws, Vw));
        float3 F = FresnelSchlick(cosT, F0);
        float gloss = saturate(1.0 - rough);

        float3 spec = refl * F * pow(gloss, 1);
        color += spec;
    }

    SceneColor[dispatchThreadId.xy] = float4(color, 1.0);
}
