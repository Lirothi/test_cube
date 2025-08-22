// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

static const uint kRM_RBits = 5u; // roughness
static const uint kRM_MBits = 3u; // metallic
static const uint kRM_MaxU8 = 255u;
static const uint kRM_MMask = (1u << kRM_MBits) - 1u;
static const float kRM_RScale = float((1u << kRM_RBits) - 1u);
static const float kRM_MScale = float((1u << kRM_MBits) - 1u);

// pack [0..1]x[0..1] -> A8_UNORM
float PackRM(float rough, float metal)
{
    uint r = (uint) round(saturate(rough) * kRM_RScale);
    uint m = (uint) round(saturate(metal) * kRM_MScale);
    uint packed = (r << kRM_MBits) | m; // [rrrrr][mmm]
    return float(packed) / float(kRM_MaxU8);
}

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

    float3 albedo = lerp(baseColor.rgb, albedoTex, useAlbedo);
    albedo *= normalize(i.N) * 0.5 + 0.5;
    //const float3 albedo = float3(normalize(i.N) * 0.5 + 0.5);
    const float metal = lerp(mr.x, mrTex.r, useMR);
    const float rough = lerp(mr.y, mrTex.g, useMR);

    o.RT0 = float4(albedo, PackRM(rough, metal));
    //o.RT1 = float4(no, saturate(rough), 0.0);
    
    //float3 n = normalize(i.N);
    //float2 nxy01 = n.xy * 0.5 + 0.5; // [-1..1] -> [0..1]
    //float signZ = (n.z >= 0.0) ? 1.0 : 0.0; // 2 бита в альфе (RTV квантует в 0 или 1)
    //o.RT1 = float4(nxy01, saturate(rough), signZ);
    
    float3 n01 = normalize(i.N) * 0.5 + 0.5; // [-1..1] -> [0..1]
    o.RT1 = float4(n01, 1); 
    
    o.RT2 = float4(0, 0, 0, 0); // emissive подключишь позже
    //o.RT2 = float4(i.N, 0);
    return o;
}