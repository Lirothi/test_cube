#pragma pack_matrix(row_major)

#include "utils.hlsl"

#ifndef GBUFFER_COMMON_HLSL
#define GBUFFER_COMMON_HLSL

cbuffer PerObject : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;

    float4 baseColor; // fallback для альбедо (linear)
    float2 metalRough; // x = metallic (fallback), y = roughness (fallback)
    float2 texFlags; // x=useAlbedo(0/1), y=useMR(0/1)
};

struct VSIn
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT;
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
    float2 UV : TEXCOORD0;
};

struct PSOut
{
    float4 RT0 : SV_Target0; // Albedo.rgb + A=pack(rough,metal)
    float4 RT1 : SV_Target1; // Normal.xyz (RGB10) + A=1
    float4 RT2 : SV_Target2; // Emissive.rgb
};

VSOut BaseVS(float3 pos, float4x4 world, float4x4 view, float4x4 proj, float3 norm, float2 uv)
{
    VSOut o;
    o.H = TransformPositionH(pos, world, view, proj);
    float3x3 w3 = (float3x3) world;
    o.NWS = NormalizeSafe(TransformDirectionWS(norm, w3), float3(0, 0, 1));
    o.UV = uv;
    return o;
}

PSOut BasePS(float3 albedo, float2 mr, float4 emiss, float3 norm)
{
    PSOut o;
    
    float metal = mr.x;
    float rough = mr.y;

    o.RT0 = float4(albedo, PackRM(rough, metal));
    float3 n01 = NrmTo01(NormalizeSafe(norm, float3(0, 0, 1)));
    o.RT1 = float4(n01, 1.0);
    o.RT2 = emiss;

    return o;
}

#endif