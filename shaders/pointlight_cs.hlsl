#define POINTLIGHT_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=11, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE))"
// t0..t3 : GBuffer (GB0, GB1, GB2, GBVelocity)
// t4     : Depth
// t5     : StructuredBuffer<PointLightData>
// t6     : Texture2DArray point shadow faces, 6 per light (D16 -> R16), B2b; plan S4: a 2D array, not a cube
// t7     : StructuredBuffer<uint> VSM page table (Rung 2 / Step 21)
// t8     : Texture2D VSM physical page pool depth
// t9     : GBAux (AO, indirect specular scale, shading model)
// u0     : Light accumulation RWTexture2D
// s0     : LinearClamp
// s1     : PointClamp
// s2     : ComparisonLinearClamp (shadow compare)

#pragma pack_matrix(row_major)

#include "utils.hlsli"
#include "vsm_sample.hlsli"
#include "contact_shadow.hlsli"
#include "local_shadow_bias.hlsli"
#include "local_shadow_filter.hlsli" // plan S2: UE's point filter

struct PointLightData
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
    float4 shadowParams; // x = shadow slot (-1 = none), y = bias, z = nearPlane, w = farPlane (radius)
    float4 volumetric;   // x = volumetric scattering intensity (plan A4d), y = UE slope scale (S1), z = sharpen, w = filter (S2)
    float4 shadowFade;   // x = UE's shadow fade (S3), y = the atlas page (S9), z = 1 / its face res (S4)
    float4 shadowAtlas;  // S9: its 3f x 2f block in the page, page UV: xy = corner, zw = one face -- mirrors LightManager::PointLightGpu
    float4 shadowParams4; // S10e: x = the light's VSM mip bias on top of the frame's (UE's moving bias);
                          // S4: y = 1 / (f - n) when the faces hold the LINEAR depth, 0 = perspective;
                          // S9e: z = the VSM view of its +X face (vsm::PointFaceView of slot * 6; the others follow)
};

Texture2D GB0 : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D GBVelocity : register(t3);
Texture2D DepthT : register(t4);
StructuredBuffer<PointLightData> PointLights : register(t5);
Texture2DArray PointShadowAtlas : register(t6); // plans S4 / S9: pages, each light a 3f x 2f block (PointShadowCubeUV)
StructuredBuffer<uint> VsmPageTable : register(t7); // Rung 2 / Step 21
Texture2D VsmPool : register(t8);
Texture2D GBAux : register(t9);
// Plan S7, the direct read: the local shadow cache's pool -- a light whose shadowFade.y is negative is its 3f x 2f block
// there (page -y - 1; shadowAtlas in the pool page's UV). The atlas stands in while there is no pool.
Texture2DArray LocalShadowPool : register(t10);
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpLinear : register(s0);
SamplerState gSmpPoint : register(s1);
SamplerComparisonState gSmpShadowCmp : register(s2);

cbuffer PointLightFrame : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float3 camPosWS;
    uint lightCount;
    float2 screenSize;
    float2 invScreenSize;
    float invPointShadowSize; // 1 / pointShadowRes, the largest face (plan S9: the shaders take a light's page texel from its own data)
    uint   useVsm;      // Rung 2 / Step 21: sample the VSM page pool instead of the cube atlas
    float  vsmRefDist;  // VSM mip level-select reference distance
    // VSM local-light shadow bias in shadow-texel units (live-tunable, vsm::g_local*Texels) — same
    // model as the spot path. Lateral normal offset + slope-scaled along-ray depth push.
    float  localLateralTexels;
    float  localDepthPushTexels;
    uint   vsmTexelBias;  // plan S10b: 1 = the push in texels of the level actually sampled, along the geometric normal
    float  _vsmPad1;
    float  _vsmPad2;
    // Contact shadows (docs/csm_improvement_plan.md S12). Same names in every light pass.
    float4x4 viewProj;           // camera world -> clip
    float4x4 projMatrix;         // camera view -> clip; the contact ray's compare tolerance
    float contactShadowLength;
    float contactShadowIntensity;
    uint  contactShadowSteps;
    uint  contactShadowLengthInWS;
    float contactShadowNormalOffset;
    float contactShadowGrazingFade;
    float contactShadowMinDist;
    float contactShadowMaxDist;
    float contactShadowFadeBand;
    float contactShadowThickness;
    uint  contactShadowFrameId;
    // LOCAL LIGHTS ONLY. 0 = shadow map, contacts off here. 1 = contacts INSTEAD of the shadow
    // map (the map is not even sampled). 2 = contacts only where the light has no shadow slot.
    // Stacking both on a small-range light darkens the same contact twice and buys nothing.
    uint  contactShadowLocalMode;
    // The light target is PRE-EXPOSED (lighting_cb.hlsli lightPreExposure): what this pass adds is
    // scaled by it, the base it reads already is.
    float lightPreExposure;
    // Plan S10e: the VSM local mip -- the spot pass's vsmLocalMip (x = pixel-footprint mip on, y = lod bias, z = the
    // pixel radius per unit of view depth).
    float4 vsmLocalMip;
}
ContactShadowParams MakeContactParams()
{
    ContactShadowParams cp;
    cp.length = contactShadowLength;
    cp.intensity = contactShadowIntensity;
    cp.steps = contactShadowSteps;
    cp.lengthInWS = contactShadowLengthInWS;
    cp.normalOffset = contactShadowNormalOffset;
    cp.grazingFade = contactShadowGrazingFade;
    cp.minDist = contactShadowMinDist;
    cp.maxDist = contactShadowMaxDist;
    cp.fadeBand = contactShadowFadeBand;
    cp.thickness = contactShadowThickness;
    cp.frameId = contactShadowFrameId;
    return cp;
}

// Which of the two shadow sources this light uses, by contactShadowLocalMode (see the CB).
void LocalShadowSources(bool hasSlot, out bool useMap, out bool useContact)
{
    useMap = true; useContact = false;
    if (contactShadowLocalMode == 1u)      { useMap = false;   useContact = true; }
    else if (contactShadowLocalMode == 2u) { useMap = hasSlot; useContact = !hasSlot; }
    else if (contactShadowLocalMode == 3u) { useMap = true;    useContact = true; } // plan S8: UE's, the trace on top
}


// Omnidirectional (cube) point shadow, depth-cube approach (B3). Reconstructs the
// standard-projection compare depth from the world offset (matches the render:
// PerspectiveFovLH(90,1,near,far), LESS_EQUAL, clear 1.0 = far — NOT reverse-Z), then
// SampleCmpLevelZero on the light's face (PointShadowCubeUV). Returns 1 (lit) .. 0 (fully shadowed). The bias is
// local_shadow_bias.hlsli's (UE's, plan S1; the old world-unit one behind --set shadow.localBiasUe:0).
// PCF mirrors the spot path (spotlight_cs.hlsl SampleShadowPCF): a 3x3 texel grid, /9. Spot
// offsets the UV by `invShadowSize`; here the coord is a direction, so we step in the tangent
// plane of d by one texel = 2*m*invRes world units — a 90-deg cube face spans 2*m at view
// depth m across `res` texels, and UV[0,1] <-> tangent[-1,1] gives the 2x. Plan S4: res is the light's own
// (shadowFade.z = 1/res). Plan S9: its six faces are one 3f x 2f block (shadowAtlas) of an atlas page (shadowFade.y);
// a page texel in UV is one face's size over res.
// `Ng` = the geometric normal (LocalShadowGeometricNormal) -- the texel bias's push; `N` (shading) everywhere else.
float PointShadowFactorMap(PointLightData Ld, float3 P, float3 N, float3 Ng)
{
    const float invRes = Ld.shadowFade.z;
    const float4 block = Ld.shadowAtlas;
    const bool fromPool = Ld.shadowFade.y < 0.0f; // plan S7: its block in the cache pool (LocalShadowPool)
    const float page = fromPool ? -Ld.shadowFade.y - 1.0f : Ld.shadowFade.y;
    const float2 pageTexel = block.zw * invRes;
    if (Ld.shadowParams.x < 0.0f) { return 1.0f; } // this light has no shadow slot this frame

    float3 Poff = P + N * ((Ld.shadowFade.w > 0.0f) ? 0.0f : PointShadowNormalOffset(Ld.shadowParams)); // the old bias only
    // Rung 2 / Step 21: sample the VSM page pool. Reconstructs the cube face's view-proj + looks up
    // the page table. World-space bias = pull the receiver toward the light before projecting (like
    // the atlas path, avoiding the crushed-perspective-depth bias problem).
    if (useVsm != 0u)
    {
        // Same texel-sized, slope-scaled, world-space bias as the spot path (ZERO NDC bias). Size
        // the offset to the ACTUAL shadow texel at the receiver so acne clears with the minimum
        // push at every distance/level -> minimal Peter-panning. A cube face is a fixed 90deg
        // frustum, so tan(halfFov)=1 and texelWorld = 2*distToLight/VSM_VIRTUAL_RES at the
        // camera-selected level. Push ALONG the light ray (depth-only, crush-immune) slope-scaled
        // by 1/N.L, plus a ~1-texel normal offset.
        // Plan S10b (vsmTexelBias): the texel of the level actually sampled, along the geometric normal (the spot path).
        const bool  texelFix   = vsmTexelBias != 0u;
        // Plan S10e: the mip the request pass asked for -- UE's pixel footprint (a face's projection has x scale 1: 90
        // degrees), or the old camera distance.
        const uint  reqLevel   = (vsmLocalMip.x != 0.0f)
            ? VsmLocalMipFromPixel(dot(P - camPosWS, invView[2].xyz) * vsmLocalMip.z, 1.0f,
                                   VsmPointFaceDepth(P, Ld.position), vsmLocalMip.y + Ld.shadowParams4.x) // S10e: + its moving bias
            : VsmSelectLevel(length(P - camPosWS), vsmRefDist, VSM_MAX_LEVEL);
        const uint  firstView  = (uint)Ld.shadowParams4.z; // plan S9e: the VSM view of its +X face
        const uint  lvl        = texelFix ? VsmPointSampledLevel(firstView, reqLevel, P, Ld.position,
                                                                 Ld.shadowParams.z, Ld.shadowParams.w, VsmPageTable)
                                          : reqLevel;
        const float3 Nb        = texelFix ? Ng : N;
        const float distLight  = length(Ld.position - P);
        const float texelWorld = (2.0f * distLight / VSM_VIRTUAL_RES) * exp2((float)lvl);
        const float3 toL       = normalize(Ld.position - P);
        const float  ndl       = saturate(dot(Nb, toL));
        const float  slope     = clamp(1.0f / max(ndl, 0.15f), 1.0f, 6.0f);
        float3 PoffV = P + Nb * (texelWorld * localLateralTexels)
                         + toL * (texelWorld * slope * localDepthPushTexels);
        return VsmPointShadowFrom(firstView, reqLevel, PoffV, Ld.position, Ld.shadowParams.z,
                                  Ld.shadowParams.w, 0.0f, VsmPageTable, VsmPool, gSmpShadowCmp);
    }

    // The TEXEL bias (local_shadow_bias.hlsli): the receiver moves by texels of THIS light's face at its distance,
    // the compare below then carries no bias at all (shadowParams.y = 0).
    if (Ld.shadowFade.w > 0.0f)
    {
        const float3 d0 = P - Ld.position;
        const float texelWorld = 2.0f * max(abs(d0.x), max(abs(d0.y), abs(d0.z))) * invRes;
        Poff = LocalShadowTexelOffset(P, Ng, normalize(Ld.position - P), texelWorld, Ld.shadowFade.w, Ld.volumetric.y);
    }
    float3 d = Poff - Ld.position; // HW picks the face from d
    float m = max(abs(d.x), max(abs(d.y), abs(d.z)));     // view-space Z on the selected face
    float nearP = Ld.shadowParams.z;
    float farP = max(Ld.shadowParams.w, nearP + 1e-3f);
    // The bias pulls the compare distance toward the light BEFORE projecting -- never a constant
    // NDC bias: perspective depth is crushed into [~0.95,1] at any real distance. UE's is a fraction
    // of the distance, slope-scaled by the receiver's NoL (local_shadow_bias.hlsli).
    const float NoL = dot(N, normalize(Ld.position - P));
    // Plan S2: UE's 29-tap disc (its bias per tap -- 0 under the texel bias), sharpened and squared.
    if (Ld.volumetric.w != 0.0f && (Ld.shadowParams.y < 0.0f || Ld.shadowFade.w > 0.0f))
    {
        float s;
        [branch]
        if (fromPool)
        {
            s = PointShadowDisc29(LocalShadowPool, gSmpShadowCmp, d, m, Ld.shadowParams, Ld.volumetric.y, NoL, invRes, block, page,
                                  pageTexel, Ld.shadowParams4.y);
        }
        else
        {
            s = PointShadowDisc29(PointShadowAtlas, gSmpShadowCmp, d, m, Ld.shadowParams, Ld.volumetric.y, NoL, invRes, block, page,
                                  pageTexel, Ld.shadowParams4.y);
        }
        return LocalShadowFinish(s, Ld.volumetric.z);
    }
    float mBiased = PointShadowCompareDistance(m, Ld.shadowParams, Ld.volumetric.y, NoL);
    float zc = PointShadowCompareDepth(mBiased, nearP, farP, Ld.shadowParams4.y); // plan S4: linear, or LH NDC

    // Tangent basis perpendicular to d (seed with the smallest-magnitude axis to avoid a
    // degenerate cross product), then a 3x3 grid stepped by one texel in world units.
    float3 ad = abs(d);
    float3 seed = (ad.x < ad.y && ad.x < ad.z) ? float3(1, 0, 0)
                : (ad.y < ad.z)                ? float3(0, 1, 0) : float3(0, 0, 1);
    float3 t1 = normalize(cross(seed, d));
    float3 t2 = normalize(cross(d, t1));
    float texel = 2.0f * m * invRes;

    float shadow = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float3 sd = d + (t1 * x + t2 * y) * texel;
            const float3 tap = PointShadowCubeUV(sd, block, page, pageTexel); // sd's face in the light's block (S4 / S9)
            shadow += fromPool ? LocalShadowPool.SampleCmpLevelZero(gSmpShadowCmp, tap, zc)
                               : PointShadowAtlas.SampleCmpLevelZero(gSmpShadowCmp, tap, zc);
        }
    }
    return shadow / 9.0f;
}

// Plan S3: UE's shadow fade by the resolution the light wants on screen (LightManager::SelectShadowedPoints; 1 under VSM).
float PointShadowFactor(PointLightData Ld, float3 P, float3 N, float3 Ng)
{
    return lerp(1.0f, PointShadowFactorMap(Ld, P, N, Ng), Ld.shadowFade.x);
}

// TILED LIGHT CULLING (2026-10-21, a hundred lamps on wind_test): the 8x8 group is a screen tile. Its receivers' depth
// range bounds it in world space (the box of the slab's 8 corners -- the slab is convex and in front of the eye, so it
// holds every receiver of the tile), the group tests each light's sphere against that box once, and a pixel walks only
// the tile's lights -- in index order, as the full loop did: the same lights reach the same per-pixel test in the same
// order, so the sum is the old one bit for bit. A tile of sky does no light work at all.
static const uint kTileLights = 1024u;           // the mask's lights; a frame with more walks every light, as before
static const uint kTileWords = kTileLights / 32u;
static const uint kLightWalkCeiling = 65536u;    // the literal bound of the per-pixel walk (the CB's count stops it)
groupshared uint gsTileMask[kTileWords];
groupshared uint gsTileZNear; // device depth bits (reverse-Z, positive floats order as uints): the nearest is the largest
groupshared uint gsTileZFar;
groupshared float3 gsTileMin;
groupshared float3 gsTileMax;

// Every thread of the group calls it, before any of them returns.
void BuildTileLights(uint groupIndex, uint2 groupId, float z, bool hasSurface, uint count)
{
    if (groupIndex == 0u) { gsTileZNear = 0u; gsTileZFar = 0x7F7FFFFFu; }
    for (uint w = groupIndex; w < kTileWords; w += 64u) { gsTileMask[w] = 0u; }
    GroupMemoryBarrierWithGroupSync();
    if (hasSurface)
    {
        uint unused;
        InterlockedMax(gsTileZNear, asuint(z), unused);
        InterlockedMin(gsTileZFar, asuint(z), unused);
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u && gsTileZNear != 0u)
    {
        const float2 uv0 = float2(groupId * 8u) * invScreenSize;
        const float2 uv1 = float2(groupId * 8u + 8u) * invScreenSize;
        float3 mn = float3(1e30f, 1e30f, 1e30f);
        float3 mx = -mn;
        [unroll] for (uint c = 0u; c < 8u; ++c)
        {
            const float2 cornerUv = float2((c & 1u) ? uv1.x : uv0.x, (c & 2u) ? uv1.y : uv0.y);
            const float3 p = ReconstructPosWS(cornerUv, asfloat((c & 4u) ? gsTileZNear : gsTileZFar), invProj, invView);
            mn = min(mn, p);
            mx = max(mx, p);
        }
        gsTileMin = mn;
        gsTileMax = mx;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gsTileZNear != 0u && count <= kTileLights)
    {
        for (uint i = groupIndex; i < kTileLights; i += 64u)
        {
            if (i >= count) { break; }
            const PointLightData Ld = PointLights[i];
            const float3 d = max(max(gsTileMin - Ld.position, Ld.position - gsTileMax), 0.0f);
            const float reach = Ld.radius * 1.001f + 1e-3f; // the corners' rounding: never lose a light a pixel keeps
            if (Ld.radius > kEpsilon && dot(d, d) <= reach * reach)
            {
                InterlockedOr(gsTileMask[i >> 5u], 1u << (i & 31u));
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

[numthreads(8,8,1)]
[RootSignature(POINTLIGHT_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint width = (uint)screenSize.x;
    uint height = (uint)screenSize.y;
    const bool inside = dispatchThreadId.x < width && dispatchThreadId.y < height;

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) * invScreenSize;
    float z = inside ? DepthT.SampleLevel(gSmpPoint, uv, 0).r : 0.0f;
    const uint count = (uint)lightCount;
    BuildTileLights(groupIndex, groupId.xy, z, inside && z > kEpsilon, count);
    if (!inside || z <= kEpsilon)
    {
        return;
    }

    float4 g0 = GB0.SampleLevel(gSmpLinear, uv, 0);
    float4 g1 = GB1.SampleLevel(gSmpLinear, uv, 0);

    float3 albedo = g0.rgb;
    float2 rm = UnpackRM(g0.a);
    float rough = rm.x;
    float metal = rm.y;
    float3 N = normalize(g1.rgb * 2.0 - 1.0);
    float3 P = ReconstructPosWS(uv, z, invProj, invView);
    float3 V = normalize(camPosWS - P);
    // The texel bias pushes along the geometry's normal, not the normal map's (local_shadow_bias.hlsli); VSM too since
    // plan S10b (vsmTexelBias), the old VSM bias keeps N.
    const float3 Ng = (useVsm == 0u || vsmTexelBias != 0u)
        ? LocalShadowGeometricNormal(DepthT, gSmpPoint, uv, invScreenSize, z, P, camPosWS, invProj, invView, N) : N;

    // F5: foliage shading model (GBAux.b is an ID -> point sample) + subsurface payload (GB2).
    const float4 gbAux = GBAux.SampleLevel(gSmpPoint, uv, 0);
    uint shadingModel = DecodeShadingModel(gbAux.b);
    bool isFoliage = (shadingModel == kShadingModelTwoSidedFoliage);
    const float transmissionNormalWeight = saturate(gbAux.a);
    float3 subsurface = 0.0f.xxx;
    if (isFoliage)
    {
        subsurface = GB2.SampleLevel(gSmpPoint, uv, 0).rgb;
    }

    float4 base = LightTarget[dispatchThreadId.xy];
    float3 accum = 0.0f.xxx;   // this pass's lights, in radiance; pre-exposed into `base` at the end

    // The tile's lights in index order (every light when there are more than the mask holds).
    const bool walkAll = count > kTileLights;
    uint word = 0u;
    uint bits = gsTileMask[0];
    [loop] for (uint k = 0u; k < kLightWalkCeiling; ++k)
    {
        uint i;
        if (walkAll)
        {
            if (k >= count) { break; }
            i = k;
        }
        else
        {
            [loop] while (bits == 0u && word + 1u < kTileWords) { ++word; bits = gsTileMask[word]; }
            if (bits == 0u) { break; }
            i = word * 32u + firstbitlow(bits);
            bits &= bits - 1u; // before the body: its `continue`s go to the next light
        }
        PointLightData Ld = PointLights[i];
        float3 Lvec = Ld.position - P;
        float dist = length(Lvec);
        if (dist > Ld.radius || Ld.radius <= kEpsilon)
        {
            continue;
        }
        float3 L = Lvec / max(dist, kEpsilon);

        const float atten = LightDistanceAttenuation(dist, Ld.radius); // P16.5

        BRDFInput bi;
        bi.albedo = albedo;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        const float3 radiance = Ld.color * Ld.intensity * atten;

        if (isFoliage)
        {
            // F5: reuse the shared F4 foliage helper so point lights match the directional model
            // exactly. Radius attenuation + shadow apply to both the front lobe and transmission.
            FoliageResult fr = EvalFoliageBRDF(
                bi, subsurface, 0.0f, transmissionNormalWeight);
            if (fr.NdotL > 0.0)
            {
                bool useMap, useContact;
                LocalShadowSources(Ld.shadowParams.x >= 0.0f, useMap, useContact);
                float shadow = useMap ? PointShadowFactor(Ld, P, N, Ng) : 1.0f;
                if (useContact)
                {
                    shadow = ApplyContactShadow(shadow, P, N, L, fr.NdotL, dispatchThreadId.xy, camPosWS,
                                                viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                                                MakeContactParams());
                }
                accum += (fr.diffBRDF + fr.specBRDF) * fr.NdotL * radiance * shadow;
            }
            // Transmission: flipped-normal shadow sample so the leaf's light-facing face does not
            // self-shadow the light passing through it (mirrors lighting_cs F4).
            if (any(fr.transBRDF > 0.0))
            {
                bool useMapT, useContactT;
                LocalShadowSources(Ld.shadowParams.x >= 0.0f, useMapT, useContactT);
                float shadowT = useMapT ? PointShadowFactor(Ld, P, -N, -Ng) : 1.0f;
                if (useContactT)
                {
                    shadowT = ApplyContactShadow(shadowT, P, -N, L, saturate(dot(-N, L)),
                                                 dispatchThreadId.xy, camPosWS,
                                                 viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                                                 MakeContactParams());
                }
                accum += fr.transBRDF * radiance * shadowT;
            }
        }
        else
        {
            BRDFResult br = EvalBRDF(bi);
            if (br.NdotL <= 0.0)
            {
                continue;
            }

            bool useMap, useContact;
            LocalShadowSources(Ld.shadowParams.x >= 0.0f, useMap, useContact);
            float shadow = useMap ? PointShadowFactor(Ld, P, N, Ng) : 1.0f;
            if (useContact)
            {
                shadow = ApplyContactShadow(shadow, P, N, L, br.NdotL, dispatchThreadId.xy, camPosWS,
                                            viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                                            MakeContactParams());
            }
            if (shadow <= 0.0f)
            {
                continue;
            }

            accum += (br.diffBRDF + br.specBRDF) * br.NdotL * radiance * shadow;
        }
    }

    LightTarget[dispatchThreadId.xy] = float4(base.rgb + accum * lightPreExposure, base.a);
}
