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

// Tessy N3 (docs/tessy_plan.md): VSM_TESSY=1 is VSM_PAGE drawing CLUSTERS -- UE's VS raster path
// (NaniteRasterizer.usf:3161-3222): DrawInstanced(128 * 3, clusters), the cluster from SV_InstanceID (a record of
// TessyVisible, written by tessy_vsm_cull_cs.hlsl), the vertex from MegaIB[first + SV_VertexID] and the mega VB
// read by hand, no input assembler at all; a vertex past the cluster's triangles is a clipped degenerate. Page
// placement, wind, the masked clip and the borders are VSM_PAGE's own code.
// N6: VSM_TESSY_MS=1 adds MSMain, the same draw as a MESH shader (UE's hardware path, NaniteRasterizer.usf
// HWRasterizeMS): one group per cluster, each of its unique vertices transformed ONCE (the VS path does every corner
// of every triangle), a triangle wholly outside one page border culled before the rasterizer.
#ifndef VSM_TESSY
#define VSM_TESSY 0
#endif
#ifndef VSM_TESSY_MS
#define VSM_TESSY_MS 0
#endif
#if VSM_TESSY_MS
#undef VSM_TESSY
#define VSM_TESSY 1
#endif
#if VSM_TESSY
#undef VSM_PAGE
#define VSM_PAGE 1
#endif
// Tessy N4: TESSY_ATLAS_MS=1 is the same mesh shader drawing the surviving clusters of a LOCAL light's atlas view (a
// point light's cube face, a spot) -- the atlas PSOs' own path (the view's PerView CB at b1, SHADOW_DEPTH_BIAS's
// normal, SHADOW_LINEAR_DEPTH's twin), its records written by tessy_atlas_cull_cs.hlsl per view. Never with VSM_PAGE.
#ifndef TESSY_ATLAS_MS
#define TESSY_ATLAS_MS 0
#endif
// The cluster view (tessy::g_debugView): TESSY_ATLAS_MS with TESSY_VIEW draws the CAMERA row's records in colour over
// the lit frame -- b1 the camera's G-buffer PerView (BuildGBufferViewCB: the jittered matrix and the same wind), no
// depth bias, no linear depth; the pixel shader returns the cluster's (or, with bit 31 of the root constant, the
// meshlet's) colour, shaded a little by the normal, after the masked clip.
#ifndef TESSY_VIEW
#define TESSY_VIEW 0
#endif

// Texture streaming plan A6: GBUFFER_BINDLESS=1 (with SHADOW_MASKED) reads the masked albedo through
// SM6.6 ResourceDescriptorHeap[] -- GroupMask.x is then the texture's slot in the shared heap, the
// gMaskAlbedo[16] table is gone and so is the 16-group cap.
#ifndef GBUFFER_BINDLESS
#define GBUFFER_BINDLESS 0
#endif

#if TESSY_ATLAS_MS
  // b0 = the view's row of TessyAtlasViewInfo, the triangle culls and the viewport's size (root constants, set per view),
  // b1 = the view's PerView; one table: Instances (+ CasterGroup, GroupMask), TessyVisible, MegaVB, Meshlets,
  // ClusterVerts, ClusterTris, TessyAtlasViewInfo (+ the masked albedos).
  #if SHADOW_MASKED && GBUFFER_BINDLESS
    #define SHADOW_INDIRECT_CSM_RS \
        "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), " \
        "RootConstants(num32BitConstants=4, b0), CBV(b1), " \
        "DescriptorTable(SRV(t0, numDescriptors=9, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
        "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"
  #elif SHADOW_MASKED
    #define SHADOW_INDIRECT_CSM_RS \
        "RootConstants(num32BitConstants=4, b0), CBV(b1), " \
        "DescriptorTable(SRV(t0, numDescriptors=25, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
        "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"
  #else
    #define SHADOW_INDIRECT_CSM_RS \
        "RootConstants(num32BitConstants=4, b0), CBV(b1), " \
        "DescriptorTable(SRV(t0, numDescriptors=7, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
  #endif
#elif VSM_TESSY
  // VSM_PAGE's tables with the cluster inputs after PageProj: TessyVisible, MegaVB, MegaIB, Clusters, Meshlets,
  // ClusterVerts, ClusterTris (and the masked table path's albedos after those). No input assembler; the VS and the MS (N6) take
  // the same table.
  #if SHADOW_MASKED && GBUFFER_BINDLESS
    #define SHADOW_INDIRECT_CSM_RS \
        "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), " \
        "DescriptorTable(SRV(t0, numDescriptors=11, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
        "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"
  #elif SHADOW_MASKED
    #define SHADOW_INDIRECT_CSM_RS \
        "DescriptorTable(SRV(t0, numDescriptors=27, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
        "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"
  #else
    #define SHADOW_INDIRECT_CSM_RS \
        "DescriptorTable(SRV(t0, numDescriptors=9, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
  #endif
#elif VSM_PAGE
  #if SHADOW_MASKED && GBUFFER_BINDLESS
    // t0 Instances, t1 CasterGroup, t2 GroupMask, t3 PageProj; the albedos by heap index.
    #define SHADOW_INDIRECT_CSM_RS \
        "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), " \
        "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
        "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"
  #elif SHADOW_MASKED
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
#elif SHADOW_MASKED && GBUFFER_BINDLESS
#define SHADOW_INDIRECT_CSM_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), " \
    "CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)"
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

#include "shadow_depth_common.hlsli"

// SHADOW_DEPTH_BIAS (S6) is its OWN permutation, not a runtime branch, and that is the whole point:
// the slope term needs the caster NORMAL, and fetching one costs +12 B per vertex on a path that
// also draws every VSM page. The VSM PSOs (both the VSM_PAGE single draw and the pool-format loop
// twins) are simply built without this define and keep the position-only input layout, so they pay
// EXACTLY nothing -- no fetch, no ALU, not even a branch. The plan had one shared layout plus a
// runtime gate; the permutation already existed, so this is strictly cheaper and provable by
// looking at which layout each PSO binds (ShadowGpuData::EnsurePipelines).
#ifndef SHADOW_DEPTH_BIAS
#define SHADOW_DEPTH_BIAS 0
#endif
#if SHADOW_DEPTH_BIAS
#define SHADOW_NORMAL_IN  float3 N : NORMAL;   // per-vertex, offset 12 of VertexPNTUV (slot 0)
#define SHADOW_VS_NORMAL(i) (i).N
#else
#define SHADOW_NORMAL_IN
#define SHADOW_VS_NORMAL(i) float3(0.0f, 0.0f, 1.0f)
#endif

// Local-light plan S4: SHADOW_LINEAR_DEPTH (with SHADOW_DEPTH_BIAS, never with VSM_PAGE) = the spot views' twin of the
// two atlas PSOs -- the pixel shader writes shadow_depth_common.hlsli's ShadowLinearDepth as SV_Depth.
#ifndef SHADOW_LINEAR_DEPTH
#define SHADOW_LINEAR_DEPTH 0
#endif
#if SHADOW_LINEAR_DEPTH
#define SHADOW_LINEAR_OUT float linearDepth : TEXCOORD3;
#else
#define SHADOW_LINEAR_OUT
#endif
#if TESSY_VIEW
#define TESSY_VIEW_OUT nointerpolation uint viewId : TEXCOORD5; float3 viewN : TEXCOORD6;
#else
#define TESSY_VIEW_OUT
#endif
#if TESSY_VIEW
// One colour per cluster (or meshlet): a hash of its index, kept off black and white, a little shading so the shapes read.
// Tessy N7e, "Levels" (bit 30 of the id): a DAG cluster by its level -- green (the leaves, LOD0's detail) through yellow
// and red to magenta (the coarsest) -- and a discrete LOD's cluster by its tier, light to dark blue.
float4 TessyViewShade(uint id, float3 n)
{
    const float lit = 0.55f + 0.45f * abs(dot(normalize(n), float3(0.36f, 0.86f, 0.36f)));
    if ((id & 0x40000000u) != 0u)
    {
        const uint code = id & 0xFFFFu;
        float3 c;
        if ((code & 0x100u) != 0u)
        {
            const float t = saturate(float(code & 0xFFu) / 6.0f); // level 0 .. 6+
            c = t < 0.5f ? lerp(float3(0.10f, 0.85f, 0.15f), float3(0.95f, 0.85f, 0.10f), t * 2.0f)
                         : lerp(float3(0.95f, 0.85f, 0.10f), float3(0.90f, 0.10f, 0.75f), t * 2.0f - 1.0f);
        }
        else
        {
            const float t = float(code & 3u) / 3.0f; // tier 0 .. 3
            c = lerp(float3(0.55f, 0.75f, 1.0f), float3(0.08f, 0.15f, 0.55f), t);
        }
        return float4(c * lit, 1.0f);
    }
    uint h = id * 0x9E3779B1u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    const float3 c = 0.12f + 0.75f * float3(h & 0xFFu, (h >> 8) & 0xFFu, (h >> 16) & 0xFFu) * (1.0f / 255.0f);
    return float4(c * lit, 1.0f);
}
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
                                float leafScaleValue, float4x4 vp, float4 w0, float4 w1,
                                float4 w2)
{
    float4 wp = mul(float4(objPos, 1.0f), world);
    float3 off = WindOffset(objPos, wp.xyz, float3(world._41, world._42, world._43), windStrengthValue,
                            windWeights, foliageValue, trunkStiffValue, leafScaleValue,
                            w0.zw, w1.x, w1.y, w1.z, w0.x);
    // w2 = (camPos.xyz, windFadeEnd). WORLD-distance sway falloff for the VSM pages (byte 224
    // of the page slot): sway fades to zero across [end/2, end], identically in EVERY clipmap
    // level, so two levels blended at a level boundary agree on the geometry by construction —
    // the hard per-level rigid gate alone put a full-amplitude mismatch straight into the
    // clipmap blend band. w2.w <= 0 = no falloff (the legacy CB path passes zero and keeps its
    // old full-sway behavior untouched).
    if (w2.w > 0.0f)
    {
        const float d = length(wp.xz - w2.xz);
        off *= 1.0f - smoothstep(0.5f * w2.w, w2.w, d);
    }
    wp.xyz += off;
    return mul(wp, vp);
}

#if VSM_PAGE

#include "vsm_addressing.hlsli" // VSM_POOL_PAGES_AXIS (no resource declarations in this header)

#if SHADOW_MASKED
StructuredBuffer<float4> PageProjRows : register(t3); // after CasterGroup/GroupMask, before the albedos
#else
StructuredBuffer<float4> PageProjRows : register(t1);
#endif

#if VSM_TESSY
#include "tessy_common.hlsli"
#if SHADOW_MASKED
StructuredBuffer<uint4>        TessyVisible : register(t4);
ByteAddressBuffer              MegaVB       : register(t5);
StructuredBuffer<uint>         MegaIB       : register(t6);
StructuredBuffer<TessyCluster> Clusters     : register(t7);
StructuredBuffer<uint4>        Meshlets     : register(t8); // N6
StructuredBuffer<uint>         ClusterVerts : register(t9);
StructuredBuffer<uint>         ClusterTris  : register(t10);
#else
StructuredBuffer<uint4>        TessyVisible : register(t2);
ByteAddressBuffer              MegaVB       : register(t3);
StructuredBuffer<uint>         MegaIB       : register(t4);
StructuredBuffer<TessyCluster> Clusters     : register(t5);
StructuredBuffer<uint4>        Meshlets     : register(t6); // N6
StructuredBuffer<uint>         ClusterVerts : register(t7);
StructuredBuffer<uint>         ClusterTris  : register(t8);
#endif
static const uint kTessyVertexStride = 52u; // sizeof(VertexPNTUV): position at 0, uv at 40, COLOR_0 at 48

struct TessyVertex
{
    float3 P;
    float2 UV;
    float4 WIND;
    uint   casterId; // the visible-list entry: caster id | page << kPageIdShift
    bool   live;
};

// The mega VB's vertex `index` (mesh-local + the mesh's base vertex) as the input assembler would deliver it.
void TessyLoadVertex(uint index, inout TessyVertex v)
{
    const uint b = index * kTessyVertexStride;
    v.P = asfloat(MegaVB.Load3(b));
    v.UV = asfloat(MegaVB.Load2(b + 40u));
    const uint c = MegaVB.Load(b + 48u); // R8G8B8A8_UNORM, as the input assembler reads it
    v.WIND = float4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24) * (1.0f / 255.0f);
}

// Instance `iid` = one surviving cluster (a TessyVisible record, tessy_common.hlsli); vertex `vid` of its 128 * 3.
// Past the record count (the cull's counter can overrun the buffer) or past the cluster's triangles the vertex is not
// live.
TessyVertex TessyFetch(uint vid, uint iid)
{
    TessyVertex v = (TessyVertex)0;
    uint count, stride;
    TessyVisible.GetDimensions(count, stride);
    if (iid >= count) { return v; }
    const uint4 rec = TessyVisible[iid];
    if (rec.x == kTessyDeadRecord) { return v; }
    const TessyCluster c = Clusters[rec.y];
    if (vid / 3u >= c.triCount) { return v; }
    TessyLoadVertex(MegaIB[c.firstIndex + vid] + rec.z, v);
    v.casterId = rec.x;
    v.live = true;
    return v;
}
#endif

// MUST match vsm::kPageIdShift in VirtualShadowMap.h (the CPU side asserts the page fits above it).
static const uint  kPageIdShift = 22u;
static const uint  kCasterMask  = (1u << kPageIdShift) - 1u;
static const float kPoolAxis    = (float)VSM_POOL_PAGES_AXIS;

// One page's 256-byte pageProj_ slot, viewed as 16 float4s: rows 0..3 = the off-center viewProj the
// setup CS built (vsm_page_setup_cs.hlsl stores pm[0..3] at bytes 0/16/32/48), elements 12 and 13 =
// the wind tail it copied to bytes 192/208. Bytes 64..191 are never written and never read.
float4x4 LoadPageVP(uint page, out float4 w0, out float4 w1, out float4 w2)
{
    const uint b = page * 16u;  // 256 B slot / 16 B per float4
    w0 = PageProjRows[b + 12u];
    w1 = PageProjRows[b + 13u];
    w2 = PageProjRows[b + 14u]; // (camPos.xyz, windFadeEnd) -- byte 224
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
    // S6: shadow-depth bias params, 224..239. Zero everywhere except a Legacy CSM cascade with
    // `depthBiasAtWrite` on; ApplyShadowDepthBias early-outs on zero.
    float  shadowConstBias;   // 224
    float  shadowSlopeBias;   // 228
    float  shadowMaxSlope;    // 232
    float  shadowClampNear;   // 236 (S7)
};

// S15 (SDSM) NEEDS NO PERMUTATION HERE, and that is deliberate. Its projection and bias ARE GPU
// results, but they arrive the same way Legacy's do: as this very cbuffer. The analyze pass writes
// a finished 256-byte PerView block per partition into a DEFAULT-heap buffer and the CPU binds
// that block's address as the b1 root CBV (SdsmShadows::ViewCbAddress). A permutation that read
// the matrix out of a StructuredBuffer existed first and was deleted: it cost a per-VERTEX load of
// a draw-uniform value, two more PSOs, a reshuffled descriptor table in the masked variant, and 16
// bytes on every PerView in the engine -- for a value D3D12 is perfectly happy to take as a CBV,
// because SDSM issues one ExecuteIndirect per partition and a root CBV is legal per draw.
// `linearDepth`: plan S4's spot depth (ShadowLinearDepth); read only by the SHADOW_LINEAR_DEPTH pixel shaders.
inline float4 WindTransformH(float3 objPos, float3 objNormal, float4x4 world, float4 windWeights,
                             float windStrengthValue, float foliageValue, float trunkStiffValue,
                             float leafScaleValue, out float linearDepth)
{
    const float4x4 vp = viewProj;
    const float4 bias = float4(shadowConstBias, shadowSlopeBias, shadowMaxSlope, shadowClampNear);
    const float4 H = WindTransformCore(objPos, world, windWeights, windStrengthValue, foliageValue,
                                       trunkStiffValue, leafScaleValue, vp,
                                       float4(windTime, windPrevTime, windDirXZ),
                                       float4(windSwayAmp, windSwayFreq, windGustMul, windPrevGustMul),
                                       float4(0.0f, 0.0f, 0.0f, 0.0f)); // no falloff on the legacy path
    linearDepth = 0.0f;
#if SHADOW_DEPTH_BIAS
    // S6. Wind has already moved the vertex; the normal is NOT re-oriented by the sway, exactly as
    // the gbuffer leaves it -- a swayed leaf's lighting normal and its shadow bias then agree.
    const float3 nWS = mul(objNormal, (float3x3)world);
#if SHADOW_LINEAR_DEPTH
    linearDepth = ShadowLinearDepth(H, nWS, vp, bias);
#endif
    return ApplyShadowDepthBias(H, nWS, vp, bias);
#else
    return H;
#endif
}

#if TESSY_ATLAS_MS
#include "tessy_common.hlsli"
// Tessy N4: the atlas view's surviving meshlets (tessy_atlas_cull_cs.hlsl). The records of every view lie in one buffer,
// each view's run at TessyAtlasViewInfo[its row] = {first record, records}.
#if SHADOW_MASKED
StructuredBuffer<uint4> TessyVisible       : register(t3);
ByteAddressBuffer       MegaVB             : register(t4);
StructuredBuffer<uint4> Meshlets           : register(t5);
StructuredBuffer<uint>  ClusterVerts       : register(t6);
StructuredBuffer<uint>  ClusterTris        : register(t7);
StructuredBuffer<uint2> TessyAtlasViewInfo : register(t8);
#else
StructuredBuffer<uint4> TessyVisible       : register(t1);
ByteAddressBuffer       MegaVB             : register(t2);
StructuredBuffer<uint4> Meshlets           : register(t3);
StructuredBuffer<uint>  ClusterVerts       : register(t4);
StructuredBuffer<uint>  ClusterTris        : register(t5);
StructuredBuffer<uint2> TessyAtlasViewInfo : register(t6);
#endif
cbuffer TessyAtlasRoot : register(b0)
{
    uint gTessyAtlasView; // this view's row of TessyAtlasViewInfo (ShadowGpuData::RecordIndirectShadowDraws)
    uint gTessyAtlasFlags; // kTessyMsCullSmall (tessy.msCull); 0 for the cluster view
    float2 gTessyAtlasViewport; // the view's viewport in texels (a face, a spot, an SDSM partition's content rect)
};
#endif

#endif // VSM_PAGE

#if SHADOW_MASKED

StructuredBuffer<uint>  CasterGroup : register(t1); // per-caster mesh-group id (region 0, static)
StructuredBuffer<uint2> GroupMask   : register(t2); // per group: x = albedo slot in gMaskAlbedo, or its bindless heap index (~0 = opaque), y = asuint(alphaCutoff)
#if GBUFFER_BINDLESS
// A6: the albedo is ResourceDescriptorHeap[GroupMask.x]; no table.
#elif TESSY_ATLAS_MS
Texture2D    gMaskAlbedo[16] : register(t9);        // after the atlas view's six cluster inputs
#elif VSM_TESSY
Texture2D    gMaskAlbedo[16] : register(t11);       // after PageProj and the seven cluster inputs
#elif VSM_PAGE
Texture2D    gMaskAlbedo[16] : register(t4);        // t3 is PageProj in this permutation
#else
Texture2D    gMaskAlbedo[16] : register(t3);        // masked groups' albedo textures (alpha channel)
#endif
SamplerState gMaskSmp        : register(s0);

struct VSInMasked
{
    float3 P        : POSITION;  // per-vertex (slot 0)
    SHADOW_NORMAL_IN             // S6, only in the SHADOW_DEPTH_BIAS permutation
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
    nointerpolation float baseAlpha : TEXCOORD4; // local-light plan S8: the slot's baseColor.a, as the G-buffer's clip
    SHADOW_LINEAR_OUT                          // plan S4
    TESSY_VIEW_OUT
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
    nointerpolation float baseAlpha : TEXCOORD4;
    SHADOW_LINEAR_OUT
    TESSY_VIEW_OUT
};

VSOutMasked MaskedVS(VSInMasked i)
{
    VSOutMasked o;
#if VSM_PAGE
    const uint page = i.casterId >> kPageIdShift;
    const uint cid  = i.casterId & kCasterMask;
    const InstancePerObject ip = Instances[cid];
    float4 w0, w1, w2;
    const float4x4 vp = LoadPageVP(page, w0, w1, w2);
    const float4 hLocal = WindTransformCore(i.P, ip.world, i.WIND, ip.windStrength, ip.windFoliage,
                                            ip.windTrunkStiff, ip.windLeafScale, vp, w0, w1, w2);
    PagePlace(page, hLocal, o.H, o.CD);
#else
    const uint cid = i.casterId;
    const InstancePerObject ip = Instances[cid];
    float linearDepth;
    o.H = WindTransformH(i.P, SHADOW_VS_NORMAL(i), ip.world, i.WIND, ip.windStrength, ip.windFoliage,
                         ip.windTrunkStiff, ip.windLeafScale, linearDepth);
#if SHADOW_LINEAR_DEPTH
    o.linearDepth = linearDepth;
#endif
#endif
    // Local-light plan S8: the G-buffer's own clip (gbuffer_common.hlsli AlphaTestClip) -- the slot's UV transform and
    // baseColor.a, which the caster record carries per submesh (FillInstanceDataForSlot). The raw UV and alpha cast
    // every frond thicker than the leaf the camera sees; the CPU path, made masked in S8, showed it (1.5 % of the frame
    // under local_shadow_test's spot B).
    o.UV = i.UV * ip.texOffsScale.zw + ip.texOffsScale.xy;
    o.baseAlpha = ip.baseColor.a;
    const uint2 gm = GroupMask[CasterGroup[cid]];
    o.texSlot = gm.x;
    o.cutoff = asfloat(gm.y);
    return o;
}

#if VSM_TESSY
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
VSOutMasked VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const TessyVertex tv = TessyFetch(vid, iid);
    if (!tv.live)
    {
        VSOutMasked dead = (VSOutMasked)0;
        dead.H = float4(0.0f, 0.0f, 0.0f, 1.0f);
        dead.CD = float4(-1.0f, -1.0f, -1.0f, -1.0f); // clipped whole: a degenerate that never reaches the PS
        dead.texSlot = 0xFFFFFFFFu;
        return dead;
    }
    VSInMasked i;
    i.P = tv.P;
    i.UV = tv.UV;
    i.WIND = tv.WIND;
    i.casterId = tv.casterId;
    return MaskedVS(i);
}
#else
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
VSOutMasked VSMain(VSInMasked i)
{
    return MaskedVS(i);
}
#endif

void MaskedClip(PSInMasked i)
{
    if (i.texSlot == 0xFFFFFFFFu) { return; } // opaque group — draw-uniform branch, no sample
#if GBUFFER_BINDLESS
    Texture2D maskAlbedo = ResourceDescriptorHeap[NonUniformResourceIndex(i.texSlot)];
    const float a = maskAlbedo.Sample(gMaskSmp, i.UV).a;
#else
    const float a = gMaskAlbedo[NonUniformResourceIndex(i.texSlot)].Sample(gMaskSmp, i.UV).a;
#endif
    ShadowAlphaClip(a * i.baseAlpha, i.cutoff, i.H.xy); // glass: a dithered share (shadow_depth_common.hlsli)
}

#if TESSY_VIEW
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
float4 PSMain(PSInMasked i) : SV_Target
{
    MaskedClip(i);
    return TessyViewShade(i.viewId, i.viewN);
}
#elif SHADOW_LINEAR_DEPTH
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
float PSMain(PSInMasked i) : SV_Depth
{
    MaskedClip(i);
    return saturate(i.linearDepth);
}
#else
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
void PSMain(PSInMasked i)
{
    MaskedClip(i);
}
#endif

#else // opaque (null-work PS) variant

struct VSInIndirect
{
    float3 P        : POSITION;  // per-vertex, from the mesh vertex buffer (slot 0)
    SHADOW_NORMAL_IN             // S6, only in the SHADOW_DEPTH_BIAS permutation
    float4 WIND     : COLOR0;    // per-vertex, offset 48 — the W7.2 baked wind weights
    uint   casterId : CASTERID;  // per-instance, from the visible-list stream (slot 1)
};
struct VSOutD
{
    float4 H : SV_POSITION;
    SHADOW_LINEAR_OUT // plan S4
    TESSY_VIEW_OUT
#if VSM_PAGE
    float4 CD : SV_ClipDistance0; // the page's four borders (replaces the per-page scissor)
#endif
};
struct PSInD { float4 H : SV_POSITION; SHADOW_LINEAR_OUT TESSY_VIEW_OUT }; // the PS never reads CD (see the masked variant)

VSOutD OpaqueVS(VSInIndirect i)
{
    VSOutD o;
#if VSM_PAGE
    const uint page = i.casterId >> kPageIdShift;
    const uint cid  = i.casterId & kCasterMask;
    const InstancePerObject ip = Instances[cid];
    float4 w0, w1, w2;
    const float4x4 vp = LoadPageVP(page, w0, w1, w2);
    const float4 hLocal = WindTransformCore(i.P, ip.world, i.WIND, ip.windStrength, ip.windFoliage,
                                            ip.windTrunkStiff, ip.windLeafScale, vp, w0, w1, w2);
    PagePlace(page, hLocal, o.H, o.CD);
#else
    const InstancePerObject ip = Instances[i.casterId];
    float linearDepth;
    o.H = WindTransformH(i.P, SHADOW_VS_NORMAL(i), ip.world, i.WIND, ip.windStrength, ip.windFoliage,
                         ip.windTrunkStiff, ip.windLeafScale, linearDepth);
#if SHADOW_LINEAR_DEPTH
    o.linearDepth = linearDepth;
#endif
#endif
    return o;
}

#if VSM_TESSY
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
VSOutD VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const TessyVertex tv = TessyFetch(vid, iid);
    if (!tv.live)
    {
        VSOutD dead = (VSOutD)0;
        dead.H = float4(0.0f, 0.0f, 0.0f, 1.0f);
        dead.CD = float4(-1.0f, -1.0f, -1.0f, -1.0f);
        return dead;
    }
    VSInIndirect i;
    i.P = tv.P;
    i.WIND = tv.WIND;
    i.casterId = tv.casterId;
    return OpaqueVS(i);
}
#else
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
VSOutD VSMain(VSInIndirect i)
{
    return OpaqueVS(i);
}
#endif

#if TESSY_VIEW
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
float4 PSMain(PSInD i) : SV_Target { return TessyViewShade(i.viewId, i.viewN); }
#elif SHADOW_LINEAR_DEPTH
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
float PSMain(PSInD i) : SV_Depth { return saturate(i.linearDepth); }
#else
// Depth-only — empty pixel shader.
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
void PSMain(PSInD i) { }
#endif

#endif // SHADOW_MASKED

#if VSM_TESSY_MS
// Tessy N6: the cluster draw as a mesh shader. Group (x, y) is TessyVisible record y * kTessyMsRow + x (the dispatch
// tessy_ms_args_cs.hlsl wrote), one MESHLET of a surviving cluster (the cull appends a record per meshlet): its unique
// vertices, each swayed and placed in the page ONCE -- one a thread, <= 64 -- and its triangles as three 8-bit places
// (two passes of the group), with the VS path's vertex math (WindTransformCore, PagePlace, the masked attributes as
// MaskedVS writes them). The caster's instance, its page and its mask are the group's. A triangle whose three corners
// lie outside the same page border is culled here (SV_CullPrimitive): the clipper would have thrown it away whole (clip
// distances interpolate linearly), after setting it up.
// 64 vertices, not the cluster's 256 (Mesh::kMeshletMaxVerts): what an SM holds of this shader at once is set by the
// DECLARED output (256 x 52 bytes masked), and at 256 it was slower than the VS path -- 0.34 of its 0.39 ms on the
// grove with every triangle culled, i.e. the shader, not the raster. 128 vertices: 0.078 ms per million triangles,
// 64: 0.072, 64 on 64 threads: 0.063 (the VS path 0.12; 2026-10-29).
#if SHADOW_MASKED
typedef VSOutMasked TessyMsVertex;
#else
typedef VSOutD TessyMsVertex;
#endif
struct TessyMsPrimitive
{
    bool cull : SV_CullPrimitive;
};

groupshared uint gsTessyOutside[64]; // per meshlet vertex: bit i = outside page border i (CD[i] < 0), bit 4 = w <= 0
groupshared float2 gsTessyPx[64];    // per meshlet vertex: its page-local pixel position (x right, y down)
static const float kTessyPageTexels = VSM_VIRTUAL_RES / (float)VSM_L0_AXIS; // a page's edge in texels (128)
// The small-triangle cull's margin: the rasterizer snaps to 1/256 px and computes the pool position in its own float
// math (~0.002 px at a 16k pool); 1/64 px keeps the cull conservative.
static const float kTessyPxMargin = 1.0f / 64.0f;

static const uint kTessyMsThreads = 64u; // = kTessyMeshletMaxVerts: one a vertex

[NumThreads(64, 1, 1)]
[OutputTopology("triangle")]
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
void MSMain(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex,
            out vertices TessyMsVertex verts[64],
            out indices uint3 tris[128],
            out primitives TessyMsPrimitive prims[128])
{
    uint cap, stride;
    TessyVisible.GetDimensions(cap, stride);
    const uint iid = gid.y * kTessyMsRow + gid.x;
    // No branch and no early return anywhere around SetMeshOutputCounts: DXC's ms_6_6 tail-duplicated the call into
    // both arms of one and failed validation ("cannot be called multiple times"). A dead group (past the count, or a
    // dead record) emits 0 and 0, and every `lane <` test below is then false.
    const uint4 rec = TessyVisible[min(iid, cap - 1u)];
    const bool live = and(iid < cap, rec.x != kTessyDeadRecord);
    const uint4 m = Meshlets[select(live, rec.y, 0u)]; // {vertex offset, vertices, triangle offset, triangles}
    const uint vertCount = select(live, min(m.y, kTessyMeshletMaxVerts), 0u);
    const uint triCount = select(live, min(m.w, kTessyMeshletMaxTris), 0u);
    SetMeshOutputCounts(vertCount, triCount); // uniform: one record per group

    const uint page = select(live, rec.x >> kPageIdShift, 0u);
    const uint cid  = select(live, rec.x & kCasterMask, 0u);
    const InstancePerObject ip = Instances[cid];
    float4 w0, w1, w2;
    const float4x4 vp = LoadPageVP(page, w0, w1, w2);
#if SHADOW_MASKED
    const uint2 gm = GroupMask[CasterGroup[cid]];
#endif
    if (lane < vertCount)
    {
        TessyVertex tv = (TessyVertex)0;
        TessyLoadVertex(ClusterVerts[m.x + lane] + rec.z, tv);
        const float4 hLocal = WindTransformCore(tv.P, ip.world, tv.WIND, ip.windStrength, ip.windFoliage,
                                                ip.windTrunkStiff, ip.windLeafScale, vp, w0, w1, w2);
        TessyMsVertex o = (TessyMsVertex)0;
        PagePlace(page, hLocal, o.H, o.CD);
#if SHADOW_MASKED
        o.UV = tv.UV * ip.texOffsScale.zw + ip.texOffsScale.xy;
        o.baseAlpha = ip.baseColor.a;
        o.texSlot = gm.x;
        o.cutoff = asfloat(gm.y);
#endif
        verts[lane] = o;
        gsTessyOutside[lane] = (o.CD.x < 0.0f ? 1u : 0u) | (o.CD.y < 0.0f ? 2u : 0u) |
                               (o.CD.z < 0.0f ? 4u : 0u) | (o.CD.w < 0.0f ? 8u : 0u) | (hLocal.w <= 0.0f ? 16u : 0u);
        // The page's own [-1, 1] mapped to its 128 texels: the same pixel grid as the pool's (a page starts on a whole
        // texel), with the precision of a 128-texel range.
        const float2 ndc = hLocal.xy / max(hLocal.w, 1.0e-20f);
        gsTessyPx[lane] = float2(ndc.x + 1.0f, 1.0f - ndc.y) * (0.5f * kTessyPageTexels);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = lane; k < triCount; k += kTessyMsThreads)
    {
        const uint t = ClusterTris[m.z + k];
        const uint3 tri = min(uint3(t & 0xFFu, (t >> 8) & 0xFFu, (t >> 16) & 0xFFu), vertCount - 1u);
        tris[k] = tri;
        const uint o0 = gsTessyOutside[tri.x], o1 = gsTessyOutside[tri.y], o2 = gsTessyOutside[tri.z];
        bool cull = (o0 & o1 & o2 & 15u) != 0u;
        // A triangle that holds no pixel centre (k + 0.5) in its box rasterizes no sample: tiny and thin triangles.
        // Not across the light's plane (w <= 0: no screen box).
        if (!cull && (rec.w & kTessyMsCullSmall) != 0u && ((o0 | o1 | o2) & 16u) == 0u)
        {
            const float2 p0 = gsTessyPx[tri.x], p1 = gsTessyPx[tri.y], p2 = gsTessyPx[tri.z];
            const float2 lo = min(p0, min(p1, p2)) - kTessyPxMargin;
            const float2 hi = max(p0, max(p1, p2)) + kTessyPxMargin;
            cull = any(ceil(lo - 0.5f) > floor(hi - 0.5f));
        }
        TessyMsPrimitive p;
        p.cull = cull;
        prims[k] = p;
    }
}
#endif // VSM_TESSY_MS

#if TESSY_ATLAS_MS
// Tessy N4: an atlas view's surviving meshlets as a mesh shader -- VSM_TESSY_MS's group (one meshlet, a vertex a thread)
// with the atlas VS's own vertex math: WindTransformH (the view's PerView: its matrix, the wind or none for a rigid
// light, S6's slope bias from the vertex normal, S4's linear depth) and MaskedVS's attributes. The face's viewport and
// scissor clip it, as they clip the classic draw. Group (x, y) is record y * kTessyMsRow + x of the view's run. A
// triangle whose box holds no texel centre of the view's viewport rasterizes no sample and is culled (VSM_TESSY_MS's
// test; the viewport starts on a whole texel, so its own grid is the atlas's).
#if SHADOW_MASKED
typedef VSOutMasked TessyAtlasVertex;
#else
typedef VSOutD TessyAtlasVertex;
#endif
struct TessyAtlasPrimitive
{
    bool cull : SV_CullPrimitive;
};
groupshared float2 gsAtlasPx[64];   // per meshlet vertex: its viewport position in texels (x right, y down)
groupshared bool gsAtlasBehind[64]; // w <= 0: no position
static const float kTessyAtlasPxMargin = 1.0f / 64.0f; // the rasterizer snaps to 1/256 px: conservative

[NumThreads(64, 1, 1)]
[OutputTopology("triangle")]
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
void MSMain(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex,
            out vertices TessyAtlasVertex verts[64],
            out indices uint3 tris[128],
            out primitives TessyAtlasPrimitive prims[128])
{
    const uint2 run = TessyAtlasViewInfo[gTessyAtlasView & 0x3FFFFFFFu]; // {first record, records}; bits 30-31: TESSY_VIEW's
    uint cap, stride;
    TessyVisible.GetDimensions(cap, stride);
    const uint i = gid.y * kTessyMsRow + gid.x;
    // Selects up to SetMeshOutputCounts, never a branch (see VSM_TESSY_MS's MSMain: DXC's ms_6_6 duplicates the call).
    const bool live = and(i < run.y, run.x + i < cap);
    const uint4 rec = TessyVisible[select(live, run.x + i, 0u)]; // {caster id, meshlet, base vertex, 0}
    const uint4 m = Meshlets[select(live, rec.y, 0u)];
    const uint vertCount = select(live, min(m.y, kTessyMeshletMaxVerts), 0u);
    const uint triCount = select(live, min(m.w, kTessyMeshletMaxTris), 0u);
    SetMeshOutputCounts(vertCount, triCount);

    const uint cid = select(live, rec.x, 0u);
    const InstancePerObject ip = Instances[cid];
#if SHADOW_MASKED
    const uint2 gm = GroupMask[CasterGroup[cid]];
#endif
    if (lane < vertCount)
    {
        const uint b = (ClusterVerts[m.x + lane] + rec.z) * 52u; // sizeof(VertexPNTUV): normal at 12, uv 40, COLOR_0 48
        const float3 P = asfloat(MegaVB.Load3(b));
        const float3 N = asfloat(MegaVB.Load3(b + 12u));
        const uint c = MegaVB.Load(b + 48u);
        const float4 wind = float4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24) * (1.0f / 255.0f);
        TessyAtlasVertex o = (TessyAtlasVertex)0;
        float linearDepth;
        o.H = WindTransformH(P, N, ip.world, wind, ip.windStrength, ip.windFoliage, ip.windTrunkStiff,
                             ip.windLeafScale, linearDepth);
#if SHADOW_LINEAR_DEPTH
        o.linearDepth = linearDepth;
#endif
#if SHADOW_MASKED
        o.UV = asfloat(MegaVB.Load2(b + 40u)) * ip.texOffsScale.zw + ip.texOffsScale.xy;
        o.baseAlpha = ip.baseColor.a;
        o.texSlot = gm.x;
        o.cutoff = asfloat(gm.y);
#endif
#if TESSY_VIEW
        // The meshlet (bit 31), the cluster's level (bit 30, tessy.debug:3: the cull wrote it in .w's high half), or the
        // cluster (its vg | index).
        o.viewId = (gTessyAtlasView & 0x80000000u) != 0u ? rec.y
                 : (gTessyAtlasView & 0x40000000u) != 0u ? (0x40000000u | (rec.w >> 16)) : rec.w;
        o.viewN = mul(N, (float3x3)ip.world);
#endif
        verts[lane] = o;
        gsAtlasBehind[lane] = o.H.w <= 0.0f;
        const float2 ndc = o.H.xy / max(o.H.w, 1.0e-20f);
        gsAtlasPx[lane] = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * gTessyAtlasViewport;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = lane; k < triCount; k += 64u)
    {
        const uint t = ClusterTris[m.z + k];
        const uint3 tri = min(uint3(t & 0xFFu, (t >> 8) & 0xFFu, (t >> 16) & 0xFFu), vertCount - 1u);
        tris[k] = tri;
        bool cull = false;
        if ((gTessyAtlasFlags & kTessyMsCullSmall) != 0u &&
            !(gsAtlasBehind[tri.x] || gsAtlasBehind[tri.y] || gsAtlasBehind[tri.z]))
        {
            const float2 p0 = gsAtlasPx[tri.x], p1 = gsAtlasPx[tri.y], p2 = gsAtlasPx[tri.z];
            const float2 lo = min(p0, min(p1, p2)) - kTessyAtlasPxMargin;
            const float2 hi = max(p0, max(p1, p2)) + kTessyAtlasPxMargin;
            cull = any(ceil(lo - 0.5f) > floor(hi - 0.5f));
        }
        TessyAtlasPrimitive p;
        p.cull = cull;
        prims[k] = p;
    }
}
#endif // TESSY_ATLAS_MS
