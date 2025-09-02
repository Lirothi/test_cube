// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3) SRV(t4)) TABLE(SAMPLER(s0) SAMPLER(s1))
#pragma pack_matrix(row_major)

#include "utils.hlsl"

// ---------- GBuffer inputs ----------
Texture2D GB0 : register(t0); // Albedo.rgb + Metal (a)
Texture2D GB1 : register(t1); // NormalOcta.rg + Rough (b)
Texture2D GB2 : register(t2); // Emissive (в compose)
Texture2D DepthT : register(t3); // R32F (SRV к D32)
Texture2D ShadowAtlas : register(t4);

SamplerState gSmpPoint : register(s0);
SamplerComparisonState gSmpLinear : register(s1); // для PCF (shadow)

// ---------- Per-frame camera/light ----------
cbuffer PerFrame : register(b0)
{
    // Направление ЛУЧЕЙ солнца в мире (куда светит). В лобе нужен вектор к источнику => -sunDirWS
    float3 sunDirWS;
    float ambientIntensity; // 0..1
    float3 lightRgb;
    float exposure; // обычно 1..2
    float3 camPosWS;
    float3 camDirWS;

    float4x4 view;
    float4x4 invView;
    float4x4 invProj;
    
    // === CSM ===
    float4x4 lightViewProj[4]; // мир -> клип света (на каждый каскад)
    float4 cascadeScaleBias[4]; // (scale.xy, bias.xy) в атлас
    float4 cascadeSplitsVS; // z_view границы: [near, split1, split2, far]
    float2 shadowAtlasSize; // (W,H) = (4096,4096)
    float4 shadowBiasNDC; // xyz: depth bias (в НОРМАЛИЗОВАННЫХ координатах z) по каскадам 0..2
    float4 normalBiasWS; // xyz: normal offset (в МИРОВЫХ юнитах) по каскадам 0..2
}

static const float shadowBias = 0.0015f; // базовый bias в 0..1 depth
static const float pcfRadius = 1.0f; // в пикселях тайла

// ---------- VS fullscreen ----------
struct VSOut
{
    float4 H : SV_POSITION;
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

int ChooseCascadeIndex(float3 Pws)
{ 
    float z;
    //z = mul(float4(Pws, 1), view).z;
    z = dot(Pws - camPosWS, camDirWS);
    float3 gt = saturate(sign(z.xxx - cascadeSplitsVS.yzw));
    return (int) (gt.x + gt.y + gt.z);
}

float ShadowPCF(float2 uv, float zRef)
{
    // базовый «hardware PCF» = 2x2 с билинеарными весами
    return ShadowAtlas.SampleCmp(gSmpLinear, uv, zRef).r;
}

// 3x3 поверх hardware PCF (рекомендую для ближнего каскада)
float ShadowPCF3x3(float2 uv, float zRef, float2 texel, float radiusPx)
{
    float s = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 off = float2(x, y) * texel * radiusPx;
            s += ShadowAtlas.SampleCmp(gSmpLinear, uv + off, zRef).r;
        }
    return s / 9.0;
}

float SampleShadowCSM(float3 Pws, float NdotL, float3 Nws)
{
    int idx = ChooseCascadeIndex(Pws);
    //idx = 2;

    float4x4 LVP = lightViewProj[idx];
    float4 sb = cascadeScaleBias[idx];
    float2 scale = sb.xy;
    float2 bias = sb.zw;

    // --- NORMAL OFFSET: двигаем точку вдоль нормали, чтобы убрать “отставание”
    float3 Poff = Pws + Nws * normalBiasWS[idx];

    float4 lc = mul(float4(Poff, 1), LVP);

    float2 uv = (lc.xy / max(1e-6, lc.w)) * float2(0.5, -0.5) + float2(0.5, 0.5);
    float z = lc.z / max(1e-6, lc.w);

    uv = uv * scale + bias;

    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1.0;
    }

    float2 texel = 1.0 / shadowAtlasSize;

    // depth bias в NDC z (пер-каскадно) + немного slope-scaled по NdotL
    float bBase = shadowBiasNDC[idx];
    float b = bBase + (1.0 - saturate(NdotL)) * bBase; // мягкое усиление под острым углом

    if (idx < 3)
    {
        return ShadowPCF3x3(uv, z - b, texel, pcfRadius);
    }
    return ShadowPCF(uv, z - b);
}

// ---------- PS ----------
float4 PSMain(VSOut i) : SV_Target
{
    float4 gb0 = GB0.Sample(gSmpPoint, i.UV);
    float4 gb1 = GB1.Sample(gSmpPoint, i.UV);

    float3 albedo = gb0.rgb;
    float2 rm = UnpackRM(gb0.a);
    float rough = rm.x;
    float metal = rm.y;

    float3 N = normalize(gb1.rgb * 2.0 - 1.0);
    float z = DepthT.Sample(gSmpPoint, i.UV).r;
    float3 P = ReconstructPosWS(i.UV, z, invProj, invView);

    const float3 V = normalize(camPosWS - P);
    const float3 L = normalize(-sunDirWS);

    const float3 ambient = albedo * ambientIntensity;

    BRDFInput bi;
    bi.albedo = albedo;
    bi.rough = rough;
    bi.metal = metal;
    bi.N = N;
    bi.V = V;
    bi.L = L;

    BRDFResult br = EvalBRDF(bi);
    if (br.NdotL <= 0.0)
    {
        return float4(ambient * lightRgb * exposure, 1.0);
    }

    float shadow = SampleShadowCSM(P, br.NdotL, N);
    float3 direct = (br.diffBRDF + br.specBRDF) * br.NdotL * lightRgb * shadow;
    float3 color = direct + ambient;

    return float4(color * exposure, 1.0);
}