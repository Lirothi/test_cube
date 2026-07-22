// Rung 0 / Step 5: indirect depth-only shadow vertex shader. Reads the caster's world matrix
// from the persistent per-caster instance buffer (t0), indexed by a caster id the input
// assembler delivers per-instance — the visible list is bound as a per-instance vertex stream
// (slot 1), and the draw's StartInstanceLocation offsets it, so instance i reads
// visibleList[StartInstanceLocation + i]. Light viewProj comes from the shared per-view CB (b1).
// A plain DRAW_INDEXED command signature (no per-draw root args) drives this; nothing draws
// from it until the Step 6 behavioral flip.
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "wind.hlsli" // W5: the SAME sway function the gbuffer BaseVS uses (shadow must not diverge)

// C2: SHADOW_MASKED=1 builds the alpha-tested variant — used for the WHOLE caster set whenever
// it contains any masked (alphaMode=MASK) group, so the per-page/per-view single-ExecuteIndirect
// structure is preserved (no per-class arg partitioning, no second CPU page loop). Opaque groups
// carry texSlot=~0 and the PS early-outs before sampling. Levels with no masked groups keep the
// null-PS fast path below.
#ifndef SHADOW_MASKED
#define SHADOW_MASKED 0
#endif

#if SHADOW_MASKED
#define SHADOW_INDIRECT_CSM_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
    "CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(SRV(t3, numDescriptors=16, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"
#else
#define SHADOW_INDIRECT_CSM_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
    "CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
#endif

// Matches render::InstancePerObject (224 bytes) / the InstanceArray element in gbuffer_common.
// W3 grew it 208 -> 224 to add windStrength; the tail (emissive + pad) is unused by the shadow
// pass, so it is aliased as padding. W5 reads `windStrength` here to sway the shadow.
struct InstancePerObject
{
    float4x4 world;
    float4x4 prevWorld;
    float4   baseColor;
    float2   metalRough;
    float    alphaCutoff;
    float    mrMultiply;
    float4   texOffsScale;
    float4   texFlags;
    uint     objectId;
    uint3    _instPad1;    // aliases the gbuffer's `emissive` (unused here)
    float    windStrength; // W3: per-object foliage sway strength (consumed by the shadow VS in W5)
    float3   _instPad2;    // pad to 224 (lockstep with render::InstancePerObject)
};

StructuredBuffer<InstancePerObject> Instances : register(t0);

// Shared per-view CB (light viewProj for the shadow passes); same layout as gbuffer_common's.
// W5: the wind tail at offset 192 must stay byte-identical to the gbuffer `cbuffer PerView` — the
// CSM/spot/point paths get it from SceneRenderer::BuildShadowViewCB, and the VSM per-page path from
// the 256-byte PageProj slot the setup CS writes (vsm_page_setup_cs.hlsl stores it at po+192).
// viewProjNoJitter/prevViewProjNoJitter stay unread here (undefined in the VSM slot).
cbuffer PerView : register(b1)
{
    float4x4 viewProj;
    float4x4 viewProjNoJitter;
    float4x4 prevViewProjNoJitter;
    float  windTime;      // 192
    float  windPrevTime;  // 196 (unused: depth-only, no motion vectors)
    float2 windDirXZ;     // 200
    float  windSwayAmp;   // 208
    float  windSwayFreq;  // 212
    float  windGustMul;   // 216
    float  _windPad;      // 220
};

// Mirrors gbuffer_common.hlsli::ApplyWindWS exactly (that header is not included here — this shader
// declares its own leaner PerObject/PerView). Any drift between the two detaches the shadow.
inline float4 WindTransformH(float3 objPos, float4x4 world, float windStrengthValue)
{
    float4 wp = mul(float4(objPos, 1.0f), world);
    wp.xyz += WindOffset(objPos, float3(world._41, world._42, world._43), windStrengthValue,
                         windDirXZ, windSwayAmp, windSwayFreq, windGustMul, windTime);
    return mul(wp, viewProj);
}

#if SHADOW_MASKED

StructuredBuffer<uint>  CasterGroup : register(t1); // per-caster mesh-group id (region 0, static)
StructuredBuffer<uint2> GroupMask   : register(t2); // per group: x = albedo slot in gMaskAlbedo (~0 = opaque), y = asuint(alphaCutoff)
Texture2D    gMaskAlbedo[16] : register(t3);        // masked groups' albedo textures (alpha channel)
SamplerState gMaskSmp        : register(s0);

struct VSInMasked
{
    float3 P        : POSITION;  // per-vertex (slot 0)
    float2 UV       : TEXCOORD;  // per-vertex, offset 40 of VertexPNTUV (slot 0)
    uint   casterId : CASTERID;  // per-instance, visible-list stream (slot 1)
};
struct VSOutMasked
{
    float4 H : SV_POSITION;
    float2 UV : TEXCOORD0;
    nointerpolation uint  texSlot : TEXCOORD1; // ~0 = opaque group (PS early-out)
    nointerpolation float cutoff  : TEXCOORD2;
};

[RootSignature(SHADOW_INDIRECT_CSM_RS)]
VSOutMasked VSMain(VSInMasked i)
{
    VSOutMasked o;
    const InstancePerObject ip = Instances[i.casterId];
    o.H = WindTransformH(i.P, ip.world, ip.windStrength);
    o.UV = i.UV;
    const uint2 gm = GroupMask[CasterGroup[i.casterId]];
    o.texSlot = gm.x;
    o.cutoff = asfloat(gm.y);
    return o;
}

[RootSignature(SHADOW_INDIRECT_CSM_RS)]
void PSMain(VSOutMasked i)
{
    if (i.texSlot == 0xFFFFFFFFu) { return; } // opaque group — draw-uniform branch, no sample
    const float a = gMaskAlbedo[NonUniformResourceIndex(i.texSlot)].Sample(gMaskSmp, i.UV).a;
    clip(a - i.cutoff);
}

#else // opaque (null-work PS) variant

struct VSInIndirect
{
    float3 P        : POSITION;  // per-vertex, from the mesh vertex buffer (slot 0)
    uint   casterId : CASTERID;  // per-instance, from the visible-list stream (slot 1)
};
struct VSOutD { float4 H : SV_POSITION; };

[RootSignature(SHADOW_INDIRECT_CSM_RS)]
VSOutD VSMain(VSInIndirect i)
{
    VSOutD o;
    const InstancePerObject ip = Instances[i.casterId];
    o.H = WindTransformH(i.P, ip.world, ip.windStrength);
    return o;
}

// Depth-only — empty pixel shader.
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
void PSMain(VSOutD i) { }

#endif // SHADOW_MASKED
