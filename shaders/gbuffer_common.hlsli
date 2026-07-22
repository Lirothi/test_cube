#pragma pack_matrix(row_major)
#include "utils.hlsli"

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
    float4 texFlags; // x=useAlbedo, y=useMR, z=useNormalMap, w=reserved
    uint objectId;
    float3 emissive; // D: premultiplied color*strength, added to RT2
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
};

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
    float4 RT5 : SV_Target5; // GBAux: AO, indirect specular scale, shading model / 15, reserved
#else
    float4 RT4 : SV_Target4; // GBAux: AO, indirect specular scale, shading model / 15, reserved
#endif
};

inline VSOut BaseVS(float3 pos,
                    float4x4 world,
                    float4x4 prevWorld,
                    float4x4 viewProj,
                    float3 norm,
                    float4 tangent,
                    float2 uv,
                    uint objectIdValue)
{
    VSOut o;
    float4 posH = float4(pos, 1.0f);
    float4 worldPos = mul(posH, world);
    o.H = mul(worldPos, viewProj);
    o.clipH = mul(worldPos, viewProjNoJitter);
    o.prevH = mul(mul(posH, prevWorld), prevViewProjNoJitter);

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
                             float indirectSpecularScaleValue, float2 motion, uint objectIdValue)
{
    PSOut o;
    float metal = mr.x;
    float rough = mr.y;

    //albedo = test[0].rgb;
    o.RT0 = float4(albedo, PackRM(rough, metal));
    //o.RT1 = float4(NrmTo01(NormalizeSafe(NWS, float3(0, 0, 1))), 1.0);
    o.RT1 = float4(NrmTo01(NWS), 1.0);
#if SHADING_MODEL_ID == 1
    o.RT2 = float4(subsurfaceColorValue * transmissionStrengthValue, 0.0);
#else
    o.RT2 = float4(emissiveValue, 0.0);
#endif
    o.RT3 = motion;
#ifdef EDITOR_OBJECT_ID
    o.RT4 = objectIdValue;
    o.RT5 = float4(saturate(ambientOcclusionValue), saturate(indirectSpecularScaleValue),
                   EncodeShadingModel(SHADING_MODEL_ID), 0.0);
#else
    o.RT4 = float4(saturate(ambientOcclusionValue), saturate(indirectSpecularScaleValue),
                   EncodeShadingModel(SHADING_MODEL_ID), 0.0);
#endif
    return o;
}

//#ifndef NORMALMAP_IS_RG   // 0 = RGB(A) normal map, 1 = RG/BC5
//#define NORMALMAP_IS_RG 1
//#endif

// Parameterized shading fetch (per-instance texOffsScale/texFlags). The non-instanced
// wrapper delegates with the PerObject globals so both paths produce identical math.
inline void FetchShadingValuesP(Texture2D txAlbedo, Texture2D txMR, Texture2D txNorm, SamplerState samp, float2 uv, float4 TWS,
                                float4 texOffsScale, float4 texFlags,
                                out float3 albedo, out float2 mr, inout float3 norm)
{
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
    nrg *= texFlags.w;
    float  nz2 = saturate(1.0 - dot(nrg, nrg));
    float3 nTS = float3(nrg, sqrt(nz2));
#else
    // --- RGB(A): standard path ---
    float3 nTS = txNorm.Sample(samp, tfUVp(uv, texOffsScale)).xyz * 2.0 - 1.0;
    nTS.xy *= texFlags.w * 1;
#endif
    //norm = PerturbNormal_Deriv(nTS, norm, PVS, uv);
    float3 T = normalize(TWS.xyz);
    float3 B = normalize(cross(norm, T) * TWS.w);

    norm = normalize(T * nTS.x + B * nTS.y + norm * nTS.z);
}

#ifndef GBUFFER_SKIP_PEROBJECT
inline void FetchShadingValues(Texture2D txAlbedo, Texture2D txMR, Texture2D txNorm, SamplerState samp, float2 uv, float4 TWS,
                                out float3 albedo, out float2 mr, inout float3 norm)
{
    FetchShadingValuesP(txAlbedo, txMR, txNorm, samp, uv, TWS, texOffsScale, texFlags, albedo, mr, norm);
}
#endif

// C1 masked foliage: discard the fragment when baseColor.a * albedo.a < cutoff. A negative
// cutoff disables the test for this slot (so an opaque submesh sharing an ALPHA_TEST PSO never
// clips). Compiled out entirely unless the material's PSO defines ALPHA_TEST.
inline void AlphaTestClip(Texture2D txAlbedo, SamplerState samp, float2 uv,
                          float4 texOffsScaleV, float baseAlpha, float cutoff)
{
#if defined(ALPHA_TEST) && ALPHA_TEST
    if (cutoff >= 0.0)
    {
        float a = txAlbedo.Sample(samp, tfUVp(uv, texOffsScaleV)).a * baseAlpha;
        clip(a - cutoff);
    }
#endif
}

#endif
