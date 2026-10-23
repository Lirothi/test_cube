// t7 = GlassReflection: the off-screen RT glass reflection (S15b), computed by the
// GlassReflGbuffer + GlassReflections passes and sampled here. It's a normal texture
// (no RayQuery in this shader), so the glass PSO is identical on RT and non-RT HW.
//
// GLASS_MATERIAL = 1: the thinTranslucent shading model (UE MSM_ThinTranslucent, glTF
// KHR_materials_transmission) -- a GBufferRenderable's glass material SLOT drawn by
// GBufferRenderable::RenderTranslucent with the slot's own textures (t15 albedo, t16 MR, t17
// transmission, t6 normal; s3 the material sampler) and MaterialParams (the mat* fields of b0). It
// blends OVER what is behind (ONE / INV_SRC_ALPHA) instead of replacing it, so glass behind glass shows.
#ifndef GLASS_MATERIAL
#define GLASS_MATERIAL 0
#endif
#if GLASS_MATERIAL
#define GLASS_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1), DescriptorTable(SRV(t0, numDescriptors=18, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE))"
#else
#define GLASS_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1), DescriptorTable(SRV(t0, numDescriptors=15, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE))"
#endif
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "ibl_common.hlsli" // P5: the shared roughness <-> mip mapping
#include "vsm_sample.hlsli"
#include "csm_sample.hlsli"
#include "local_shadow_bias.hlsli" // plan S1: the point light's receiver bias, the light pass's own
#include "local_shadow_filter.hlsli" // plan S2: the light passes' spot / point filters
#include "height_fog.hlsli" // plan A5: the analytic medium beyond the fog volume
#include "fog_common.hlsli"  // plan A5: the froxel volume

struct PointLightData
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
    float4 shadowParams; // x = shadow slot (-1 = none), y = bias, z = nearPlane, w = farPlane (radius)
    float4 volumetric;   // x = volumetric scattering intensity (plan A4d), y = UE slope scale (S1), z = sharpen, w = filter (S2)
    float4 shadowFade;   // x = UE's shadow fade (S3), y = the atlas page (S9), z = 1 / its face res (S4), w = texel-bias normal texels
    float4 shadowAtlas;  // S9: its 3f x 2f block in the page, page UV: xy = corner, zw = one face -- mirrors LightManager::PointLightGpu
    float4 shadowParams4; // S10e: x = the light's VSM mip bias (unread here); S4: y = 1 / (f - n) of a LINEAR face depth;
                          // S9e: z = the VSM view of its +X face
};

struct SpotLightData
{
    float4 positionRange;      // xyz = position, w = range
    float4 directionCosOuter;  // xyz = direction, w = cos(outer)
    float4 colorIntensity;     // xyz = color, w = intensity
    float4 shadowParams;       // x = cos(inner), y = shadow index, z = invAngleRange, w = depth bias
    float4 shadowParams2;      // x = normal bias (world units), y = volumetric scattering, z = UE's shadow fade (S3),
                               // w = the texel bias's normal texels (> 0 marks it; x is then its push texels)
    float4x4 viewProj;
    float4 shadowParams3;      // S2: x = near, y = soft-transition scale (1/m), z = sharpen, w = 1 UE's tent / 0 old box
    float4 shadowAtlas;        // S9: its square in its atlas page, page UV: xy = corner, z = size, w = page
    float4 shadowParams4;      // S10e: x = the light's VSM mip bias (unread here); S4: y = 1 / (f - n) of a LINEAR atlas depth;
                               // S9e: z = its VSM view
};

// Per-object data (unique per glass instance).
cbuffer GlassParams : register(b0)
{
    float4x4 world;
    float4x4 prevWorld;
    float4 absorptionThickness;   // xyz = absorption, w = thickness
    float4 tintRoughness;         // xyz = tint color, w = roughness
    float4 matExtra;              // x = IOR, y = reflection strength, z = refraction distortion, w = normal map enabled (0=disabled, 1=enabled)
    uint objectId;
    uint3 _objectIdPad;
    // GLASS_MATERIAL only (GBufferRenderable::RenderTranslucent; zero from TransparentStaticMesh, unread there):
    // the slot's MaterialParams and glass values.
    float4 matBaseColor;          // linear tint (rgb, multiplies the albedo map) + alpha factor (a)
    float4 matSurface;            // x = metallic, y = roughness, z = transmission factor, w = alpha cutoff (-1 = no split)
    float4 matTexFlags;           // x = useAlbedo, y = useMR, z = useNormal, w = normal strength
    float4 matTexOffsScale;       // UV offset (xy), scale (zw) -- gbuffer_common.hlsli's tfUVp
    float4 matGlass;              // x = transmission map present, y = mrMultiply, z = unused, w = unused
};

#include "glass_view_cb.hlsli" // b1: the transparent pass's shared per-view CB (also particles)

Texture2D SceneOpaque : register(t0);
Texture2D ShadowAtlas : register(t1);
Texture2DArray SpotShadowAtlas : register(t2);
TextureCube SkyboxTex : register(t3);
StructuredBuffer<PointLightData> PointLights : register(t4);
StructuredBuffer<SpotLightData> SpotLights : register(t5);
Texture2D NormalMap : register(t6);
Texture2D GlassReflection : register(t7); // S15b: premultiplied off-screen RT glass reflection
Texture2DArray PointShadowAtlas : register(t8); // B3: point shadow faces, 6 per light (D16 -> R16); a 2D array since S4
StructuredBuffer<uint> VsmPageTable : register(t9);  // Rung 2 / Step 21
Texture2D VsmPool : register(t10);
// P5: GGX-prefiltered sky. Inert unless skySpecMipCount says this level's sky has it, in which case
// glass stops guessing `rough * 5` on the display cube and reads the shared environment.
TextureCube SkySpecular : register(t11);
Texture3D<float4> FogVolume : register(t12); // plan A5: the integrated froxel volume
// t13: S15, this frame's SDSM partitions. glass is a PIXEL shader, which is why the partition
// buffer's canonical state carries the PIXEL bit as well as the non-pixel one.
StructuredBuffer<SdsmPartition> SdsmPartitions : register(t13);
// Plan S7, the direct read: the local shadow cache's pool -- a light whose atlas page is negative is read there (page
// -page - 1, the atlas layout). The spot atlas stands in while there is no pool.
Texture2DArray LocalShadowPool : register(t14);
#if GLASS_MATERIAL
Texture2D MatAlbedo : register(t15);       // sRGB view
Texture2D MatMR : register(t16);           // R = metal, G = rough (B/G when MR_LAYOUT_GLTF)
Texture2D MatTransmission : register(t17); // R multiplies matSurface.z
#endif

SamplerState LinearSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
SamplerState EnvSampler : register(s2);
#if GLASS_MATERIAL
SamplerState MatSampler : register(s3);    // the G-buffer's own (anisotropic, wrap): material UVs tile
#endif

// Omnidirectional (cube) point shadow, depth-cube approach (B3). Mirrors PointShadowFactor
// in pointlight_cs.hlsl: normal-offset receiver, WORLD-space depth bias subtracted from the
// compare distance before projecting to the standard-projection depth (render is
// PerspectiveFovLH(90,1,near,far), LESS_EQUAL, clear 1.0 = far), then SampleCmpLevelZero on
// the light's face (PointShadowCubeUV, plan S4). 1 = lit, 0 = shadowed. The bias is local_shadow_bias.hlsli's.
static const float kPointPcfRadius  = 0.015f; // PCF disk radius as a fraction of the light distance
static const float3 kPointPcfOffsets[8] = {
    float3( 1,  1,  1), float3( 1, -1,  1), float3(-1, -1,  1), float3(-1,  1,  1),
    float3( 1,  1, -1), float3( 1, -1, -1), float3(-1, -1, -1), float3(-1,  1, -1)
};
// `Ng` = the vertex normal (no normal map) -- the texel bias's push, as the light passes' geometric normal.
float PointShadowFactorMap(PointLightData Ld, float3 P, float3 N, float3 Ng)
{
    if (Ld.shadowParams.x < 0.0f) { return 1.0f; }
    float3 Poff = P + N * ((Ld.shadowFade.w > 0.0f) ? 0.0f : PointShadowNormalOffset(Ld.shadowParams)); // the old bias only
    // Rung 2 / Step 21: sample the VSM page pool (matches pointlight_cs.hlsl's VSM branch).
    if (vsmParams.x != 0.0f)
    {
        // The push toward the light the atlas path compares at (local_shadow_bias.hlsli), as metres here.
        float3 toLight = Ld.position - Poff;
        const float dist = length(toLight);
        Poff += (toLight / max(dist, 1e-4f)) * (dist - PointShadowCompareDistance(dist, Ld.shadowParams, Ld.volumetric.y, 1.0f));
        return VsmPointShadow((uint)Ld.shadowParams4.z, Poff, Ld.position, Ld.shadowParams.z, // S9e: its first VSM view
                              Ld.shadowParams.w, camPosSky.xyz, vsmParams.y, 0.0f,
                              VsmPageTable, VsmPool, ShadowSampler);
    }
    // The TEXEL bias, as pointlight_cs.hlsl's (local_shadow_bias.hlsli): the receiver moves, the compare carries none.
    if (Ld.shadowFade.w > 0.0f)
    {
        const float3 d0 = P - Ld.position;
        const float texelWorld = 2.0f * max(abs(d0.x), max(abs(d0.y), abs(d0.z))) * Ld.shadowFade.z;
        Poff = LocalShadowTexelOffset(P, Ng, normalize(Ld.position - P), texelWorld, Ld.shadowFade.w, Ld.volumetric.y);
    }
    float3 d = Poff - Ld.position;
    float m = max(abs(d.x), max(abs(d.y), abs(d.z)));
    float nearP = Ld.shadowParams.z;
    float farP = max(Ld.shadowParams.w, nearP + 1e-3f);
    const float NoL = dot(N, normalize(Ld.position - P));
    // Plan S9: the light's 3f x 2f block of an atlas page (as pointlight_cs.hlsl's); a page texel = one face / res.
    const float4 block = Ld.shadowAtlas;
    const bool fromPool = Ld.shadowFade.y < 0.0f; // plan S7: its block in the cache pool (LocalShadowPool)
    const float page = fromPool ? -Ld.shadowFade.y - 1.0f : Ld.shadowFade.y;
    const float2 texel = block.zw * Ld.shadowFade.z;
    // Plan S8 (unblocked by S4, which put the light's resolution in its own data): pointlight_cs's filter, UE's 29-tap
    // disc, sharpened and squared -- glass no longer shades a point shadow its own way.
    if (Ld.volumetric.w != 0.0f && (Ld.shadowParams.y < 0.0f || Ld.shadowFade.w > 0.0f))
    {
        float s;
        [branch]
        if (fromPool)
        {
            s = PointShadowDisc29(LocalShadowPool, ShadowSampler, d, m, Ld.shadowParams, Ld.volumetric.y, NoL, Ld.shadowFade.z,
                                  block, page, texel, Ld.shadowParams4.y);
        }
        else
        {
            s = PointShadowDisc29(PointShadowAtlas, ShadowSampler, d, m, Ld.shadowParams, Ld.volumetric.y, NoL, Ld.shadowFade.z,
                                  block, page, texel, Ld.shadowParams4.y);
        }
        return LocalShadowFinish(s, Ld.volumetric.z);
    }
    float mBiased = PointShadowCompareDistance(m, Ld.shadowParams, Ld.volumetric.y, NoL);
    float zc = PointShadowCompareDepth(mBiased, nearP, farP, Ld.shadowParams4.y); // plan S4
    // PCF: 8 cube taps, offset scaled by m for a ~constant world-space footprint (the old filter,
    // shadow.localFilterUe:0).
    float radius = m * kPointPcfRadius;
    float shadow = 0.0f;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float3 sd = d + kPointPcfOffsets[i] * radius;
        const float3 tap = PointShadowCubeUV(sd, block, page, texel);
        shadow += fromPool ? LocalShadowPool.SampleCmpLevelZero(ShadowSampler, tap, zc)
                           : PointShadowAtlas.SampleCmpLevelZero(ShadowSampler, tap, zc);
    }
    return shadow * 0.125f;
}

// Plan S3: UE's shadow fade, as pointlight_cs.hlsl's.
float PointShadowFactor(PointLightData Ld, float3 P, float3 N, float3 Ng)
{
    return lerp(1.0f, PointShadowFactorMap(Ld, P, N, Ng), Ld.shadowFade.x);
}

struct VSIn
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT;
    float2 UV : TEXCOORD0;
};

struct VSOut
{
    float4 posH : SV_POSITION;
    float4 prevPosH : TEXCOORD5;
    float4 clipPos : TEXCOORD6;
    float3 posWS : TEXCOORD0;
    float3 normalWS : TEXCOORD1;
    float3 tangentWS : TEXCOORD2;
    float3 bitangentWS : TEXCOORD3;
    float2 uv : TEXCOORD4;
};

[RootSignature(GLASS_RS)]
VSOut VSMain(VSIn input)
{
    VSOut o;
    float4 localPos = float4(input.P, 1.0f);
    float4 worldPos = mul(localPos, world);
    o.posWS = worldPos.xyz;
    o.posH = mul(worldPos, viewProj);
    o.clipPos = mul(worldPos, viewProjNoJitter);
    float4 prevWorldPos = mul(localPos, prevWorld);
    o.prevPosH = mul(prevWorldPos, prevViewProjNoJitter);
    float3x3 w3 = (float3x3) world;
    float3 tangentWS = mul(input.T.xyz, w3);
    float3 normalWS = mul(input.N, w3);
    tangentWS = normalize(tangentWS);
    normalWS = normalize(normalWS);
    float3 bitangentWS = normalize(cross(normalWS, tangentWS) * input.T.w);
    o.normalWS = normalWS;
    o.tangentWS = tangentWS;
    o.bitangentWS = bitangentWS;
    o.uv = input.UV;
    return o;
}

#ifndef NORMALMAP_IS_RG
#define NORMALMAP_IS_RG 0
#endif

float SampleShadowCSM(float3 Pws, float3 Nws, float NdotL)
{
    // Step 24f: VSM mode samples the directional clipmap (matches lighting_cs.hlsl); Legacy = cascades.
    if (vsmParams.x != 0.0f)
    {
        // P16.16: same arithmetic and the same numbers lighting_cs gets, or glass shades against
        // a differently-biased shadow.
        VsmSmrtParams smrt;
        smrt.rayCount = (uint)smrtParams.x;
        smrt.samplesPerRay = (uint)smrtParams.y;
        smrt.rayLengthScale = smrtParams.z;
        smrt.extrapolateMaxSlope = smrtParams.w;
        smrt.sourceRadius = smrtParams2.x;
        smrt.texelDitherScale = smrtParams2.y;
        smrt.levelMargin = smrtParams2.z;
        smrt.frameIndex = (uint)smrtParams2.w;
        smrt.adaptiveRayCount = 0u; // pixel shader: no wave-op early out (see VSM_SMRT_COMPUTE)
        return VsmClipmapShadow(Pws, Nws, camPosSky.xyz, clipmapParams.y, clipmapParams.z,
                                clipmapParams.w, vsmParams.z, vsmParams.w,
                                invProj._11, clipmapUvNormal,
                                normalize(-sunDirAmbient.xyz), smrt,
                                // No screen-space ray here: the transparent pass has no scene
                                // depth to march, and a glass surface is not where contact detail
                                // is judged. Same scope note as the adaptive ray count above.
                                0.0f,
                                clipmapViewProj, VsmPageTable, VsmPool, ShadowSampler);
    }

    // S3: the cascade path now goes through the SHARED csm_sample.hlsli, same as lighting_cs.
    // This is a deliberate behaviour FIX, not a refactor: the copy that used to live here had no
    // coarser-cascade fallback (it returned "lit" the moment the point left the tile) and no blend
    // band, so glass disagreed with the geometry next to it at every cascade boundary.
    CsmParams p;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        p.lightViewProj[i] = lightViewProj[i];
        p.scaleBias[i]     = cascadeScaleBias[i];
    }
    p.splitsVS     = cascadeSplitsVS;
    p.depthBiasNDC = shadowBiasNDC;
    p.cascadeTexelWS = cascadeTexelWS;
    p.atlasSize    = shadowAtlasSizeInv.xy;   // xy = size, zw = 1/size (see the GlassView cbuffer)
    p.camPosWS     = camPosSky.xyz;
    p.camDirWS     = normalize(camDirWS.xyz);
    p.pcfRadius    = 1.0f;
    // S8: same derivation as lighting_cs -- shadowBiasNDC already scales with the cascade's world
    // texel, so it IS the transition width. Glass must match the geometry beside it exactly.
    [unroll]
    for (int t = 0; t < 4; ++t)
    {
        p.transitionScale[t] = 1.0f / max(1e-6f, shadowBiasNDC[t]);
    }
    p.receiverBiasMin  = csmFilterMode.y;
    p.sharpen          = csmFilterMode.z;
    p.overBlurCorrect  = csmFilterMode.w;
    p.normalBiasTexels = csmFilterParams.w;
    p.farSplit             = csmFilterParams.x;
    p.blendFraction        = csmFilterParams.y;
    p.distanceFadeFraction = csmFilterParams.z;
    p.useGatherPcf     = (uint)csmFilterMode.x;
    p.evsmExponents    = float2(0.0f, 0.0f); // S16: see lighting_cb.hlsli
    // S15: same overlay as lighting_cb.hlsli's MakeCsmParams. Glass has to agree with the geometry
    // beside it EXACTLY -- that was the whole point of S3 -- so this is not optional here.
    if (csmSdsmParams.x > 0.0f) { CsmApplySdsm(p, SdsmPartitions, (uint)csmSdsmParams.x); }

    int cascade;
    return CsmSampleShadow(p, ShadowAtlas, ShadowSampler, LinearSampler, Pws, Nws, NdotL, cascade);
}

float ShadowPCF3x3Array(Texture2DArray atlas, float3 uvw, float depth, float2 texel)
{
    float s = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texel;
            s += atlas.SampleCmpLevelZero(ShadowSampler, float3(uvw.xy + offset, uvw.z), depth);
        }
    }
    return s / 9.0f;
}

float SampleSpotShadowMap(const SpotLightData light, float3 P, float3 N, float3 Ng, float NdotL)
{
    // shadowParams.y < 0 => this spot has no shadow slot this frame (unshadowed).
    if (light.shadowParams.y < 0.0f)
    {
        return 1.0f;
    }

    // The TEXEL bias as spotlight_cs.hlsl's (shadowParams2.w > 0: .w = normal texels, .x = push texels).
    const bool texelBias = light.shadowParams2.w > 0.0f;
    float normalBias = texelBias ? 0.0f : light.shadowParams2.x;
    float depthBias = light.shadowParams.w;
    float3 Poff = P + N * normalBias;

    // Rung 2 / Step 21: sample the VSM page pool (matches spotlight_cs.hlsl's VSM branch).
    if (vsmParams.x != 0.0f)
    {
        const uint view = (uint)light.shadowParams4.z; // plan S9e: the light's VSM view
        return VsmSpotShadow(view, light.viewProj, Poff, camPosSky.xyz, vsmParams.y, depthBias,
                             VsmPageTable, VsmPool, ShadowSampler);
    }

    float texelWorld = 0.0f;
    if (texelBias)
    {
        const float cosOuter = max(light.directionCosOuter.w, 1e-3f);
        const float tanOuter = sqrt(saturate(1.0f - cosOuter * cosOuter)) / cosOuter;
        const float res = max(light.shadowAtlas.z / max(spotShadowInfo.z, 1e-9f), 1.0f);
        texelWorld = 2.0f * max(mul(float4(P, 1.0f), light.viewProj).w, 0.0f) * tanOuter / res;
        Poff = LocalShadowTexelOffset(P, Ng, normalize(light.positionRange.xyz - P), texelWorld,
                                      light.shadowParams2.w, light.shadowParams2.x);
    }

    float4 clip = mul(float4(Poff, 1.0f), light.viewProj);
    float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    float2 uv = ndc.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    const float linearInvRange = light.shadowParams4.y; // plan S4: the atlas's encoding, as spotlight_cs.hlsl's
    float depth = SpotReceiverDepth(clip, light.shadowParams3.x, linearInvRange);
    if (any(uv < 0.0f) || any(uv > 1.0f) || depth <= 0.0f || depth >= 1.0f)
    {
        return 1.0f;
    }

    float2 texel = spotShadowInfo.zw;
    uv = LocalShadowAtlasUV(uv, light.shadowAtlas.xy, light.shadowAtlas.zz, texel, 3.0f); // plan S9: its square of a page
    const bool fromPool = light.shadowAtlas.w < 0.0f; // plan S7: its picture in the cache pool (LocalShadowPool)
    const float page = fromPool ? -light.shadowAtlas.w - 1.0f : light.shadowAtlas.w;
    float3 uvw = float3(uv, page);
    if (light.shadowParams3.w != 0.0f) // plan S2: the spot pass's own filter (local_shadow_filter.hlsli)
    {
        const float n = light.shadowParams3.x;
        const float f = max(light.positionRange.w, n + 0.1f);
        const float ts = (texelBias ? 1.0f / max(kSpotRampTexels * texelWorld, 1e-4f) : light.shadowParams3.y)
                       * lerp(0.5f, 1.0f, saturate(dot(N, normalize(light.positionRange.xyz - P))));
        float s;
        [branch]
        if (fromPool)
        {
            s = SpotShadowTent6x6(LocalShadowPool, LinearSampler, uv, page, clip.w, ts, n, f, 1.0f / texel, texel, linearInvRange);
        }
        else
        {
            s = SpotShadowTent6x6(SpotShadowAtlas, LinearSampler, uv, page, clip.w, ts, n, f, 1.0f / texel, texel, linearInvRange);
        }
        return LocalShadowFinish(s, light.shadowParams3.z);
    }
    [branch]
    if (fromPool) { return ShadowPCF3x3Array(LocalShadowPool, uvw, depth - depthBias, texel); }
    return ShadowPCF3x3Array(SpotShadowAtlas, uvw, depth - depthBias, texel);
}

// Plan S3: UE's shadow fade, as spotlight_cs.hlsl's.
float SampleSpotShadow(const SpotLightData light, float3 P, float3 N, float3 Ng, float NdotL)
{
    return lerp(1.0f, SampleSpotShadowMap(light, P, N, Ng, NdotL), light.shadowParams2.z);
}

#if GLASS_MATERIAL
// What a ROUGH pane shows of the scene behind it: SceneOpaque has one mip, so the GGX lobe's spread is
// taken as a disc of 8 taps + the centre, rough^2 * 3 % of the frame's height across (the frosted
// shelf glass of the_bathroom_free is 0.73). Clear glass (< 1 px) reads one tap. Pre-exposed, as stored.
float3 SampleBehindGlass(float2 uv, float rough)
{
    const float radius = rough * rough * 0.03f;                       // UV units of the height
    const float3 centre = SceneOpaque.SampleLevel(LinearSampler, uv, 0).rgb;
    if (radius < screenSizeInv.w) { return centre; }
    static const float2 kDisc[8] = {
        float2( 0.707f,  0.707f), float2(-0.707f,  0.707f), float2(-0.707f, -0.707f), float2( 0.707f, -0.707f),
        float2( 0.5f,    0.0f),   float2(-0.5f,    0.0f),   float2( 0.0f,    0.5f),   float2( 0.0f,   -0.5f)
    };
    const float2 scale = float2(radius * screenSizeInv.z / screenSizeInv.w, radius);
    float3 acc = centre;
    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        acc += SceneOpaque.SampleLevel(LinearSampler, saturate(uv + kDisc[k] * scale), 0).rgb;
    }
    return acc * (1.0f / 9.0f);
}
#endif

struct PSOut
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
#ifdef EDITOR_OBJECT_ID
    uint objectId : SV_Target2;
#endif
};

[RootSignature(GLASS_RS)]
#if GLASS_MATERIAL
PSOut PSMain(VSOut i, bool isFront : SV_IsFrontFace)
#else
PSOut PSMain(VSOut i)
#endif
{
    float3 N = normalize(i.normalWS);
#if GLASS_MATERIAL
    if (!isFront) { N = -N; } // a two-sided slot (CULL_NONE) shades the face it shows
#endif
    const float3 geomN = N; // the local shadows' texel bias pushes along this, never the normal map's
    float3 camPos = camPosSky.xyz;
    float3 V = normalize(camPos - i.posWS);
    float ior = max(matExtra.x, 1.0f);
    float3 absorption = max(absorptionThickness.xyz, 0.0f.xxx);
    float thickness = max(absorptionThickness.w, 0.0f);
    float3 tint = saturate(tintRoughness.xyz);
    float rough = saturate(tintRoughness.w);
    float reflectionStrength = max(matExtra.y, 0.0f);
    float refractionDistortion = matExtra.z;
    float skyIntensity = camPosSky.w;
    const float skySpecMipCount = camDirWS.w;
    float normalInfo = matExtra.w;
    float normalStrength = max(refractionDistortion, 0.0f);

    float metal = 0.0f;
#if GLASS_MATERIAL
    // The slot's material, as the G-buffer reads it (gbuffer_common.hlsli FetchShadingValuesP).
    const float2 matUv = i.uv * matTexOffsScale.zw + matTexOffsScale.xy;
    const float4 albedoTex = (matTexFlags.x > 0.5f) ? MatAlbedo.Sample(MatSampler, matUv) : float4(1.0f, 1.0f, 1.0f, 1.0f);
    // The alpha-test SPLIT: the texels the cutout keeps were drawn opaque by the G-buffer (a glass
    // slot's G-buffer half); this pass draws what it removed. Cutoff -1 = no split, the slot is all glass.
    if (matSurface.w >= 0.0f && albedoTex.a * matBaseColor.a >= matSurface.w) { discard; }
    // glTF BLEND alpha is COVERAGE on top of the transmission model -- what three.js and Blender do
    // with it. Sketchfab's exporter writes a pane's opacity both as this alpha and as 1 - transmission
    // (the_bathroom_free's shelf 0.404 / 0.596, the tumblers 0.498 / 0.502): left out, the shelf was a
    // full sheet of blue glass where the source shows 60 % of the room through it untouched.
    const float coverage = saturate(albedoTex.a * matBaseColor.a);
    tint = saturate(albedoTex.rgb * matBaseColor.rgb);
    float2 mr = matSurface.xy;
    if (matTexFlags.y > 0.5f)
    {
#if MR_LAYOUT_GLTF
        const float2 mrTex = MatMR.Sample(MatSampler, matUv).bg;
#else
        const float2 mrTex = MatMR.Sample(MatSampler, matUv).rg;
#endif
        mr = lerp(mrTex, mrTex * matSurface.xy, matGlass.y);
    }
    metal = saturate(mr.x);
    rough = saturate(mr.y);
    const float transmissionShare = saturate(matSurface.z *
        ((matGlass.x > 0.5f) ? MatTransmission.Sample(MatSampler, matUv).r : 1.0f));
    if (matTexFlags.z > 0.5f)
    {
        const float3 T = normalize(i.tangentWS);
        const float3 B = normalize(i.bitangentWS);
#if NORMALMAP_IS_RG
        const float2 nrg = NormalMap.Sample(MatSampler, matUv).rg * 2.0f - 1.0f;
        const float3 nTS = normalize(float3(nrg * matTexFlags.w, max(sqrt(saturate(1.0f - dot(nrg, nrg))), 1e-4f)));
#else
        float3 nTS = NormalMap.Sample(MatSampler, matUv).xyz * 2.0f - 1.0f;
        nTS = normalize(float3(nTS.xy * matTexFlags.w, nTS.z));
#endif
        N = normalize(T * nTS.x + B * nTS.y + N * nTS.z);
    }
    bool useNormalMap = false; // the material's map, applied above
#else
    bool useNormalMap = (normalInfo > 0.5f) && (normalStrength > 0.0f);
#endif
    if (useNormalMap)
    {
        float3 baseN = N;
        float3 T = normalize(i.tangentWS);
        float3 B = normalize(i.bitangentWS);
#if NORMALMAP_IS_RG
        float2 nrg = NormalMap.Sample(LinearSampler, i.uv).rg * 2.0f - 1.0f;
        float nz = sqrt(saturate(1.0f - dot(nrg, nrg)));
        float3 nTS = normalize(float3(nrg * normalStrength, max(nz, 1e-4f)));
#else
        float3 nRGB = NormalMap.Sample(LinearSampler, i.uv).xyz * 2.0f - 1.0f;
        float3 nTS = float3(nRGB.xy * normalStrength, nRGB.z);
        nTS = normalize(nTS);
#endif
        N = normalize(T * nTS.x + B * nTS.y + baseN * nTS.z);
    }

    float3 sunDir = normalize(sunDirAmbient.xyz);
    float ambientIntensity = sunDirAmbient.w;
    float3 sunColor = sunColorExposure.xyz * sunColorExposure.w;

    // P16.9: `ambientIntensity` is the level's `ambient`, and `ambient` has always meant "this
    // FRACTION OF THE SUN'S COLOUR bounces around" -- lighting_cs's flat branch spells it out as
    // `ambient * ambientRgb` where ambientRgb IS the sun colour. Glass dropped the sun and kept the
    // fraction, so its ambient was an ABSOLUTE 0.05-0.1 with no light in it at all: fine while the
    // scene sat near 1, a thousandth of the frame once the sun was in lux.
    float3 diffuseAccum = tint * ambientIntensity * sunColor;
    float3 specAccum = 0.0f.xxx;

    // Directional light
    {
        BRDFInput bi;
        bi.albedo = tint;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = normalize(-sunDir);

        BRDFResult br = EvalBRDF(bi, preExposureParams.y, preExposureParams.z);   // the sun's disc (UE sphere light)
        // No sun (a closed cloud deck), no shadow lookup: its passes did not run (lighting_cs.hlsl).
        if (br.NdotL > 0.0f && any(sunColor > 0.0f))
        {
            float shadow = SampleShadowCSM(i.posWS, N, br.NdotL);
            float3 radiance = sunColor;
            diffuseAccum += br.diffBRDF * br.NdotL * radiance * shadow;
            specAccum += br.specBRDF * br.NdotL * radiance * shadow;
        }
    }

    uint pointCount = (uint)lightCounts.x;
    for (uint idx = 0; idx < pointCount; ++idx)
    {
        PointLightData light = PointLights[idx];
        float3 Lvec = light.position - i.posWS;
        float dist = length(Lvec);
        if (dist > light.radius || light.radius <= kEpsilon)
        {
            continue;
        }
        float3 L = Lvec / max(dist, kEpsilon);
        const float atten = LightDistanceAttenuation(dist, light.radius); // P16.5

        BRDFInput bi;
        bi.albedo = tint;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        BRDFResult br = EvalBRDF(bi);
        if (br.NdotL <= 0.0f)
        {
            continue;
        }

        float shadow = PointShadowFactor(light, i.posWS, N, geomN);
        if (shadow <= 0.0f)
        {
            continue;
        }

        float3 radiance = light.color * light.intensity * atten;
        diffuseAccum += br.diffBRDF * br.NdotL * radiance * shadow;
        specAccum += br.specBRDF * br.NdotL * radiance * shadow;
    }

    uint spotCount = (uint)lightCounts.y;
    for (uint idx = 0; idx < spotCount; ++idx)
    {
        SpotLightData light = SpotLights[idx];
        float3 Lvec = light.positionRange.xyz - i.posWS;
        float dist = length(Lvec);
        if (dist >= light.positionRange.w || light.positionRange.w <= kEpsilon)
        {
            continue;
        }
        float3 L = Lvec / max(dist, kEpsilon);

        float spotCos = dot(-L, light.directionCosOuter.xyz);
        if (spotCos <= light.directionCosOuter.w)
        {
            continue;
        }

        float angleAtten = saturate((spotCos - light.directionCosOuter.w) * light.shadowParams.z);
        angleAtten *= angleAtten;

        const float distAtten = LightDistanceAttenuation(dist, light.positionRange.w); // P16.5

        BRDFInput bi;
        bi.albedo = tint;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        BRDFResult br = EvalBRDF(bi);
        if (br.NdotL <= 0.0f)
        {
            continue;
        }

        float shadow = SampleSpotShadow(light, i.posWS, N, geomN, br.NdotL);
        float3 radiance = light.colorIntensity.xyz * light.colorIntensity.w * distAtten * angleAtten;
        diffuseAccum += br.diffBRDF * br.NdotL * radiance * shadow;
        specAccum += br.specBRDF * br.NdotL * radiance * shadow;
    }

    float3 reflectionDir = reflect(-V, N);
    float3 skyRefl = 0.0f.xxx;
    if (lightCounts.w > 0.5f)
    {
        // P5: the shared roughness <-> mip mapping (ibl_common.hlsli), so glass, water and opaque
        // surfaces all broaden their reflections by the same rule.
        skyRefl = (skySpecMipCount > 0.0f)
            ? SkySpecular.SampleLevel(EnvSampler, reflectionDir,
                                      IblMipFromRoughness(rough, skySpecMipCount)).rgb * skyIntensity
            : SkyboxTex.SampleLevel(EnvSampler, reflectionDir, rough * 5.0f).rgb * skyIntensity;
    }
    float3 envRefl = skyRefl;
    // S15b off-screen RT reflection: the GlassReflGbuffer + GlassReflections passes traced this
    // glass surface's reflection (on-screen AND off-screen recompute, via rt_reflections_cs) into
    // GlassReflection, premultiplied (rgb = radiance, a = coverage). Composite the skybox where the
    // ray missed (a < 1). Gated by lightCounts.z (reflection source is SSR/RT); when off,
    // GlassReflection is not produced this frame and is never read here. lightCounts.w
    // independently suppresses the cubemap in the true None mode.
#if !GLASS_MATERIAL
    // (A material slot is not rasterized into the glass-reflection G-buffer, so its reflection is the sky's.)
    if (lightCounts.z > 0.5f)
    {
        float2 reflUV = i.posH.xy * screenSizeInv.zw; // SV_POSITION pixel -> normalized screen UV
        float4 gr = GlassReflection.SampleLevel(LinearSampler, reflUV, 0);
        envRefl = gr.rgb + skyRefl * (1.0f - gr.a);
    }
#endif
    //return float4(envRefl, 1.0f);

    //ior = 1.0f;
    float eta = 1.0f / ior;
    float3 refrDir = refract(-V, N, eta);
    bool totalInternal = dot(refrDir, refrDir) < 1e-6f;

    //thickness = 0.01f;
    float3 refrColor = 0.0f.xxx;
    const float2 pixelUv = i.posH.xy * screenSizeInv.zw;
    float2 refrUv = pixelUv;
    if (!totalInternal)
    {
        float3 refrPosWS = i.posWS + refrDir * thickness;
        float4 clip = mul(mul(float4(refrPosWS, 1.0f), view), proj);
        float2 uv = clip.xy / max(clip.w, 1e-6f);
        uv = uv * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        uv = saturate(uv);
        refrUv = uv;
        // P16.1: this is a COPY OF SCENE COLOUR and is therefore already pre-exposed, while
        // everything else in this shader is raw radiance. Bring it back to raw here; the whole
        // result is scaled once at the end. Scaling it twice is a plain brightness error on
        // whatever is seen THROUGH the glass, which is most of what glass shows.
        refrColor = SceneOpaque.Sample(LinearSampler, uv).rgb / max(preExposureParams.x, 1.0e-8f);
    }

#if GLASS_MATERIAL
    // UE ThinTranslucent / glTF transmission, thin-walled. The interface reflects Fenv (roughness-aware
    // Schlick, Fdez-Aguera: a frosted pane does not turn into a mirror at grazing angles); of the rest,
    // the transmission share goes THROUGH tinted by the base colour, and 1 - share is the surface's own
    // diffuse, lit as an opaque one. A metal transmits nothing.
    const float invPreExposure = 1.0f / max(preExposureParams.x, 1.0e-8f);
    const float cosV = saturate(dot(N, V));
    const float r0g = (ior - 1.0f) / (ior + 1.0f);
    const float3 F0g = lerp((r0g * r0g).xxx, tint, metal);
    const float3 Fenv = totalInternal ? 1.0f.xxx
                                      : F0g + (max((1.0f - rough).xxx, F0g) - F0g) * pow(1.0f - cosV, 5.0f);
    const float3 surface = (1.0f - transmissionShare) * diffuseAccum + specAccum + envRefl * Fenv;
    const float3 through = transmissionShare * (1.0f - metal) * (1.0f - Fenv) * tint;
    const float throughAvg = (through.x + through.y + through.z) * (1.0f / 3.0f);
    // ONE / INV_SRC_ALPHA keeps throughAvg of what the target already holds -- the opaque scene AND any
    // glass drawn behind this one. The tint's colour and the refraction's bend are put on here against
    // the opaque copy: exact where only opaque lies behind, the tint's average where more glass does.
    const float3 behind = SampleBehindGlass(refrUv, rough) * invPreExposure;
    const float3 behindHere = SceneOpaque.SampleLevel(LinearSampler, pixelUv, 0).rgb * invPreExposure;
#else
    //return float4(refrColor, 1.0f);
    //absorption = float3(0.16f, 0.07f, 0.03f) * 1.0f;
    float3 transmittance = exp(-absorption * thickness);
    refrColor *= transmittance;
    //return float4(refrColor, 1.0f);

    float cosTheta = saturate(dot(N, V));
    float r0 = (ior - 1.0f) / (ior + 1.0f);
    float3 F0 = float3(r0 * r0, r0 * r0, r0 * r0);
    float3 F = F_Schlick(cosTheta, F0);
    if (totalInternal)
    {
        F = 1.0f.xxx;
    }

    float3 lightingColor = diffuseAccum + specAccum;
    float3 color = lightingColor + refrColor * (1.0f - F) + envRefl * (reflectionStrength * F);

    //color = diffuseAccum;

	color = max(color, 0.0f.xxx);

    float transAvg = (transmittance.x + transmittance.y + transmittance.z) * (1.0f / 3.0f);
    float Favg = (F.x + F.y + F.z) * (1.0f / 3.0f);
    float alpha = saturate(1.0f - (1.0f - Favg) * transAvg);

    color = lerp(refrColor, color, alpha);
    //color = alpha.xxx;
    //alpha = 1.0f;
    //color = envRefl;
    //color = diffuseAccum;
#endif

    // Volumetric fog (plan A5): the air between the camera and this glass, the same split compose
    // applies to the opaque scene -- the froxel volume to its far plane, the analytic medium beyond
    // (its start pushed out along the ray, UE CombineVolumetricFog). Applied to the FINAL colour
    // (refraction included: what is seen through the glass is behind the same air as the glass).
    // GLASS_MATERIAL: only on the surface's own light -- what shows through it is the scene copy,
    // which compose has already fogged along the whole ray, this glass's stretch of air included.
    float fogTransmit = 1.0f;
    float3 fogAdd = 0.0f.xxx;
    {
        const float3 toPoint = i.posWS - camPos;
        const float dist = length(toPoint);
        const float3 viewDir = dist > 1.0e-4f ? toPoint / dist : float3(0.0f, 0.0f, 1.0f);
        const float viewDepth = max(i.clipPos.w, 1.0e-4f);
        const float2 fogUv = i.posH.xy * screenSizeInv.zw;
        const float4 vol = FogVolumeSampleAt(FogVolume, LinearSampler, fogUv, viewDepth, fogVolumeParams, fogVolumeZParams);
        float fogT = 1.0f;
        float3 fogIn = 0.0f.xxx;
        if (fogParams0.x > 0.0f)
        {
            HeightFogParams fog;
            fog.density = fogParams0.x;
            fog.heightFalloff = fogParams0.y;
            fog.referenceHeight = fogParams0.z;
            fog.startDistance = max(fogParams0.w, FogAnalyticExclude(fogVolumeParams, dist, viewDepth));
            fog.maxOpacity = fogParams1.x;
            fog.sunScatterStrength = fogParams1.y;
            fog.sunScatterExponent = fogParams1.z;
            fog.sunScatterStartDistance = max(fogParams1.w, FogAnalyticExclude(fogVolumeParams, dist, viewDepth));
                const float fogShared = HeightFogSharedIntegral(dist, camPos.y, i.posWS.y, fog);
            const float tau = HeightFogOpticalDepth(fogShared, dist, fog);
            const float minT = HeightFogMinTransmittance(tau, fog.maxOpacity);
            fogT = HeightFogTransmittance(tau, minT);
            const float headroom = HeightFogHeadroom(fogT, minT);
            const float3 skyAlongView = FogSkyAlongView(SkyboxTex, EnvSampler, viewDir, dist,
                HeightFogSkyRoughness(headroom, fogParams2.x), fogParams2.zw, skyIntensity);
            const float3 toSun = -normalize(sunDirAmbient.xyz);
            fogIn = HeightFogInscatter(skyAlongView, sunColorExposure.xyz * sunColorExposure.w,
                                        dot(viewDir, toSun), fogShared, dist, headroom, fog);
        }
        // == (c * fogT + fogIn * (1 - fogT)) * vol.a + vol.rgb, split into its two factors
        fogTransmit = fogT * vol.a;
        fogAdd = fogIn * (1.0f - fogT) * vol.a + vol.rgb;
    }
#if GLASS_MATERIAL
    // The surface covers 1 - throughAvg of the pixel, so it takes that share of the air's in-scatter
    // (the rest is already in the fogged scene behind it). Coverage then scales the whole glass over
    // what is behind: target = c * glass + (1 - c) * target.
    const float3 color = coverage * (max(surface * fogTransmit + fogAdd * (1.0f - throughAvg), 0.0f.xxx)
                                     + through * behind - throughAvg * behindHere);
    const float outAlpha = coverage * (1.0f - throughAvg);
#else
    color = color * fogTransmit + fogAdd;
    const float outAlpha = 1.0f;
#endif

    float2 currUv = ClipToUV(i.clipPos);
    float2 prevUv = ClipToUV(i.prevPosH);
    float2 motion = currUv - prevUv;

    PSOut o;
    // P16.1: the one place the factor goes on. Alpha is a blend weight, not a colour, so it stays.
    o.color = float4(color * preExposureParams.x, outAlpha);
    o.velocity = motion;
#ifdef EDITOR_OBJECT_ID
    o.objectId = objectId;
#endif
    return o;
}

#if GLASS_MATERIAL
// The glass slot's DEPTH PREPASS (GBufferRenderable::RenderTranslucent, colour writes off, depth on):
// a cup has two surfaces facing the camera on most pixels -- the near wall's outside and the far
// wall's inside -- and drawn in index order without depth the far one blended OVER the near one: the
// cup looked inside out. The prepass leaves the object's nearest glass surface in the depth buffer, so
// the colour pass (GREATER_EQUAL, same VS) shades exactly that one (HDRP's transparent depth prepass).
// The split's opaque texels are skipped: the G-buffer already put them there.
[RootSignature(GLASS_RS)]
void PSDepthOnly(VSOut i)
{
    const float2 matUv = i.uv * matTexOffsScale.zw + matTexOffsScale.xy;
    const float alpha = (matTexFlags.x > 0.5f) ? MatAlbedo.Sample(MatSampler, matUv).a : 1.0f;
    if (matSurface.w >= 0.0f && alpha * matBaseColor.a >= matSurface.w) { discard; }
}
#endif
