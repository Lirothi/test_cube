// S15b glass reflection G-buffer prepass.
//
// Rasterizes glass front faces (reverse-Z depth test + write, back-face cull, so the
// front-most glass wins) into a reflection-res G-buffer: world-space normal (RTV,
// R10G10B10A2 = gb1 format, encoded normal*0.5+0.5) + depth (DSV). A second
// rt_reflections_cs dispatch then reads this G-buffer to compute glass reflections
// (including the off-screen recompute), which the forward glass pass samples.
#define GLASS_REFL_PREPASS_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1)"
#pragma pack_matrix(row_major)

cbuffer ObjParams  : register(b0) { float4x4 world; }      // per glass object
cbuffer ViewParams : register(b1) { float4x4 viewProj; }   // per pass

struct VSIn  { float3 P : POSITION; float3 N : NORMAL; float4 T : TANGENT; float2 UV : TEXCOORD0; };
struct VSOut { float4 posH : SV_POSITION; float3 nWS : TEXCOORD0; };

[RootSignature(GLASS_REFL_PREPASS_RS)]
VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 worldPos = mul(float4(i.P, 1.0f), world);
    o.posH = mul(worldPos, viewProj);
    o.nWS  = normalize(mul(i.N, (float3x3)world));
    return o;
}

[RootSignature(GLASS_REFL_PREPASS_RS)]
float4 PSMain(VSOut i) : SV_Target0
{
    // Encoded like the GBuffer normal so rt_reflections_cs decodes it as gb1.rgb*2-1.
    return float4(normalize(i.nWS) * 0.5f + 0.5f, 1.0f);
}
