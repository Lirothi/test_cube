// RootSignature: CBV(b0) CBV(b1) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3)) TABLE(SAMPLER(s0) SAMPLER(s1))
#pragma pack_matrix(row_major)
#include "utils.hlsl"

static const float PI = 3.14159265359;

Texture2D GB0    : register(t0); // Albedo.rgb + Metal (a)
Texture2D GB1    : register(t1); // NormalOct.xy (0..1), Roughness (a)
Texture2D GB2    : register(t2); // Emissive (не обязателен здесь)
Texture2D DepthT : register(t3); // линейная глубина [0..1]

SamplerState SampLin : register(s0);
SamplerState SampPt  : register(s1);

cbuffer PerFrame : register(b0)
{
    float4x4 view;
    float4x4 proj;
    float4x4 invView;
    float4x4 invProj;
    float3   camPosWS;
    float    _pad0;
    float2   screenSize;
    float2   _pad1;
};

cbuffer LightCB : register(b1)
{
    float3 lightPosWS;
    float  lightRadius;
    float3 lightColor;
    float  lightIntensity;
};

struct VSOut {
    float4 H : SV_Position;
    float2 UV : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    float2 p = float2(vid == 2 ? 3.0 : -1.0, vid == 1 ? 3.0 : -1.0);
    o.H = float4(p, 0, 1);
    o.UV = float2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5));
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float2 uv = i.UV;

    float4 g0 = GB0.Sample(SampLin, uv);
    float4 g1 = GB1.Sample(SampLin, uv);
    float z = DepthT.Sample(SampPt, uv).r;
    if (z >= 1.0 - kEpsilon)
    {
        discard;
    }

    float3 albedo = g0.rgb;
    float2 rm = UnpackRM(g0.a); // (rough, metal)
    float rough = rm.x;
    float metal = rm.y;
    float3 N = normalize(g1.rgb * 2.0 - 1.0);
    float3 P = ReconstructPosWS(uv, z, invProj, invView);

    float3 Lvec = lightPosWS - P;
    float dist = length(Lvec);
    if (dist > lightRadius)
    {
        discard;
    }
    float3 L = Lvec / max(kEpsilon, dist);

    // плавное затухание по радиусу
    float x = saturate(1.0 - dist / lightRadius);
    float atten = x * x;

    float3 V = normalize(camPosWS - P);

    BRDFInput bi;
    bi.albedo = albedo;
    bi.rough = rough;
    bi.metal = metal;
    bi.N = N;
    bi.V = V;
    bi.L = L;

    BRDFResult br = EvalBRDF(bi);

	float3 radiance = lightColor * lightIntensity * atten;
    float3 contrib = (br.diffBRDF + br.specBRDF) * radiance * br.NdotL;

    return float4(contrib, 0.0);
}