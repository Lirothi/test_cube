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

// VSM_PAGE=1: the VSM single-draw page permutation. ONE ExecuteIndirect covers every pool page, so
// the per-page projection can no longer arrive as a root CBV (indirect arguments carry no root
// arguments) and the per-page viewport can no longer be set per draw. Instead the VS reads that
// page's matrix + wind out of pageProj_ (an SRV) using a physical page index packed into the HIGH
// bits of the per-instance caster id, remaps clip space into the page's pool cell, and emits the
// page's four borders as SV_ClipDistance0 in place of the scissor. b1 is GONE from this
// permutation's root signature — the projection is no longer a constant buffer at all.
#ifndef VSM_PAGE
#define VSM_PAGE 0
#endif

#if VSM_PAGE
  #if SHADOW_MASKED
    // ONE table: t0 Instances, t1 CasterGroup, t2 GroupMask, t3 PageProj, t4..t19 gMaskAlbedo[16].
    // Folded into a single range because Material::Bind keys tables by their base register and
    // silently drops any base >= RenderContext::kMaxBindings (4) — a second table at t4 would never
    // be bound. PageProj sits BEFORE the albedos so the staged range is
    // {instances, casterGroup, groupMask, pageProj} + MaskedAlbedoCount() and unused albedo slots
    // still need no dummy descriptors.
    #define SHADOW_INDIRECT_CSM_RS \
        "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
        "DescriptorTable(SRV(t0, numDescriptors=20, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
        "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"
  #else
    // t0 Instances, t1 PageProj.
    #define SHADOW_INDIRECT_CSM_RS \
        "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
        "DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
  #endif
#elif SHADOW_MASKED
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
    float    windStrength;  // W3: per-object foliage sway strength (consumed by the shadow VS in W5)
    float    windLeafScale;  // W7.4: WORLD metres of leaf arc per unit of COLOR_0.b (0 = unbaked)
    float    windFoliage;    // PER-SLOT 0..1 (0 = trunk, 1 = leaves)
    float    windTrunkStiff; // per-object; divides the main bend
};

StructuredBuffer<InstancePerObject> Instances : register(t0);

// Mirrors gbuffer_common.hlsli::ApplyWindWS exactly (that header is not included here — this shader
// declares its own leaner PerObject/PerView). Any drift between the two detaches the shadow.
//
// EVERY per-view input is an explicit parameter, so the two permutations physically cannot drift:
// the CB path passes `cbuffer PerView`, the VSM_PAGE path passes the same values read out of that
// page's pageProj_ slot. `w0`/`w1` are the two float4s of the CB's wind tail, verbatim:
//   w0 = (windTime, windPrevTime, windDirXZ.x, windDirXZ.y)   -- byte 192
//   w1 = (windSwayAmp, windSwayFreq, windGustMul, windPrevGustMul) -- byte 208
// Do NOT re-implement any of this per permutation; add parameters instead.
inline float4 WindTransformCore(float3 objPos, float4x4 world, float4 windWeights,
                                float windStrengthValue, float foliageValue, float trunkStiffValue,
                                float leafScaleValue, float4x4 vp, float4 w0, float4 w1)
{
    float4 wp = mul(float4(objPos, 1.0f), world);
    wp.xyz += WindOffset(objPos, wp.xyz, float3(world._41, world._42, world._43), windStrengthValue,
                         windWeights, foliageValue, trunkStiffValue, leafScaleValue,
                         w0.zw, w1.x, w1.y, w1.z, w0.x);
    return mul(wp, vp);
}

#if VSM_PAGE

#include "vsm_addressing.hlsli" // VSM_POOL_PAGES_AXIS (no resource declarations in this header)

#if SHADOW_MASKED
StructuredBuffer<float4> PageProjRows : register(t3); // after CasterGroup/GroupMask, before the albedos
#else
StructuredBuffer<float4> PageProjRows : register(t1);
#endif

// MUST match vsm::kPageIdShift in VirtualShadowMap.h (the CPU side asserts the page fits above it).
static const uint  kPageIdShift = 22u;
static const uint  kCasterMask  = (1u << kPageIdShift) - 1u;
static const float kPoolAxis    = (float)VSM_POOL_PAGES_AXIS;

// One page's 256-byte pageProj_ slot, viewed as 16 float4s: rows 0..3 = the off-center viewProj the
// setup CS built (vsm_page_setup_cs.hlsl stores pm[0..3] at bytes 0/16/32/48), elements 12 and 13 =
// the wind tail it copied to bytes 192/208. Bytes 64..191 are never written and never read.
float4x4 LoadPageVP(uint page, out float4 w0, out float4 w1)
{
    const uint b = page * 16u;  // 256 B slot / 16 B per float4
    w0 = PageProjRows[b + 12u];
    w1 = PageProjRows[b + 13u];
    return float4x4(PageProjRows[b + 0u], PageProjRows[b + 1u],
                    PageProjRows[b + 2u], PageProjRows[b + 3u]);
}

// Page-local clip -> the page's cell of the pool, plus the four page-border clip planes.
//
// The per-page projection already maps the page's virtual sub-rect to the FULL [-1,1] clip volume,
// so placing it in a pool cell is a plain scale/bias on clip space (homogeneous: scale x/y, bias by
// w), and the page's borders are exactly that volume's four side planes. The scale/bias is the
// algebraic inverse of the per-page viewport the CPU loop used to set, which is why the pixel
// footprint — and therefore dz/dpixel and every depth-bias term — is unchanged.
void PagePlace(uint page, float4 hLocal, out float4 H, out float4 CD)
{
    const float s  = 1.0f / kPoolAxis;                    // half a cell in NDC
    const float gx = (float)(page % VSM_POOL_PAGES_AXIS); // same split as the CPU loop's viewport
    const float gy = (float)(page / VSM_POOL_PAGES_AXIS);
    H.x = hLocal.x * s + hLocal.w * (-1.0f + (2.0f * gx + 1.0f) * s);
    H.y = hLocal.y * s + hLocal.w * ( 1.0f - (2.0f * gy + 1.0f) * s); // NDC +y up, pool row 0 is top
    H.z = hLocal.z;
    H.w = hLocal.w;
    CD  = float4(hLocal.w + hLocal.x, hLocal.w - hLocal.x,
                 hLocal.w + hLocal.y, hLocal.w - hLocal.y);
}

#else // per-page loop path: the projection arrives as a root CBV, set per page by the CPU

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
    float  windPrevGustMul; // 220 (unused: depth-only, but kept layout-identical to gbuffer)
};

inline float4 WindTransformH(float3 objPos, float4x4 world, float4 windWeights,
                             float windStrengthValue, float foliageValue, float trunkStiffValue,
                             float leafScaleValue)
{
    return WindTransformCore(objPos, world, windWeights, windStrengthValue, foliageValue,
                             trunkStiffValue, leafScaleValue, viewProj,
                             float4(windTime, windPrevTime, windDirXZ),
                             float4(windSwayAmp, windSwayFreq, windGustMul, windPrevGustMul));
}

#endif // VSM_PAGE

#if SHADOW_MASKED

StructuredBuffer<uint>  CasterGroup : register(t1); // per-caster mesh-group id (region 0, static)
StructuredBuffer<uint2> GroupMask   : register(t2); // per group: x = albedo slot in gMaskAlbedo (~0 = opaque), y = asuint(alphaCutoff)
#if VSM_PAGE
Texture2D    gMaskAlbedo[16] : register(t4);        // t3 is PageProj in this permutation
#else
Texture2D    gMaskAlbedo[16] : register(t3);        // masked groups' albedo textures (alpha channel)
#endif
SamplerState gMaskSmp        : register(s0);

struct VSInMasked
{
    float3 P        : POSITION;  // per-vertex (slot 0)
    float2 UV       : TEXCOORD;  // per-vertex, offset 40 of VertexPNTUV (slot 0)
    float4 WIND     : COLOR0;    // per-vertex, offset 48 — the W7.2 baked wind weights
    uint   casterId : CASTERID;  // per-instance, visible-list stream (slot 1)
};
struct VSOutMasked
{
    float4 H : SV_POSITION;
    float2 UV : TEXCOORD0;
    nointerpolation uint  texSlot : TEXCOORD1; // ~0 = opaque group (PS early-out)
    nointerpolation float cutoff  : TEXCOORD2;
#if VSM_PAGE
    float4 CD : SV_ClipDistance0; // the page's four borders (replaces the per-page scissor)
#endif
};
// The PS never reads the clip distances (the hardware consumed them), so it takes its own struct —
// a PS input signature may be a subset of the VS output's.
struct PSInMasked
{
    float4 H : SV_POSITION;
    float2 UV : TEXCOORD0;
    nointerpolation uint  texSlot : TEXCOORD1;
    nointerpolation float cutoff  : TEXCOORD2;
};

[RootSignature(SHADOW_INDIRECT_CSM_RS)]
VSOutMasked VSMain(VSInMasked i)
{
    VSOutMasked o;
#if VSM_PAGE
    const uint page = i.casterId >> kPageIdShift;
    const uint cid  = i.casterId & kCasterMask;
    const InstancePerObject ip = Instances[cid];
    float4 w0, w1;
    const float4x4 vp = LoadPageVP(page, w0, w1);
    const float4 hLocal = WindTransformCore(i.P, ip.world, i.WIND, ip.windStrength, ip.windFoliage,
                                            ip.windTrunkStiff, ip.windLeafScale, vp, w0, w1);
    PagePlace(page, hLocal, o.H, o.CD);
#else
    const uint cid = i.casterId;
    const InstancePerObject ip = Instances[cid];
    o.H = WindTransformH(i.P, ip.world, i.WIND, ip.windStrength, ip.windFoliage, ip.windTrunkStiff,
                         ip.windLeafScale);
#endif
    o.UV = i.UV;
    const uint2 gm = GroupMask[CasterGroup[cid]];
    o.texSlot = gm.x;
    o.cutoff = asfloat(gm.y);
    return o;
}

[RootSignature(SHADOW_INDIRECT_CSM_RS)]
void PSMain(PSInMasked i)
{
    if (i.texSlot == 0xFFFFFFFFu) { return; } // opaque group — draw-uniform branch, no sample
    const float a = gMaskAlbedo[NonUniformResourceIndex(i.texSlot)].Sample(gMaskSmp, i.UV).a;
    clip(a - i.cutoff);
}

#else // opaque (null-work PS) variant

struct VSInIndirect
{
    float3 P        : POSITION;  // per-vertex, from the mesh vertex buffer (slot 0)
    float4 WIND     : COLOR0;    // per-vertex, offset 48 — the W7.2 baked wind weights
    uint   casterId : CASTERID;  // per-instance, from the visible-list stream (slot 1)
};
struct VSOutD
{
    float4 H : SV_POSITION;
#if VSM_PAGE
    float4 CD : SV_ClipDistance0; // the page's four borders (replaces the per-page scissor)
#endif
};
struct PSInD { float4 H : SV_POSITION; }; // the PS never reads CD (see the masked variant)

[RootSignature(SHADOW_INDIRECT_CSM_RS)]
VSOutD VSMain(VSInIndirect i)
{
    VSOutD o;
#if VSM_PAGE
    const uint page = i.casterId >> kPageIdShift;
    const uint cid  = i.casterId & kCasterMask;
    const InstancePerObject ip = Instances[cid];
    float4 w0, w1;
    const float4x4 vp = LoadPageVP(page, w0, w1);
    const float4 hLocal = WindTransformCore(i.P, ip.world, i.WIND, ip.windStrength, ip.windFoliage,
                                            ip.windTrunkStiff, ip.windLeafScale, vp, w0, w1);
    PagePlace(page, hLocal, o.H, o.CD);
#else
    const InstancePerObject ip = Instances[i.casterId];
    o.H = WindTransformH(i.P, ip.world, i.WIND, ip.windStrength, ip.windFoliage, ip.windTrunkStiff,
                         ip.windLeafScale);
#endif
    return o;
}

// Depth-only — empty pixel shader.
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
void PSMain(PSInD i) { }

#endif // SHADOW_MASKED
