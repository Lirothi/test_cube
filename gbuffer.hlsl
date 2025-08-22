// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

cbuffer PerObject : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
    float4 baseColor; // fallback для альбедо
    float2 mr; // x = metallic, y = roughness (fallback)
    float2 texFlags; // x=useAlbedo(0/1), y=useMR(0/1)
};

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1);
Texture2D gNormalMap : register(t2);
SamplerState gSmp : register(s0);

struct VSIn
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT;
    float2 UV : TEXCOORD0;
};
struct VSOut
{
    float4 H : SV_POSITION;
    float3 N : NORMAL_VS;
    float2 UV : TEXCOORD0;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 wp = mul(float4(i.P, 1), world);
    float4 vp = mul(wp, view);
    o.H = mul(vp, proj);
    float3 nWS = mul(i.N, (float3x3) world);
    o.N = nWS;
    o.UV = i.UV;
    return o;
}

static const float kOctEpsilon = 1e-6;

float2 SignNotZero(float2 v)
{
    // В HLSL sign(0)==0 → плохо для окты. Нам нужно ±1.
    return float2(v.x >= 0.0 ? 1.0 : -1.0,
                  v.y >= 0.0 ? 1.0 : -1.0);
}

float2 EncodeOcta(float3 n)
{
    n = normalize(n);
    // проекция на октаэдр
    n /= (abs(n.x) + abs(n.y) + abs(n.z) + kOctEpsilon);
    float2 p = n.xy;
    if (n.z < 0.0)
    {
	    p = (1.0 - abs(p.yx)) * SignNotZero(p);
    }
    return p * 0.5 + 0.5;
}

struct PSOut
{
    float4 RT0 : SV_Target0; // Albedo.rgb + Metal
    float4 RT1 : SV_Target1; // NormalOcta.rg + Rough + pad
    float4 RT2 : SV_Target2; // Emissive.rgb
};

PSOut PSMain(VSOut i)
{
    PSOut o;

    // флаги наличия текстур
    const float useAlbedo = texFlags.x;
    const float useMR = texFlags.y;

    // семплы (если SRV нулевой, в D3D12 вернутся нули — это ок для наших lerp'ов)
    const float3 albedoTex = gAlbedo.Sample(gSmp, i.UV).rgb;
    const float2 mrTex = gMR.Sample(gSmp, i.UV).rg;

    //const float3 albedo = lerp(baseColor.rgb, albedoTex, useAlbedo);
    const float3 albedo = float3(normalize(i.N) * 0.5 + 0.5);
    const float metal = lerp(mr.x, mrTex.r, useMR);
    const float rough = lerp(mr.y, mrTex.g, useMR);

    const float2 no = EncodeOcta(normalize(i.N));

    o.RT0 = float4(albedo, saturate(metal));
    o.RT1 = float4(no, saturate(rough), 0.0);
    o.RT2 = float4(0, 0, 0, 0); // emissive подключишь позже
    //o.RT2 = float4(normalize(i.N), 0);
    return o;
}