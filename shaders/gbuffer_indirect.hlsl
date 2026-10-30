// Occlusion plan S4: the GPU-driven G-buffer. One ExecuteIndirect per virtual group (mesh submesh
// x camera LOD); every instance of the draw is a caster id the input assembler delivers from the
// camera visible list (slot 1, per-instance -- the shadow_indirect_csm.hlsl shape), and the
// per-object data comes out of the registry's per-caster record (t4, the SAME buffer the shadow
// VS reads). The pixel shader is gbuffer.hlsl's line for line; the per-object values it used to
// read from `cbuffer PerObject` (b0) it reads from that record itself, by the caster id the VS
// passes (one flat uint) -- 2026-10-23: the VS used to carry them as five flat float4 outputs (~42
// floats per vertex with the shared ones), and at foliage triangle sizes the pass was bound by its
// outputs: GBuffer_Indirect 0.38-0.40 -> 0.30-0.31 ms on wind_test. Dropping the unjittered clip
// position as well (rebuilt from SV_Position) measured nothing more and was not kept. The instance
// buffer therefore rests PIXEL | NON_PIXEL (ShadowGpuData kInstancesReadState).
//
// Parity with the CPU path (RenderableObject::Render / GBufferRenderable::Render) is the whole
// point: same BaseVS, same FetchShadingValuesP/FinalizeGBuffer, same wind, same dithered LOD
// crossfade -- the fade of THIS draw is (groupLod == the caster's tier ? -f : +f), which is what
// the CPU path writes into lodFade for its two draws of a fading object.
//
// b0: IndirectDraw (the virtual group's LOD)   b1: PerView (gbuffer_common)   b2: SurfaceParams
// t0..t3: the group's material textures (t3 = Part E's emissive map)   t4: InstancePerObject[]
// t5: caster fade[]   t6: caster camera LOD[]   s0: the material sampler
#pragma pack_matrix(row_major)
#define GBUFFER_SKIP_PEROBJECT
#include "gbuffer_common.hlsli"

// A6: GBUFFER_BINDLESS=1 -- the material textures come through ResourceDescriptorHeap[] by the
// indices SurfaceParams carries, and b0 (the group LOD) is ROOT CONSTANTS instead of a CBV: that is
// what lets one ExecuteIndirect span every group -- the command signature writes the constants and
// the b2 address per command ({VBV, IBV, CBV(b2), CONSTANT(b0), DRAW_INDEXED}).
#ifndef GBUFFER_BINDLESS
#define GBUFFER_BINDLESS 0
#endif

// Tessy N5 (docs/tessy_plan.md): TESSY_GBUFFER_MS=1 is the mesh shader twin of a merged run's PSO (the same desc: RTs,
// ALPHA_TEST, cull mode, shading model, MR / normal layouts) drawing the camera's surviving CLUSTERS -- the meshlet
// records tessy_atlas_cull_cs wrote into the camera row set, one DispatchMesh per run. One dispatch spans every group of
// the run, so what the merged commands set per draw comes per record instead: the virtual group in the record's .w
// gives the LOD (the crossfade's side) and the group's SurfaceParams (a per-frame buffer, t13, in place of b2), and the
// textures are indexed non-uniformly. Bindless runs only.
#ifndef TESSY_GBUFFER_MS
#define TESSY_GBUFFER_MS 0
#endif
#if TESSY_GBUFFER_MS && !GBUFFER_BINDLESS
#error "TESSY_GBUFFER_MS twins the bindless permutation only"
#endif

#if !GBUFFER_BINDLESS
Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1); // R=metal, G=rough
Texture2D gNormalMap : register(t2); // tangent-space, +Z
Texture2D gEmissiveMap : register(t3); // Part E: read only when texIndices.w says there is one
#endif
StructuredBuffer<InstancePerObject> gInstances : register(t4);
StructuredBuffer<float> gCasterFade : register(t5); // > 0: the caster fades between lod and lod + 1
StructuredBuffer<uint>  gCasterLod  : register(t6); // the caster's camera tier (bit 7 = chunk EXACT)
SamplerState gSmp : register(s0);

#if TESSY_GBUFFER_MS
#include "tessy_common.hlsli"

cbuffer TessyGBufferDraw : register(b0) // root constants, per run
{
    uint gTessyRow;        // the run's row of TessyAtlasViewInfo (row set 2)
    uint gTessyFlags;      // kTessyMsCullSmall
    float2 gTessyViewport; // the render target in pixels (the small-triangle cull's grid)
};

// SurfaceParams (b2 of the other permutations) as a per-frame buffer by mesh group: MaterialSurfaceParamsGpu.
struct SurfaceParamsData
{
    float3 subsurfaceColor;
    float transmissionStrength;
    float ambientOcclusion;
    float indirectSpecularScale;
    float transmissionAlbedoPower;
    float transmissionNormalWeight;
    float4 terrainTiling;
    float4 terrainEdgeParams;
    uint4 texIndices;
};
StructuredBuffer<uint4>            TessyVisible       : register(t7);  // {caster id, meshlet, base vertex, vg | ... << 16}
ByteAddressBuffer                  MegaVB             : register(t8);  // VertexPNTUV, 52 bytes
StructuredBuffer<uint4>            Meshlets           : register(t9);
StructuredBuffer<uint>             ClusterVerts       : register(t10);
StructuredBuffer<uint>             ClusterTris        : register(t11);
StructuredBuffer<uint2>            TessyAtlasViewInfo : register(t12); // per row: {first record, records}
StructuredBuffer<SurfaceParamsData> GroupSurface      : register(t13);

// The pixel shader's SurfaceParams under their b2 names: per pixel from its group (one dispatch, many materials).
static float3 subsurfaceColor;
static float transmissionStrength;
static float ambientOcclusion;
static float indirectSpecularScale;
static float transmissionAlbedoPower;
static float transmissionNormalWeight;
static float4 terrainTiling;
static float4 terrainEdgeParams;
static uint4 texIndices;
void LoadSurfaceParams(uint group)
{
    const SurfaceParamsData s = GroupSurface[group];
    subsurfaceColor = s.subsurfaceColor;
    transmissionStrength = s.transmissionStrength;
    ambientOcclusion = s.ambientOcclusion;
    indirectSpecularScale = s.indirectSpecularScale;
    transmissionAlbedoPower = s.transmissionAlbedoPower;
    transmissionNormalWeight = s.transmissionNormalWeight;
    terrainTiling = s.terrainTiling;
    terrainEdgeParams = s.terrainEdgeParams;
    texIndices = s.texIndices;
}
static const uint kTessyVgLods = 4u; // render::kMaxShadowLods: virtual group = mesh group * 4 + LOD
#else
cbuffer IndirectDraw : register(b0)
{
    uint gGroupLod;   // the LOD of the virtual group this ExecuteIndirect draws
    uint3 gIndirectPad;
};

cbuffer SurfaceParams : register(b2)
{
    float3 subsurfaceColor;
    float transmissionStrength;
    float ambientOcclusion;
    float indirectSpecularScale;
    float transmissionAlbedoPower;
    float transmissionNormalWeight;
    float4 terrainTiling;
    float4 terrainEdgeParams;
    uint4 texIndices; // A6: albedo, MR, normal slots in the bindless heap; w = the emissive map's (kNoEmissiveMap = none)
};
#endif

#if TESSY_GBUFFER_MS
// b0 the run's root constants, b1 PerView, one table t4..t13 (the records, fades and LODs the classic draws read, the
// cluster tables, the run's row, the groups' SurfaceParams), the material sampler.
#define GBUFFER_INDIRECT_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(num32BitConstants=4, b0)," \
    "CBV(b1)," \
    "DescriptorTable(SRV(t4, numDescriptors=10, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#elif GBUFFER_BINDLESS
#define GBUFFER_INDIRECT_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(num32BitConstants=4, b0)," \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(SRV(t4, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#else
#define GBUFFER_INDIRECT_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0)," \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(SRV(t4, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#endif

static const uint kCasterLodMask = 0x0Fu;

struct VSInIndirect
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT;
    float2 UV : TEXCOORD0;
    float4 WIND : COLOR0;
    uint casterId : CASTERID; // slot 1, per instance: the camera visible list
};

// VSOut plus what the PS needs to find the object's material values: its caster id, and this draw's crossfade.
struct VSOutIndirect
{
    VSOut b;
    nointerpolation uint casterId : TEXCOORD6;
    nointerpolation float lodFade : TEXCOORD7;
#if TESSY_GBUFFER_MS
    nointerpolation uint surface : TEXCOORD8; // the mesh group: its SurfaceParams (GroupSurface)
#endif
};

#if TESSY_GBUFFER_MS
// One group per MESHLET record of the run (Group (x, y) = record y * kTessyMsRow + x), a vertex a thread: BaseVS on the
// vertex fetched from the mega VB (the classic VS's math to the bit: the same matrices, wind and previous position), the
// crossfade's side from the record's LOD. A triangle whose screen box holds no pixel centre rasterizes no sample and is
// culled here (shadow_indirect_csm.hlsl VSM_TESSY_MS's test on the render target's grid); the rasterizer culls the
// back faces as the run's PSO says. The output is VSOutIndirect, 25 values a vertex: a 19-value one (the unjittered
// position rebuilt from SV_Position, the object id and the crossfade in the pixel shader) measured the same on the
// grove and the tunnel (2026-10-30) -- this draw is not bound by its output.
struct TessyGBufferPrim
{
    bool cull : SV_CullPrimitive;
};
groupshared float2 gsTessyPx[64];  // per meshlet vertex: its pixel position (x right, y down)
groupshared bool gsTessyBehind[64]; // w <= 0: no screen position
static const float kTessyPxMargin = 1.0f / 64.0f; // the rasterizer snaps to 1/256 px: conservative

[NumThreads(64, 1, 1)]
[OutputTopology("triangle")]
[RootSignature(GBUFFER_INDIRECT_RS)]
void MSMain(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex,
            out vertices VSOutIndirect verts[64],
            out indices uint3 tris[128],
            out primitives TessyGBufferPrim prims[128])
{
    const uint2 run = TessyAtlasViewInfo[gTessyRow];
    uint cap, stride;
    TessyVisible.GetDimensions(cap, stride);
    const uint i = gid.y * kTessyMsRow + gid.x;
    // Selects up to SetMeshOutputCounts, never a branch (DXC's ms_6_6 duplicates the call into both arms of one).
    const bool live = and(i < run.y, run.x + i < cap);
    const uint4 rec = TessyVisible[select(live, run.x + i, 0u)];
    const uint4 m = Meshlets[select(live, rec.y, 0u)]; // {vertex offset, vertices, triangle offset, triangles}
    const uint vertCount = select(live, min(m.y, kTessyMeshletMaxVerts), 0u);
    const uint triCount = select(live, min(m.w, kTessyMeshletMaxTris), 0u);
    SetMeshOutputCounts(vertCount, triCount);

    const uint cid = select(live, rec.x, 0u);
    const uint vg = rec.w & kTessyCamVgMask;
    const InstancePerObject o = gInstances[cid];
    // The crossfade, as VSMain: a fading caster sits in its tier's group (-f) and the next one's (+f). A cluster of the
    // mesh's DAG (N7d) is drawn once, whole: its cut changes cluster by cluster, never by tiers.
    const float f = (rec.w & kTessyCamDagBit) != 0u ? 0.0f : gCasterFade[cid];
    const uint lod = gCasterLod[cid] & kCasterLodMask;
    const float lodFade = (f > 0.0f) ? (((vg % kTessyVgLods) == lod) ? -f : f) : 0.0f;
    if (lane < vertCount)
    {
        const uint b = (ClusterVerts[m.x + lane] + rec.z) * 52u; // VertexPNTUV: N at 12, T 24, UV 40, COLOR_0 48
        const float3 P = asfloat(MegaVB.Load3(b));
        const float3 N = asfloat(MegaVB.Load3(b + 12u));
        const float4 T = asfloat(MegaVB.Load4(b + 24u));
        const float2 UV = asfloat(MegaVB.Load2(b + 40u));
        const uint c = MegaVB.Load(b + 48u);
        const float4 wind = float4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24) * (1.0f / 255.0f);
        VSOutIndirect v;
        v.b = BaseVS(P, o.world, o.prevWorld, viewProj, N, T, UV, o.objectId,
                     wind, o.windStrength, o.windFoliage, o.windTrunkStiff, o.windLeafScale);
        v.casterId = cid;
        v.lodFade = lodFade;
        v.surface = vg / kTessyVgLods;
        verts[lane] = v;
        const float4 h = v.b.H;
        gsTessyBehind[lane] = h.w <= 0.0f;
        const float2 ndc = h.xy / max(h.w, 1.0e-20f);
        gsTessyPx[lane] = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * gTessyViewport;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = lane; k < triCount; k += 64u)
    {
        const uint t = ClusterTris[m.z + k];
        const uint3 tri = min(uint3(t & 0xFFu, (t >> 8) & 0xFFu, (t >> 16) & 0xFFu), vertCount - 1u);
        tris[k] = tri;
        bool cull = false;
        if ((gTessyFlags & kTessyMsCullSmall) != 0u && !(gsTessyBehind[tri.x] || gsTessyBehind[tri.y] || gsTessyBehind[tri.z]))
        {
            const float2 p0 = gsTessyPx[tri.x], p1 = gsTessyPx[tri.y], p2 = gsTessyPx[tri.z];
            const float2 lo = min(p0, min(p1, p2)) - kTessyPxMargin;
            const float2 hi = max(p0, max(p1, p2)) + kTessyPxMargin;
            cull = any(ceil(lo - 0.5f) > floor(hi - 0.5f));
        }
        TessyGBufferPrim p;
        p.cull = cull;
        prims[k] = p;
    }
}
#else
[RootSignature(GBUFFER_INDIRECT_RS)]
VSOutIndirect VSMain(VSInIndirect i)
{
    const InstancePerObject o = gInstances[i.casterId];
    VSOutIndirect v;
    v.b = BaseVS(i.P, o.world, o.prevWorld, viewProj, i.N, i.T, i.UV, o.objectId,
                 i.WIND, o.windStrength, o.windFoliage, o.windTrunkStiff, o.windLeafScale);
    v.casterId = i.casterId;
    // The crossfade: a fading caster sits in two buckets, its own tier (fading OUT, -f) and the
    // next (fading IN, +f) -- RenderableObject::Render's pass 0 / pass 1.
    const float f = gCasterFade[i.casterId];
    const uint lod = gCasterLod[i.casterId] & kCasterLodMask;
    v.lodFade = (f > 0.0f) ? ((gGroupLod == lod) ? -f : f) : 0.0f;
    return v;
}
#endif

[RootSignature(GBUFFER_INDIRECT_RS)]
PSOut PSMain(VSOutIndirect v, bool isFrontFace : SV_IsFrontFace)
{
#if TESSY_GBUFFER_MS
    // Neighbouring pixels of one wave may belong to different groups' materials: the indices are not uniform.
    LoadSurfaceParams(v.surface);
    Texture2D gAlbedo = ResourceDescriptorHeap[NonUniformResourceIndex(texIndices.x)];
    Texture2D gMR = ResourceDescriptorHeap[NonUniformResourceIndex(texIndices.y)];
    Texture2D gNormalMap = ResourceDescriptorHeap[NonUniformResourceIndex(texIndices.z)];
    Texture2D gEmissiveMap = ResourceDescriptorHeap[NonUniformResourceIndex(EMISSIVE_MAP_INDEX(texIndices))];
#elif GBUFFER_BINDLESS
    Texture2D gAlbedo = ResourceDescriptorHeap[texIndices.x];
    Texture2D gMR = ResourceDescriptorHeap[texIndices.y];
    Texture2D gNormalMap = ResourceDescriptorHeap[texIndices.z];
    Texture2D gEmissiveMap = ResourceDescriptorHeap[EMISSIVE_MAP_INDEX(texIndices)];
#endif
    const VSOut i = v.b;
    const InstancePerObject o = gInstances[v.casterId];
    const float4 baseColor = o.baseColor;
    const float2 metalRough = o.metalRough;
    const float alphaCutoff = o.alphaCutoff;
    const float mrMultiply = o.mrMultiply;
    const float4 texOffsScale = o.texOffsScale;
    const float4 texFlags = o.texFlags;
    const float3 emissive = o.emissive;

    LodFadeClip(i.H.xy, v.lodFade);
    AlphaTestClip(gAlbedo, gSmp, i.UV, texOffsScale, terrainTiling, terrainEdgeParams,
                  baseColor.a, alphaCutoff);

    // A back face: the normal map in the front face's frame, THEN the flip (gbuffer.hlsl says why).
    const float3 NNorm = normalize(i.NWS);
    const float faceSign = isFrontFace ? 1.0f : -1.0f;

    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValuesP(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, texOffsScale, texFlags,
                        terrainTiling, terrainEdgeParams, albedo, mr, N);

    // baseColor multiplies the texture in BOTH layouts. An imported material's factors are baked
    // into its DDS, so its tint is 1 unless someone authored one (material "tint", object
    // "baseColor") -- and then they mean it: a lerp that dropped it made the tint a control that
    // did nothing on every textured imported mesh, while RT reflections (rt_reflect_common.hlsli)
    // already multiplied it.
    albedo = texFlags.x > 0.5 ? albedo * baseColor.rgb : baseColor.rgb;
    float2 texturedMR = lerp(mr, mr * metalRough.xy, mrMultiply);
    mr = lerp(metalRough.xy, texturedMR, texFlags.y);
    if (texFlags.z < 0.5)
    {
        N = NNorm;
    }
    N *= faceSign;
    float2 currUv = ClipToUV(i.clipH);
    float2 prevUv = ClipToUV(i.prevH);
    float2 motion = currUv - prevUv;

    const float3 emissiveOut = ApplyEmissiveMap(emissive, gEmissiveMap, gSmp, i.UV, texOffsScale, texIndices.w);
    return FinalizeGBuffer(albedo, mr, N, emissiveOut, subsurfaceColor, transmissionStrength,
                           ambientOcclusion, indirectSpecularScale, transmissionAlbedoPower,
                           transmissionNormalWeight, motion, i.objectId);
}
