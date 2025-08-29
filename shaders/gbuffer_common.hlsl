#pragma pack_matrix(row_major)
#include "utils.hlsl"

#ifndef GBUFFER_COMMON_HLSL
#define GBUFFER_COMMON_HLSL

cbuffer PerObject : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;

    float4 baseColor; // fallback Albedo (linear)
    float2 metalRough; // x=metallic (fallback), y=roughness (fallback)
    float4 texOffsScale;
    float4 texFlags; // x=useAlbedo, y=useMR, z=useNormalMap, w=reserved
};

float2 tfUV(float2 rawUV)
{
    return float2((rawUV + texOffsScale.xy) * texOffsScale.zw);
}

struct VSIn
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT; // .w = handedness
    float2 UV : TEXCOORD0;
};

struct VSInInst
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT;
    float2 UV : TEXCOORD0;
    uint IID : SV_InstanceID;
};

struct VSOut
{
    float4 H : SV_POSITION;
    float3 NWS : TEXCOORD1;
    float4 TWS : TEXCOORD2; // .xyz = tangent in world, .w = sign
    float2 UV : TEXCOORD0;
};

struct PSOut
{
    float4 RT0 : SV_Target0; // Albedo.rgb + A=pack(rough,metal)
    float4 RT1 : SV_Target1; // Normal.xyz (RGB10) + A=1
    float4 RT2 : SV_Target2; // Emissive.rgb
};

inline VSOut BaseVS(float3 pos, float4x4 world, float4x4 view, float4x4 proj, float3 norm, float4 tangent, float2 uv)
{
    VSOut o;
    o.H = TransformPositionH(pos, world, view, proj);

    float3x3 w3 = (float3x3) world;
    float3 N = NormalizeSafe(TransformDirectionWS(norm, w3), float3(0, 0, 1));
    float3 T = NormalizeSafe(TransformDirectionWS(tangent.xyz, w3), float3(1, 0, 0));
    o.TWS = float4(T, tangent.w);
    o.NWS = N;
    o.UV = uv;
    return o;
}

// финальный вывод в MRT по готовым значениям
inline PSOut FinalizeGBuffer(float3 albedo, float2 mr, float3 NWS, float4 emiss)
{
    PSOut o;
    float metal = mr.x;
    float rough = mr.y;

    o.RT0 = float4(albedo, PackRM(rough, metal));
    //o.RT1 = float4(NrmTo01(NormalizeSafe(NWS, float3(0, 0, 1))), 1.0);
    o.RT1 = float4(NrmTo01(NWS), 1.0);
    o.RT2 = emiss;
    return o;
}

//#ifndef NORMALMAP_IS_RG   // 0 = RGB(A) normal map, 1 = RG/BC5
//#define NORMALMAP_IS_RG 1
//#endif

inline void FetchShadingValues(Texture2D txAlbedo, Texture2D txMR, Texture2D txNorm, SamplerState samp, float2 uv, float4 TWS,
                                out float3 albedo, out float2 mr, inout float3 norm)
{
    albedo = txAlbedo.Sample(samp, tfUV(uv)).rgb;
    mr = txMR.Sample(samp, tfUV(uv)).rg;
    
#if NORMALMAP_IS_RG
    // --- RG (BC5/R8G8_UNORM): n.xy в [-1..1], n.z восстанавливаем ---
    float2 nrg = txNorm.Sample(samp, tfUV(uv)).rg * 2.0 - 1.0;
    nrg *= texFlags.w;
    float  nz2 = saturate(1.0 - dot(nrg, nrg));
    float3 nTS = float3(nrg, sqrt(nz2));
#else
    // --- RGB(A): классика ---
    float3 nTS = txNorm.Sample(samp, tfUV(uv)).xyz * 2.0 - 1.0;
    nTS.xy *= texFlags.w * 1;
#endif
    //norm = PerturbNormal_Deriv(nTS, norm, PVS, uv);
    float3 T = normalize(TWS.xyz);
    float3 B = normalize(cross(norm, T) * TWS.w);

    norm = normalize(T * nTS.x + B * nTS.y + norm * nTS.z);
}

#endif