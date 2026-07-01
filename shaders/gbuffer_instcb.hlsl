// GBuffer auto-instancing variant (Step 4). Per-object data is a constant-buffer array
// (b0) indexed by SV_InstanceID, filled on the CPU from a run of identical (mesh,material)
// objects. Shading mirrors gbuffer.hlsl exactly (shared BaseVS / FetchShadingValuesP /
// FinalizeGBuffer) so instanced objects render pixel-identical to the per-object path.
#pragma pack_matrix(row_major)
#define GBUFFER_SKIP_PEROBJECT
#include "gbuffer_common.hlsl"

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1); // R=metal, G=rough
Texture2D gNormalMap : register(t2); // tangent-space, +Z
SamplerState gSmp : register(s0);

#define GBUFFER_INSTCB_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0)," \
    "CBV(b1)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

struct VSOutInst
{
    float4 H : SV_POSITION;
    float4 clipH : TEXCOORD4;
    float4 prevH : TEXCOORD3;
    float3 NWS : TEXCOORD1;
    float4 TWS : TEXCOORD2; // .xyz = tangent in world, .w = sign
    float2 UV : TEXCOORD0;
    nointerpolation uint IID : TEXCOORD5;
    nointerpolation uint objectId : TEXCOORD6;
};

[RootSignature(GBUFFER_INSTCB_RS)]
VSOutInst VSMain(VSInInst i)
{
    InstancePerObject d = inst[i.IID];
    VSOut b = BaseVS(i.P, d.world, d.prevWorld, viewProj, i.N, i.T, i.UV, d.objectId);

    VSOutInst o;
    o.H = b.H;
    o.clipH = b.clipH;
    o.prevH = b.prevH;
    o.NWS = b.NWS;
    o.TWS = b.TWS;
    o.UV = b.UV;
    o.IID = i.IID;
    o.objectId = b.objectId;
    return o;
}

[RootSignature(GBUFFER_INSTCB_RS)]
PSOut PSMain(VSOutInst i)
{
    InstancePerObject d = inst[i.IID];

    float3 NNorm = normalize(i.NWS);

    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValuesP(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, d.texOffsScale, d.texFlags, albedo, mr, N);

    albedo = lerp(d.baseColor.rgb, albedo, d.texFlags.x);
    mr = lerp(d.metalRough.xy, mr, d.texFlags.y);
    if (d.texFlags.z < 0.5)
    {
        N = NNorm;
    }
    float2 currUv = ClipToUV(i.clipH);
    float2 prevUv = ClipToUV(i.prevH);
    float2 motion = currUv - prevUv;

    return FinalizeGBuffer(albedo, mr, N, float4(0, 0, 0, 0), motion, i.objectId);
}
