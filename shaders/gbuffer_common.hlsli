#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "terrain_tiling.hlsli"
#include "wind.hlsli" // W4: shared WindOffset (same math as the shadow VS in W5)

#ifndef GBUFFER_COMMON_HLSLI
#define GBUFFER_COMMON_HLSLI

#ifndef SHADING_MODEL_ID
#define SHADING_MODEL_ID 0
#endif

// Per-instance payload (Step 4 auto-instancing). Same field layout as PerObject so the
// shading math is shared; arrayed and indexed by SV_InstanceID. The explicit pad makes
// the C++/HLSL offsets match (metalRough at 144, texOffsScale at 160 — cbuffer rules).
#ifndef GBUFFER_MAX_INSTANCES
#define GBUFFER_MAX_INSTANCES 256
#endif
struct InstancePerObject
{
    float4x4 world;
    float4x4 prevWorld;
    float4 baseColor;
    float2 metalRough;
    float alphaCutoff; // C1 alpha test (-1 disables)
    float mrMultiply; // 0=MR texture overrides values, 1=texture*metalRough
    float4 texOffsScale;
    float4 texFlags;
    uint objectId;
    float3 emissive; // D: premultiplied color*strength
    float windStrength; // W3: per-object foliage sway strength (0 = rigid)
    float windLeafScale; // W7.4: WORLD metres of leaf arc per unit of COLOR_0.b (0 = unbaked)
    float windFoliage; // PER-SLOT 0..1 (0 = trunk, 1 = leaves)
    float windTrunkStiff; // per-object; divides the main bend
};

#ifndef GBUFFER_SKIP_PEROBJECT
cbuffer PerObject : register(b0)
{
    float4x4 world;
    float4x4 prevWorld;

    float4 baseColor; // fallback Albedo (linear); .a = alpha factor (glTF)
    float2 metalRough; // x=metallic (fallback), y=roughness (fallback)
    float alphaCutoff; // C1 alpha test (-1 disables)
    float mrMultiply; // 0=MR texture overrides values, 1=texture*metalRough
    float4 texOffsScale;
    float4 texFlags; // x=useAlbedo, y=useMR, z=useNormalMap, w=normalStrength
    uint objectId;
    float3 emissive; // D: premultiplied color*strength, added to RT2
    float windStrength; // W3: per-object foliage sway strength (0 = rigid); read by the VS in W4
    float windLeafScale; // W7.4: WORLD metres of leaf arc per unit of COLOR_0.b (0 = unbaked)
    float windFoliage; // PER-SLOT 0..1 (0 = trunk, 1 = leaves)
    float windTrunkStiff; // per-object; divides the main bend
    // Dithered LOD crossfade (camera pass only; shadow variants never write it -> 0 = solid).
    // 0 = no dither; +f = this draw is FADING IN with coverage f; -f = FADING OUT with
    // coverage 1-f. The two draws of one transition carry +f/-f -> complementary masks.
    float lodFade;
};
#else
// Instanced variant: per-object data is an array indexed by SV_InstanceID (root CBV b0).
cbuffer InstanceArray : register(b0)
{
    InstancePerObject inst[GBUFFER_MAX_INSTANCES];
};
#endif

// Per-view/per-pass data shared by every object in a pass. Filled once per pass
// (camera matrices for the gbuffer pass, light viewProj for the shadow passes).
// Declared here so the gbuffer + shadow variants share one layout; the depth-only
// shadow shaders only consume viewProj (the other two are unused there).
cbuffer PerView : register(b1)
{
    float4x4 viewProj;
    float4x4 viewProjNoJitter;
    float4x4 prevViewProjNoJitter;
    // W3: global wind, filled from WindState by BuildGBufferViewCB. The gbuffer VS (W4) applies the
    // sway offset from these; the depth-only shadow shaders declare a shorter PerView and ignore
    // this tail (W5 adds the matching fields to the shadow CB). windGustMul defaults to 1.
    float  windTime;
    float  windPrevTime;
    float2 windDirXZ;
    float  windSwayAmp;
    float  windSwayFreq;
    float  windGustMul;
    float  windPrevGustMul;
};

// W4/W5: single call site of the PerView wind fields for every shader that includes this header
// (the gbuffer BaseVS and the CPU-fallback shadow VS variants). `t` is windTime for the current
// position and windPrevTime for the motion-vector prev position. The shadow VS in
// shadow_indirect_csm.hlsl declares its own PerView copy and mirrors this helper — the two MUST
// stay identical or the shadow detaches from the tree.
inline float3 ApplyWindWS(float3 objPos, float3 posWS, float4x4 w, float windStrengthValue,
                          float4 windWeights, float foliageValue, float trunkStiffValue,
                          float leafScaleValue, float gustMulValue, float t)
{
    return WindOffset(objPos, posWS, float3(w._41, w._42, w._43), windStrengthValue,
                      windWeights, foliageValue, trunkStiffValue, leafScaleValue,
                      windDirXZ, windSwayAmp, windSwayFreq, gustMulValue, t);
}

// Parameterized UV transform (per-instance texOffsScale). The non-instanced wrapper below
// delegates to this with the PerObject global so both paths produce identical math.
float2 tfUVp(float2 rawUV, float4 texOffsScale)
{
    //return float2((rawUV + texOffsScale.xy) * texOffsScale.zw);
    return float2((rawUV * texOffsScale.zw) + texOffsScale.xy);
}

#ifndef GBUFFER_SKIP_PEROBJECT
float2 tfUV(float2 rawUV)
{
    return tfUVp(rawUV, texOffsScale);
}
#endif

struct VSIn
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT; // .w = handedness
    float2 UV : TEXCOORD0;
    // W7.2 baked wind weights (COLOR_0 at offset 48 of VertexPNTUV, R8G8B8A8_UNORM):
    // r = geodesic weight, g = limb id, b = leaf-edge weight, a = baked marker.
    float4 WIND : COLOR0;
};

struct VSInInst
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT;
    float2 UV : TEXCOORD0;
    float4 WIND : COLOR0; // see VSIn
    uint IID : SV_InstanceID;
};

struct VSOut
{
    float4 H : SV_POSITION;
    float4 clipH : TEXCOORD4;
    float4 prevH : TEXCOORD3;
    float3 NWS : TEXCOORD1;
    float4 TWS : TEXCOORD2; // .xyz = tangent in world, .w = sign
    float2 UV : TEXCOORD0;
    nointerpolation uint objectId : TEXCOORD5;
};

struct PSOut
{
    float4 RT0 : SV_Target0; // Albedo.rgb + A=pack(rough,metal)
    float4 RT1 : SV_Target1; // Normal.xyz (RGB10) + unused A2
    float4 RT2 : SV_Target2; // DefaultLit: emissive; foliage: subsurface/transmission payload
    float2 RT3 : SV_Target3; // Motion vector (UV delta)
#ifdef EDITOR_OBJECT_ID
    uint RT4 : SV_Target4; // Editor object id (0 = none)
    float4 RT5 : SV_Target5; // GBAux: AO, indirect specular scale, shading model / 15, transmission normal weight
#else
    float4 RT4 : SV_Target4; // GBAux: AO, indirect specular scale, shading model / 15, transmission normal weight
#endif
};

inline VSOut BaseVS(float3 pos,
                    float4x4 world,
                    float4x4 prevWorld,
                    float4x4 viewProj,
                    float3 norm,
                    float4 tangent,
                    float2 uv,
                    uint objectIdValue,
                    float4 windWeights,
                    float windStrengthValue,
                    float windFoliageValue,
                    float windTrunkStiffValue,
                    float windLeafScaleValue)
{
    VSOut o;
    float4 posH = float4(pos, 1.0f);

    // W4: global wind sway via the shared WindOffset. Phase comes from the world origin, so every
    // submesh/slot (and, in W5, the shadow) bends in lockstep. The SAME offset is applied to the
    // prev world position with windPrevTime, so DLSS/TAA motion vectors track the sway (else the
    // moving leaves smear). windStrengthValue 0 => zero offset => byte-identical to no-wind.
    // The offset is a function of the UN-swayed world position, so read it before adding.
    float4 worldPos = mul(posH, world);
    worldPos.xyz += ApplyWindWS(pos, worldPos.xyz, world, windStrengthValue, windWeights,
                                windFoliageValue, windTrunkStiffValue, windLeafScaleValue,
                                windGustMul, windTime);

    float4 prevWorldPos = mul(posH, prevWorld);
    prevWorldPos.xyz += ApplyWindWS(pos, prevWorldPos.xyz, prevWorld, windStrengthValue,
                                    windWeights, windFoliageValue, windTrunkStiffValue,
                                    windLeafScaleValue, windPrevGustMul, windPrevTime);

    o.H = mul(worldPos, viewProj);
    o.clipH = mul(worldPos, viewProjNoJitter);
    o.prevH = mul(prevWorldPos, prevViewProjNoJitter);

    float3x3 w3 = (float3x3) world;
    float3 N = NormalizeSafe(TransformDirectionWS(norm, w3), float3(0, 0, 1));
    float3 T = NormalizeSafe(TransformDirectionWS(tangent.xyz, w3), float3(1, 0, 0));
    o.TWS = float4(T, tangent.w);
    o.NWS = N;
    o.UV = uv;
    o.objectId = objectIdValue;
    return o;
}

// Final MRT output using prepared values
inline PSOut FinalizeGBuffer(float3 albedo, float2 mr, float3 NWS,
                             float3 emissiveValue, float3 subsurfaceColorValue,
                             float transmissionStrengthValue, float ambientOcclusionValue,
                             float indirectSpecularScaleValue, float transmissionAlbedoPowerValue,
                             float transmissionNormalWeightValue, float2 motion, uint objectIdValue)
{
    PSOut o;
    float metal = mr.x;
    float rough = mr.y;

    //albedo = test[0].rgb;
    o.RT0 = float4(albedo, PackRM(rough, metal));
    //o.RT1 = float4(NrmTo01(NormalizeSafe(NWS, float3(0, 0, 1))), 1.0);
    o.RT1 = float4(NrmTo01(NWS), 1.0);
#if SHADING_MODEL_ID == 1
    // Use linear albedo as a spatial absorption/thickness proxy. This is a Beer-Lambert-like
    // relation (T = T0^distance): dark veins/stems transmit less while color variation survives.
    const float albedoPower = max(transmissionAlbedoPowerValue, 0.0);
    const float3 albedoTransmission = pow(max(saturate(albedo), 1.0e-3), albedoPower);
    o.RT2 = float4(subsurfaceColorValue * transmissionStrengthValue * albedoTransmission, 0.0);
#else
    o.RT2 = float4(emissiveValue, 0.0);
#endif
    o.RT3 = motion;
#ifdef EDITOR_OBJECT_ID
    o.RT4 = objectIdValue;
    o.RT5 = float4(saturate(ambientOcclusionValue), saturate(indirectSpecularScaleValue),
                   EncodeShadingModel(SHADING_MODEL_ID), saturate(transmissionNormalWeightValue));
#else
    o.RT4 = float4(saturate(ambientOcclusionValue), saturate(indirectSpecularScaleValue),
                   EncodeShadingModel(SHADING_MODEL_ID), saturate(transmissionNormalWeightValue));
#endif
    return o;
}

//#ifndef NORMALMAP_IS_RG   // 0 = RGB(A) normal map, 1 = RG/BC5
//#define NORMALMAP_IS_RG 1
//#endif

// Parameterized shading fetch (per-instance texOffsScale/texFlags). The non-instanced
// wrapper delegates with the PerObject globals so both paths produce identical math.
inline void FetchShadingValuesP(Texture2D txAlbedo, Texture2D txMR, Texture2D txNorm, SamplerState samp, float2 uv, float4 TWS,
                                float4 texOffsScale, float4 texFlags, float4 terrainTiling,
                                float4 terrainEdgeParams,
                                out float3 albedo, out float2 mr, inout float3 norm)
{
#if SHADING_MODEL_ID == 2
    TerrainTileSample tile0;
    TerrainTileSample tile1;
    TerrainTileSample tile2;
    TerrainBuildTileSamples(tfUVp(uv, texOffsScale), terrainTiling, terrainEdgeParams,
                            tile0, tile1, tile2);

    albedo = TerrainSampleTextureColor(
        txAlbedo, samp, tile0, tile1, tile2).rgb;
    const float4 packedMR = TerrainSampleTextureColor(
        txMR, samp, tile0, tile1, tile2);
#if MR_LAYOUT_GLTF
    mr = packedMR.bg;
#else
    mr = packedMR.rg;
#endif

#if NORMALMAP_IS_RG
    const bool terrainNormalIsRG = true;
#else
    const bool terrainNormalIsRG = false;
#endif
    const float2 blendedDerivative = TerrainBlendNormalDerivatives(
        txNorm, samp, tile0, tile1, tile2, terrainNormalIsRG);
    const float3 nTS = normalize(float3(-blendedDerivative * texFlags.w, 1.0));
#else
    albedo = txAlbedo.Sample(samp, tfUVp(uv, texOffsScale)).rgb;
#if MR_LAYOUT_GLTF
    // glTF packs metallic in B, roughness in G. Swizzle to the engine's (metal, rough) order
    // WITHOUT re-encoding the texture at load. (undefined MR_LAYOUT_GLTF -> 0 -> engine layout.)
    mr = txMR.Sample(samp, tfUVp(uv, texOffsScale)).bg;
#else
    mr = txMR.Sample(samp, tfUVp(uv, texOffsScale)).rg; // engine: R=metal, G=rough
#endif

#if NORMALMAP_IS_RG
    // --- RG (BC5/R8G8_UNORM): n.xy in [-1..1], reconstruct n.z ---
    float2 nrg = txNorm.Sample(samp, tfUVp(uv, texOffsScale)).rg * 2.0 - 1.0;
    // Reconstruct the source normal before applying strength. Scaling XY first can push it
    // outside the unit circle; saturating the resulting negative Z then creates flat Z=0
    // patches for strengths above one.
    float  nz = sqrt(saturate(1.0 - dot(nrg, nrg)));
    float3 nTS = normalize(float3(nrg * texFlags.w, max(nz, 1e-4)));
#else
    // --- RGB(A): standard path ---
    float3 nTS = txNorm.Sample(samp, tfUVp(uv, texOffsScale)).xyz * 2.0 - 1.0;
    nTS.xy *= texFlags.w * 1;
#endif
#endif
    //norm = PerturbNormal_Deriv(nTS, norm, PVS, uv);
    float3 T = normalize(TWS.xyz);
    float3 B = normalize(cross(norm, T) * TWS.w);

    norm = normalize(T * nTS.x + B * nTS.y + norm * nTS.z);
}

#ifndef GBUFFER_SKIP_PEROBJECT
inline void FetchShadingValues(Texture2D txAlbedo, Texture2D txMR, Texture2D txNorm, SamplerState samp, float2 uv, float4 TWS,
                                float4 terrainTiling, float4 terrainEdgeParams,
                                out float3 albedo, out float2 mr, inout float3 norm)
{
    FetchShadingValuesP(txAlbedo, txMR, txNorm, samp, uv, TWS, texOffsScale, texFlags,
                        terrainTiling, terrainEdgeParams, albedo, mr, norm);
}
#endif

// Dithered LOD crossfade (screen-door). In the fade band the SAME instance is drawn at BOTH
// LOD tiers: the outgoing draw carries fade = -f, the incoming one fade = +f. The 4x4 Bayer
// threshold splits the pixels between them EXACTLY (in keeps bayer < f, out keeps bayer >= f
// — union = every pixel, intersection = none), so depth stays consistent and nothing double
// writes; DLSS/TAA then resolves the pattern into a smooth blend. fade == 0 (the common
// case) costs one compare. discard here does NOT lose early-Z REJECTION — only the early
// depth WRITE turns late, same as the alpha-tested foliage this pass already draws.
// 4x4 Bayer thresholds ((n + 0.5) / 16). File scope, not function-local: dxc turns a local
// static array into an external declaration that fails validation as "unused".
static const float kLodFadeBayer[16] = {
     0.5 / 16.0,  8.5 / 16.0,  2.5 / 16.0, 10.5 / 16.0,
    12.5 / 16.0,  4.5 / 16.0, 14.5 / 16.0,  6.5 / 16.0,
     3.5 / 16.0, 11.5 / 16.0,  1.5 / 16.0,  9.5 / 16.0,
    15.5 / 16.0,  7.5 / 16.0, 13.5 / 16.0,  5.5 / 16.0 };

inline void LodFadeClip(float2 svPos, float fade)
{
    if (fade != 0.0f)
    {
        const uint2 p = uint2(svPos) & 3u;
        const float bayer = kLodFadeBayer[p.y * 4u + p.x];
        clip(fade > 0.0f ? (fade - bayer) : (bayer + fade));
    }
}

// C1 masked foliage: discard the fragment when baseColor.a * albedo.a < cutoff. A negative
// cutoff disables the test for this slot (so an opaque submesh sharing an ALPHA_TEST PSO never
// clips). Compiled out entirely unless the material's PSO defines ALPHA_TEST.
inline void AlphaTestClip(Texture2D txAlbedo, SamplerState samp, float2 uv,
                          float4 texOffsScaleV, float4 terrainTiling, float4 terrainEdgeParams,
                          float baseAlpha, float cutoff)
{
#if defined(ALPHA_TEST) && ALPHA_TEST
    if (cutoff >= 0.0)
    {
#if SHADING_MODEL_ID == 2
        TerrainTileSample tile0;
        TerrainTileSample tile1;
        TerrainTileSample tile2;
        TerrainBuildTileSamples(tfUVp(uv, texOffsScaleV), terrainTiling, terrainEdgeParams,
                                tile0, tile1, tile2);
        float a = TerrainSampleTexture(txAlbedo, samp, tile0, tile1, tile2).a * baseAlpha;
#else
        float a = txAlbedo.Sample(samp, tfUVp(uv, texOffsScaleV)).a * baseAlpha;
#endif
        clip(a - cutoff);
    }
#endif
}

#endif
