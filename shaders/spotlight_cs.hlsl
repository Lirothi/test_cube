#define SPOTLIGHT_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=10, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE))"
// t0..t3 : GBuffer (GB0, GB1, GB2, GBVelocity)
// t4     : Depth
// t5     : Texture2DArray Spot Shadow Atlas
// t6     : StructuredBuffer<SpotLightData>
// t7     : StructuredBuffer<uint> VSM page table (Rung 2 / Step 21)
// t8     : Texture2D VSM physical page pool depth
// t9     : GBAux (AO, indirect specular scale, shading model)
// u0     : Light accumulation RWTexture2D
// s0     : LinearClamp
// s1     : PointClamp
// s2     : ComparisonLinearClamp

#pragma pack_matrix(row_major)

#include "utils.hlsli"
#include "vsm_sample.hlsli"
#include "contact_shadow.hlsli"
#include "local_shadow_bias.hlsli"
#include "local_shadow_filter.hlsli" // plan S2: UE's spot filter

struct SpotLightData
{
    float4 positionRange;      // xyz = position, w = range
    float4 directionCosOuter;  // xyz = direction, w = cos(outer)
    float4 colorIntensity;     // xyz = color, w = intensity
    float4 shadowParams;       // x = cos(inner), y = shadow index, z = invAngleRange, w = depth bias
    float4 shadowParams2;      // x = normal bias (world units), y = volumetric scattering, z = UE's shadow fade (S3),
                               // w reserved
    float4x4 viewProj;
    float4 shadowParams3;      // S2: x = near, y = soft-transition scale (1/m), z = sharpen, w = 1 UE's tent / 0 old box
    float4 shadowAtlas;        // S9: its square in its atlas page, page UV: xy = corner, z = size, w = page
    float4 shadowParams4;      // S10e: x = the light's VSM mip bias on top of the frame's (UE's moving bias);
                               // S4: y = 1 / (f - n) when the atlas holds the LINEAR depth, 0 = perspective;
                               // S9e: z = its VSM view (vsm::SpotView of its slot)
};

Texture2D GB0 : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D GBVelocity : register(t3);
Texture2D DepthT : register(t4);
Texture2DArray SpotShadowAtlas : register(t5);
StructuredBuffer<SpotLightData> SpotLights : register(t6);
StructuredBuffer<uint> VsmPageTable : register(t7); // Rung 2 / Step 21
Texture2D VsmPool : register(t8);
Texture2D GBAux : register(t9);
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpLinear : register(s0);
SamplerState gSmpPoint : register(s1);
SamplerComparisonState gSmpShadow : register(s2);

cbuffer SpotLightFrame : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float3 camPosWS;
    uint   lightCount;
    float2 screenSize;
    float2 invScreenSize;
    float2 invShadowSize;
    uint   useVsm;      // Rung 2 / Step 21: sample the VSM page pool instead of the atlas
    float  vsmRefDist;  // VSM mip level-select reference distance
    // VSM local-light shadow bias in shadow-texel units (live-tunable, vsm::g_local*Texels). Lateral =
    // surface-normal offset (~1 texel => Peter-panning at most a texel); depth push = along-the-light-
    // ray, slope-scaled, so grazing acne clears without moving flat lit ground. Per-light
    // shadowNormalBias/shadowDepthBias now only drive the Legacy (atlas) path.
    float  localLateralTexels;
    float  localDepthPushTexels;
    uint   vsmTexelBias;  // plan S10b: 1 = the push in texels of the level actually sampled, along the geometric normal
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
    // Plan S10e: the VSM local mip (vsm_addressing.hlsli VsmLocalMipFromPixel, UE's): x = 1 by the camera pixel's
    // footprint (0 = by camera distance), y = the lod bias (ResolutionLodBiasLocal + dynamic resolution), z = the
    // pixel radius per unit of view depth (min(1 / (W P00), 1 / (H P11)) at the render resolution).
    float4 vsmLocalMip;
};
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


float SampleShadowPCF(float3 uvw, float depth, float2 texel)
{
    float shadow = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texel;
            shadow += SpotShadowAtlas.SampleCmpLevelZero(gSmpShadow, float3(uvw.xy + offset, uvw.z), depth);
        }
    }
    return shadow / 9.0f;
}

// `Ng` = the geometric normal (LocalShadowGeometricNormal) -- the texel bias's push; `N` (shading) everywhere else.
float ComputeSpotShadowMap(const SpotLightData light, float3 P, float3 N, float3 Ng, float NdotL)
{
    // shadowParams.y < 0 => this spot has no shadow slot this frame (unshadowed).
    if (light.shadowParams.y < 0.0f)
    {
        return 1.0f;
    }

    // The TEXEL bias (local_shadow_bias.hlsli) when shadowParams2.w > 0: .w = normal texels, .x = push texels -- the
    // world normal bias .x carries otherwise is then not one.
    const bool texelBias = light.shadowParams2.w > 0.0f;
    float normalBias = texelBias ? 0.0f : light.shadowParams2.x;
    float depthBias = light.shadowParams.w;
    float3 Poff = P + N * normalBias;

    // Rung 2 / Step 21: sample through the VSM page table + physical pool instead of the atlas.
    if (useVsm != 0u)
    {
        // Coarse VSM mip levels (far from the camera) cover 2-4x more world per texel than the 512^2
        // atlas the constant normalBias was tuned for, so that fixed offset no longer clears the
        // texel -> the receiver self-shadows into acne STRIPES on grazing ground. Scale the normal
        // offset by the LOD coarsening (continuous ~distCam/refDist, the SAME quantity that selects
        // the mip level), clamped to the coarsest level. Continuous (no 2x step) => no ring at a
        // level boundary. Mirrors the directional clipmap's distance-scaled offset.
        // Bias in WORLD space with ZERO NDC depth bias. An NDC bias is crush-sensitive: a spot's
        // perspective depth compresses to ~[0.95,1] and the crush scales with far/near, so a fixed
        // NDC bias becomes a huge world-space Peter-pan for wide/long spots (shadow detaches metres
        // off the base). Instead size the bias to the ACTUAL shadow texel at the receiver -- the
        // perspective half-width (distToLight * tan(outer)) over the virtual resolution at the
        // camera-selected level. That captures the spot's cone width, which the old camera-distance
        // biasScale ignored (wide spots were under-biased -> the acne 'stripes'). The push is ALONG
        // the light ray (depth-only, no lateral texel shift) and slope-scaled by 1/N.L so grazing
        // surfaces (where acne lives) get the depth while flat lit ground (where Peter-panning shows)
        // stays put. A small ~1-texel normal offset handles the rest.
        // Plan S10b (vsmTexelBias): the texel of the level the sampler will ACTUALLY read -- a page that fell back to a
        // coarser level has bigger texels, and a push sized for the requested level left the whole page in acne (hatched
        // quads with bright seams) -- along the GEOMETRIC normal (a normal map's ripple tilts N off a grazing light and
        // leaves a ripple of acne at any push; plan S9 found it on the atlases).
        const uint  view       = (uint)light.shadowParams4.z; // plan S9e: the light's VSM view
        const bool  texelFix   = vsmTexelBias != 0u;
        const float cosOuter   = max(light.directionCosOuter.w, 1e-3f);
        const float tanOuter   = sqrt(saturate(1.0f - cosOuter * cosOuter)) / cosOuter;
        // Plan S10e: the mip the request pass asked for -- UE's pixel footprint (the projection's x scale is
        // 1 / tan(outer): fov 2 x outer, aspect 1), or the old camera distance.
        uint reqLevel;
        if (vsmLocalMip.x != 0.0f)
        {
            const float viewDepth = dot(P - camPosWS, invView[2].xyz);
            const float lightDepth = mul(float4(P, 1.0f), light.viewProj).w;
            reqLevel = VsmLocalMipFromPixel(viewDepth * vsmLocalMip.z, 1.0f / max(tanOuter, 1e-4f), lightDepth,
                                            vsmLocalMip.y + light.shadowParams4.x); // S10e: + the light's moving bias
        }
        else
        {
            reqLevel = VsmSelectLevel(length(P - camPosWS), vsmRefDist, VSM_MAX_LEVEL);
        }
        const uint  lvl        = texelFix ? VsmSpotSampledLevel(view, reqLevel, light.viewProj, P, VsmPageTable) : reqLevel;
        const float3 Nb        = texelFix ? Ng : N;
        const float distLight  = length(light.positionRange.xyz - P);
        const float texelWorld = (2.0f * distLight * tanOuter / VSM_VIRTUAL_RES) * exp2((float)lvl);
        const float3 toL       = normalize(light.positionRange.xyz - P);
        const float  ndl       = saturate(dot(Nb, toL));
        const float  slope     = clamp(1.0f / max(ndl, 0.15f), 1.0f, 6.0f);
        float3 PoffV = P + Nb * (texelWorld * localLateralTexels)
                         + toL * (texelWorld * slope * localDepthPushTexels);
        return VsmSpotShadowFrom(view, reqLevel, light.viewProj, PoffV, 0.0f, VsmPageTable, VsmPool, gSmpShadow);
    }

    // TEXEL bias: one shadow texel at the receiver is 2 w tan(outer) over the light's resolution (its square in the page).
    float texelWorld = 0.0f;
    if (texelBias)
    {
        const float cosOuter = max(light.directionCosOuter.w, 1e-3f);
        const float tanOuter = sqrt(saturate(1.0f - cosOuter * cosOuter)) / cosOuter;
        const float res = max(light.shadowAtlas.z / max(invShadowSize.x, 1e-9f), 1.0f);
        texelWorld = 2.0f * max(mul(float4(P, 1.0f), light.viewProj).w, 0.0f) * tanOuter / res;
        Poff = LocalShadowTexelOffset(P, Ng, normalize(light.positionRange.xyz - P), texelWorld,
                                      light.shadowParams2.w, light.shadowParams2.x);
    }

    float4 clip = mul(float4(Poff, 1.0f), light.viewProj);
    float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    float2 uv = ndc.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    // Plan S4: the atlas's encoding -- the linear (w - n) / (f - n) or the perspective NDC z.
    const float linearInvRange = light.shadowParams4.y;
    float depth = SpotReceiverDepth(clip, light.shadowParams3.x, linearInvRange);

    if (any(uv < 0.0f) || any(uv > 1.0f) || depth <= 0.0f || depth >= 1.0f)
    {
        return 1.0f;
    }
    // Plan S4: the light drew into the top-left square of its own resolution; 3 texels inside it for the 6x6 tent.
    // Plan S9: its square of an atlas page (shadowAtlas); `invShadowSize` is one page texel.
    uv = LocalShadowAtlasUV(uv, light.shadowAtlas.xy, light.shadowAtlas.zz, invShadowSize, 3.0f);
    const float page = light.shadowAtlas.w;

    // Plan S2: UE's Manual5x5PCF with its soft transition, the ramp narrowed toward the light by the receiver's NoL
    // (UE: TransitionScale x lerp(1 - SpotLightReceiverBias 0.5, 1, NoL), ShadowProjectionPixelShader.usf:248), then
    // sharpen and square. clip.w is the receiver's linear depth along the light's axis.
    if (light.shadowParams3.w != 0.0f)
    {
        const float n = light.shadowParams3.x;
        const float f = max(light.positionRange.w, n + 0.1f); // SpotLight::UpdateCachedData's far
        const float NoL = saturate(dot(N, normalize(light.positionRange.xyz - P)));
        // The ramp: UE's (f - n) / 60 metres at bias 0.5 (~23 cm on a 14 m spot -- a shadow fading in that far behind
        // its caster reads as a gap at the foot), or kSpotRampTexels shadow texels under the TEXEL bias.
        const float ts = (texelBias ? 1.0f / max(kSpotRampTexels * texelWorld, 1e-4f) : light.shadowParams3.y)
                       * lerp(0.5f, 1.0f, NoL);
        const float s = SpotShadowTent6x6(SpotShadowAtlas, gSmpPoint, uv, page, clip.w, ts, n, f,
                                          1.0f / invShadowSize, invShadowSize, linearInvRange);
        return LocalShadowFinish(s, light.shadowParams3.z);
    }
    float3 uvw = float3(uv, page);
    return SampleShadowPCF(uvw, depth - depthBias, invShadowSize);
}

// Plan S3: UE's shadow fade by the resolution the light wants on screen (LightManager::SelectShadowedSpots; 1 under VSM).
float ComputeSpotShadow(const SpotLightData light, float3 P, float3 N, float3 Ng, float NdotL)
{
    return lerp(1.0f, ComputeSpotShadowMap(light, P, N, Ng, NdotL), light.shadowParams2.z);
}

// TILED LIGHT CULLING (2026-10-21, a hundred lamps on wind_test) -- pointlight_cs.hlsl's, the spot's reach sphere
// against the tile's box: a pixel walks only the tile's lights, in index order, so the sum is the old one bit for bit.
static const uint kTileLights = 1024u;           // the mask's lights; a frame with more walks every light, as before
static const uint kTileWords = kTileLights / 32u;
static const uint kLightWalkCeiling = 65536u;    // the literal bound of the per-pixel walk (the CB's count stops it)
groupshared uint gsTileMask[kTileWords];
groupshared uint gsTileZNear; // device depth bits (reverse-Z): the nearest is the largest
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
            const float4 pr = SpotLights[i].positionRange;
            const float3 d = max(max(gsTileMin - pr.xyz, pr.xyz - gsTileMax), 0.0f);
            const float reach = pr.w * 1.001f + 1e-3f; // the corners' rounding: never lose a light a pixel keeps
            if (pr.w > kEpsilon && dot(d, d) <= reach * reach)
            {
                InterlockedOr(gsTileMask[i >> 5u], 1u << (i & 31u));
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

[numthreads(8, 8, 1)]
[RootSignature(SPOTLIGHT_CS_RS)]
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
        SpotLightData light = SpotLights[i];
        float3 Lvec = light.positionRange.xyz - P;
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
        angleAtten = angleAtten * angleAtten;

        const float distAtten = LightDistanceAttenuation(dist, light.positionRange.w); // P16.5

        BRDFInput bi;
        bi.albedo = albedo;
        bi.rough = rough;
        bi.metal = metal;
        bi.N = N;
        bi.V = V;
        bi.L = L;

        const float3 radiance = light.colorIntensity.xyz * light.colorIntensity.w * distAtten * angleAtten;

        if (isFoliage)
        {
            // F5: shared F4 foliage helper -> spot lights match the directional/point model.
            // Cone + distance attenuation and shadow apply to the front lobe and transmission.
            FoliageResult fr = EvalFoliageBRDF(
                bi, subsurface, 0.0f, transmissionNormalWeight);
            if (fr.NdotL > 0.0f)
            {
                bool useMap, useContact;
                LocalShadowSources(light.shadowParams.y >= 0.0f, useMap, useContact);
                float shadow = useMap ? ComputeSpotShadow(light, P, N, Ng, fr.NdotL) : 1.0f;
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
            if (any(fr.transBRDF > 0.0f))
            {
                bool useMapT, useContactT;
                LocalShadowSources(light.shadowParams.y >= 0.0f, useMapT, useContactT);
                float shadowT = useMapT ? ComputeSpotShadow(light, P, -N, -Ng, saturate(dot(-N, L))) : 1.0f;
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
            if (br.NdotL <= 0.0f)
            {
                continue;
            }

            bool useMap, useContact;
            LocalShadowSources(light.shadowParams.y >= 0.0f, useMap, useContact);
            float shadow = useMap ? ComputeSpotShadow(light, P, N, Ng, br.NdotL) : 1.0f;
            if (useContact)
            {
                shadow = ApplyContactShadow(shadow, P, N, L, br.NdotL, dispatchThreadId.xy, camPosWS,
                                            viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                                            MakeContactParams());
            }
            accum += (br.diffBRDF + br.specBRDF) * br.NdotL * radiance * shadow;
        }
    }

    LightTarget[dispatchThreadId.xy] = float4(base.rgb + accum * lightPreExposure, base.a);
}

