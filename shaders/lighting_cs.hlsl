// t0..t3 : GBuffer textures (GB0, GB1, GB2, GBVelocity)
// t4     : Depth (R32F)
// t5     : Shadow atlas
// t8     : GBAux (AO, indirect specular scale, shading model)
// t9     : Caustics flipbook atlas (inert when there is no ocean)
// u0     : Light accumulation target (RWTexture2D)
// s0     : PointClamp
// s1     : ComparisonLinearClamp
// s2     : LinearWrap (caustics)

#pragma pack_matrix(row_major)

#include "utils.hlsli"
#include "vsm_sample.hlsli"
#include "caustics.hlsli"

Texture2D GB0 : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D GBVelocity : register(t3);
Texture2D DepthT : register(t4);
Texture2D ShadowAtlas : register(t5);
StructuredBuffer<uint> VsmPageTable : register(t6); // Rung 2 / Step 24f: directional clipmap page table
Texture2D VsmPool : register(t7);                    // VSM physical page pool depth
Texture2D GBAux : register(t8);
Texture2D CausticsAtlas : register(t9);
// F8: cosine-convolved sky irradiance, already divided by PI, so a Lambertian surface multiplies
// its albedo by this directly. Inert unless `skyIrradianceEnabled` says the level has one.
TextureCube SkyIrradiance : register(t10);
// P6B item 7: dynamic screen-space AO at RENDER resolution. Read only when `gtaoEnabled`;
// the target is not written at all when the pass is off, so an unconditional read would be
// sampling whatever was left in it.
Texture2D GtaoTex : register(t11);
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpPoint : register(s0);
SamplerComparisonState gSmpLinear : register(s1);
SamplerState gSmpLinearWrap : register(s2);

cbuffer PerFrame : register(b0)
{
    float3 sunDirWS;
    float ambientIntensity;
    float3 lightRgb;
    float exposure;
    float3 camPosWS;
    float3 camDirWS;
    // P4: the sky fill's own colour. Defaults to the effective sun colour, which reproduces the
    // legacy `ambient * lightRgb` tinting exactly; a level that turns the tint off gets a real
    // sky colour here instead, so shaded sand reads blue at sunset rather than orange.
    float3 ambientRgb;
    // F8: 0 = flat fill (ambientRgb), the pre-F8 behaviour. 1 = sample the real sky irradiance in
    // the surface normal's direction, which is what makes a sky-facing surface catch the sky and a
    // downward-facing one catch the ground -- the thing one flat number can never express.
    uint skyIrradianceEnabled;
    // The level's sky intensity, so the irradiance cube honours the same control the background,
    // compose and the ocean do.
    float skyIrradianceScale;
    // P6B item 6/7. `gtaoEnabled` 0 leaves the target unread (it is not written when the pass is
    // off). `gtaoStrength` is UE's AmbientOcclusionStaticFraction: lerp(1, ao, strength).
    uint gtaoEnabled;
    float gtaoStrength;

    float4x4 invView;
    float4x4 invProj;

    float4x4 lightViewProj[4];
    float4 cascadeScaleBias[4];
    float4 cascadeSplitsVS;
    float2 shadowAtlasSize;
    float4 shadowBiasNDC;
    float4 normalBiasWS;
    float2 screenSize;
    float2 invScreenSize;
    // Configurable artist boost for the analytic sun specular on metals. The lobe is
    // scaled by (1 + metal*sunMetalSpec): metal=0 is a no-op (dielectrics stay physical),
    // metal=1 amplifies the sun highlight so it reads against the environment reflection
    // that would otherwise swamp it. 0 = pure physical.
    float sunMetalSpec;
    // Sun angular size, added to the GGX alpha for the analytic sun only (see EvalBRDF).
    // Floors the specular lobe width so a smooth surface shows a finite, bright,
    // sample-able sun glint instead of a sub-pixel spike. 0 = punctual (no change).
    float sunAngularSize;
    float2 _padSun;
    // Rung 2 / Step 24f: sample directional shadows from the VSM clipmap (VSM mode) instead of the
    // CSM cascades. clipmapViewProj[i] = clipmap level i's camera-centered ortho viewProj.
    uint useVsm;
    float vsmDepthBias;
    float clipmapBaseExtent;  // finest clipmap level's world extent (for per-level texel-scaled bias)
    float clipmapNormalBias;  // normal offset in texels
    float4x4 clipmapViewProj[8];
    // Underwater caustics (see caustics.hlsli). causticsTint.w == 0 disables the whole block:
    // that is the state when the level has no ocean, or the ocean has caustics switched off.
    float4 causticsTint;          // rgb = tint, w = master enable
    float4 causticsParams0;       // x: intensity, y: metres per tile, z: frames/sec, w: water level Y
    float4 causticsParams1;       // x: depth fade, y: surface fade, z: up-facing gate, w: bias
    float4 causticsParams2;       // x: dispersion, y: second-layer blend, z: time, w: world metres per pixel
    // S0.3: Legacy CSM debug visualization. 0 = off, 1 = tint by the RESOLVED cascade.
    // Always 0 in VSM mode (the CPU side forces it), so the branch below is dead there.
    uint csmDebugMode;
}

static const float pcfRadius = 1.0f;

// P6B item 6 -- the one place the two occlusion terms meet. Shared shape with compose_cs; if this
// rule ever changes, both change together or the diffuse and specular halves disagree about how
// occluded a pixel is.
float CombinedAo(float materialAo, float2 uv)
{
    if (gtaoEnabled == 0u)
    {
        return materialAo;
    }
    // `strength` scales the DYNAMIC term only. UE's AmbientOcclusionStaticFraction damps the whole
    // product, but here the material term already shipped in F9 and is not this step's to switch
    // off: at strength 0 this must be an EXACT no-op against the pre-P6B build, and the sweep
    // level's AO row is what proves it (damping the product moved it by 177/255).
    const float dynamicAo = saturate(GtaoTex.SampleLevel(gSmpPoint, uv, 0).r);
    return saturate(materialAo * lerp(1.0f, dynamicAo, saturate(gtaoStrength)));
}

int ChooseCascadeIndex(float3 Pws)
{
    float z = dot(Pws - camPosWS, camDirWS);
    float3 gt = saturate(sign(z.xxx - cascadeSplitsVS.yzw));
    return (int)(gt.x + gt.y + gt.z);
}

float ShadowPCF3x3(float2 uv, float zRef, float2 texel, float radiusPx)
{
    float s = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 off = float2(x, y) * texel * radiusPx;
            s += ShadowAtlas.SampleCmpLevelZero(gSmpLinear, uv + off, zRef).r;
        }
    }
    return s / 9.0;
}

// Blend-band width as a fraction of each split distance (Step 3).
static const float kBlendFraction = 0.1;

// Sample the shadow factor starting at cascade `start`, falling back to a coarser cascade
// if the (normal-bias-offset) point lands outside `start`'s atlas tile. Tests the
// cascade-local UV (BEFORE atlas scale+bias) so a neighbour tile is never sampled, and
// insets by the PCF reach so 3x3 taps never bleed across the gutterless tile border.
// Returns 1.0 (lit) only when the point is beyond cascade 3 — past the shadow range.
// outCascade reports which cascade the chain RESOLVED to (4 = fell past cascade 3, no shadow
// data). That is the cascade the pixel was actually shaded from, which is not always the one
// ChooseCascadeIndex picked — the difference is exactly the tile-border fallback (S0.3 tints it).
float SampleCascadeChain(int start, float3 Pws, float NdotL, float3 Nws, out int outCascade)
{
    const float2 texel = 1.0 / shadowAtlasSize;
    outCascade = 4;

    [unroll]
    for (int c = 0; c < 4; ++c)
    {
        if (c < start)
        {
            continue;
        }

        const float4x4 LVP = lightViewProj[c];
        const float4 sb = cascadeScaleBias[c];
        const float2 scale = sb.xy;
        const float2 biasUV = sb.zw;

        // Re-evaluate the offset per cascade: each has its own texel size.
        const float3 Poff = Pws + Nws * normalBiasWS[c];

        const float4 lc = mul(float4(Poff, 1), LVP);
        const float2 uvLocal = (lc.xy / max(1e-6, lc.w)) * float2(0.5, -0.5) + float2(0.5, 0.5);
        const float z = lc.z / max(1e-6, lc.w);

        const float2 margin = (pcfRadius * texel) / max(1e-6, scale);
        if (any(uvLocal < margin) || any(uvLocal > 1.0 - margin))
        {
            continue;
        }

        const float2 uv = uvLocal * scale + biasUV;
        const float bBase = shadowBiasNDC[c];
        const float b = bBase + (1.0 - saturate(NdotL)) * bBase;

        // Step 4: every cascade uses 3x3 PCF, but the texel radius is scaled per cascade
        // so the WORLD-space penumbra is anchored to cascade 0 instead of growing with the
        // cascade. A fixed 1-texel radius blurs far cascades ~10-16x more in world space
        // (their texels are that much larger) — that turned the last cascade into mush.
        // normalBiasWS[c] is proportional to cascade c's world texel size, so its ratio to
        // cascade 0 is the scale (the normalBiasInTexels factor cancels); c==0 -> 1.0.
        const float pcfR = pcfRadius * pow((normalBiasWS[0] / max(1e-6, normalBiasWS[c])), 0.25);
        outCascade = c;
        return ShadowPCF3x3(uv, z - b, texel, pcfR);
    }

    return 1.0;
}

float SampleShadowCSM(float3 Pws, float NdotL, float3 Nws, out int outCascade)
{
    const int idx = ChooseCascadeIndex(Pws);
    float shadow = SampleCascadeChain(idx, Pws, NdotL, Nws, outCascade);

    // Step 3: blend band. In a band just before cascade idx's far split, cross-fade into
    // cascade idx+1 so the hard cascade switch (and its bias / texel-density / PCF
    // discontinuity) becomes a gradient instead of a visible seam. Cascade 3 has no
    // coarser neighbour, so it never blends. Costs a second sample only inside the band.
    if (idx < 3)
    {
        const float zView = dot(Pws - camPosWS, camDirWS);
        const float splitNext = idx == 0 ? cascadeSplitsVS.y : (idx == 1 ? cascadeSplitsVS.z : cascadeSplitsVS.w);
        const float band = splitNext * kBlendFraction;
        const float t = saturate((zView - (splitNext - band)) / max(1e-4, band));
        if (t > 0.0)
        {
            // The blend partner's resolved index is not reported: outCascade stays the primary
            // cascade, so the debug tint shows zones rather than a striped blend band.
            int blendCascade;
            const float shadowNext = SampleCascadeChain(idx + 1, Pws, NdotL, Nws, blendCascade);
            shadow = lerp(shadow, shadowNext, t);
        }
    }

    return shadow;
}

// Directional sun visibility for a receiver at P with surface normal N and cosine ndl. Wraps
// the VSM-vs-CSM selection so the front and transmission samples share one code path. For the
// foliage transmission lobe the caller passes the flipped normal (-N) so the receiver is offset
// toward the sun-facing side (see CSMain).
// outCascade: the resolved CSM cascade (S0.3 debug tint). Left untouched in VSM mode — the
// clipmap has no cascades, and the CPU side forces csmDebugMode to 0 there anyway.
float SampleSunShadow(float3 P, float3 N, float ndl, out int outCascade)
{
    if (useVsm != 0u)
    {
        outCascade = 0;
        return VsmClipmapShadow(P, N, camPosWS, clipmapBaseExtent, clipmapNormalBias, vsmDepthBias,
                                clipmapViewProj, VsmPageTable, VsmPool, gSmpLinear);
    }
    return SampleShadowCSM(P, ndl, N, outCascade);
}

// Assemble the caustics inputs; returns tint.w == 0 when the feature is off for this frame.
CausticsParams LoadCausticsParams()
{
    CausticsParams p;
    p.tint = causticsTint.rgb;
    p.intensity = causticsParams0.x;
    p.scale = causticsParams0.y;
    p.speed = causticsParams0.z;
    p.waterLevel = causticsParams0.w;
    p.depthFade = causticsParams1.x;
    p.surfaceFade = causticsParams1.y;
    p.upFacing = causticsParams1.z;
    p.bias = causticsParams1.w;
    p.dispersion = causticsParams2.x;
    p.layerBlend = causticsParams2.y;
    p.time = causticsParams2.z;
    p.pixelWorldScale = causticsParams2.w;
    return p;
}

#define LIGHTING_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=12, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE))"

[RootSignature(LIGHTING_RS)]
[numthreads(8,8,1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = (uint)screenSize.x;
    uint height = (uint)screenSize.y;
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) * invScreenSize;

    float4 gb0 = GB0.SampleLevel(gSmpPoint, uv, 0);
    float4 gb1 = GB1.SampleLevel(gSmpPoint, uv, 0);

    float3 albedo = gb0.rgb;
    float2 rm = UnpackRM(gb0.a);
    float rough = rm.x;
    float metal = rm.y;

    float3 N = normalize(gb1.rgb * 2.0 - 1.0);
    float z = DepthT.SampleLevel(gSmpPoint, uv, 0).r;
    if (z <= kEpsilon)
    {
        LightTarget[dispatchThreadId.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float3 P = ReconstructPosWS(uv, z, invProj, invView);

    const float3 V = normalize(camPosWS - P);
    const float3 L = normalize(-sunDirWS);

    // GBAux.b carries the four-bit shading-model ID (F2). Foliage additionally reads its
    // premultiplied subsurface/transmission payload from GB2 (F3).
    const float4 gbAux = GBAux.SampleLevel(gSmpPoint, uv, 0);
    uint shadingModel = DecodeShadingModel(gbAux.b);
    const bool isFoliage = (shadingModel == kShadingModelTwoSidedFoliage);
    const float transmissionNormalWeight = saturate(gbAux.a);
    // F9: scalar material AO, written by the GBuffer since F3. Default 1 = no occlusion.
    const float materialAo = saturate(gbAux.r);
    float3 subsurface = 0.0f.xxx;
    if (isFoliage)
    {
        subsurface = GB2.SampleLevel(gSmpPoint, uv, 0).rgb;
    }

    // Q2: diffuse ambient applies to the dielectric (non-metal) fraction only. Metals have
    // no Lambertian response; their ambient arrives specularly via the env reflection in the
    // compose pass. Without the (1-metal) gate, metals get a flat albedo floor that washes
    // them out and kills highlight contrast.
    float3 ambient = albedo * (1.0 - metal) * ambientIntensity;
    BRDFInput bi;
    bi.albedo = albedo;
    bi.rough = rough;
    bi.metal = metal;
    bi.N = N;
    bi.V = V;
    bi.L = L;

    // Sky fill.
    //
    // Legacy: `ambient` is a FRACTION OF THE SUN COLOUR (`ambient * ambientRgb`), a number authored
    // to mean "this much of the sun's light bounces around". Once the source is the sky's measured
    // irradiance that fraction has no meaning -- reusing it multiplies an absolute radiance by
    // 0.05..0.1 and buries the fill about twenty times too deep, which is exactly what the first
    // version of this did. So the IBL branch uses the irradiance AS the fill and the level's
    // `ambient` stays what it always was: the strength of the flat fallback.
    // NOTE the local `ambient` above already carries albedo * (1 - metal) * ambientIntensity, so the
    // IBL branch has to re-apply albedo and the metal gate itself -- dropping them makes the fill
    // independent of the material, which reads as a uniform grey wash over the whole scene.
    float3 color;
    if (skyIrradianceEnabled != 0u)
    {
        const float3 irradiance = SkyIrradiance.SampleLevel(gSmpLinearWrap, N, 0).rgb;
        color = albedo * (1.0 - metal) * irradiance * skyIrradianceScale;
    }
    else
    {
        color = ambient * ambientRgb;
    }
    // F9 + P6B items 6-7: occlude INDIRECT DIFFUSE and nothing else. The sun, the local lights and
    // emissive are direct, and neither a cavity map nor a screen-space estimate has any business
    // dimming them.
    //
    // THE COMBINE RULE IS A PRODUCT, and it is UE's (DiffuseIndirectComposite.usf:371,
    // `lerp(1, MaterialAO * DynamicAO, AOMask * AmbientOcclusionStaticFraction)`). A product is the
    // right shape because the two terms describe INDEPENDENT occluders: the material's own cavities,
    // which no depth buffer can see, and the geometry around this pixel, which no baked map can see.
    // It is bounded in [0,1] by construction, monotonic in both inputs, and each input is an exact
    // identity at 1 -- so a scene with neither term is bit-identical to the build before this.
    // (`min` was the alternative: it never double-counts, but it also refuses to let a cavity deepen
    // a contact, which is exactly the case this pass exists to render.)
    color *= CombinedAo(materialAo, uv);

    // S0.3: seed the debug cascade with the one the split selection picks, so surfaces that never
    // sample a shadow (facing away from the sun) still show their zone. A real sample below
    // overwrites it with the cascade the fallback chain resolved to — that difference is what
    // makes the tile-border ring visible. Costs nothing when the mode is off.
    int csmCascade = (csmDebugMode != 0u) ? ChooseCascadeIndex(P) : 0;

    // Caustics scale the sun's irradiance, so they multiply the direct term rather than being
    // added on top: a surface the sun cannot reach gets none, and the bright filaments ride the
    // same shadow and cosine as the light they come from. Clamped so a negative bias can dim the
    // cells between filaments without ever going below black.
    float3 causticsGain = 1.0.xxx;
    if (causticsTint.w > 0.0)
    {
        causticsGain = max(0.0.xxx, 1.0.xxx + EvaluateCaustics(
            CausticsAtlas, gSmpLinearWrap, LoadCausticsParams(),
            P, N, dot(P - camPosWS, camDirWS)));
    }

    if (isFoliage)
    {
        FoliageResult fr = EvalFoliageBRDF(
            bi, subsurface, sunAngularSize, transmissionNormalWeight);

        // Front hemisphere: the leaf face directly toward the sun (Lambert + GGX), same math
        // and same shadow as DefaultLit.
        if (fr.NdotL > 0.0)
        {
            float shadow = SampleSunShadow(P, N, fr.NdotL, csmCascade);
            const float3 specSun = fr.specBRDF * (1.0 + metal * sunMetalSpec * 1);
            color += (fr.diffBRDF + specSun) * fr.NdotL * lightRgb * shadow * causticsGain;
        }

        // View-opposed lobe: light transmitted through the thin leaf toward the viewer. Sample
        // the shadow with the normal flipped (-N) so the receiver is offset toward the
        // light-facing side and its slope bias uses the opposite cosine — otherwise the leaf's own
        // front face self-shadows the transmission to black. This is the scoped transmission
        // shadow bias the plan (F4) calls for, not shadow disablement: a genuine occluder
        // between the sun and the leaf still darkens the sun-facing side and kills transmission.
        if (any(fr.transBRDF > 0.0))
        {
            // Separate index, discarded: the transmission lobe samples with a flipped normal and
            // must not decide which cascade the debug tint reports for this pixel.
            int transCascade;
            float shadowT = SampleSunShadow(P, -N, saturate(dot(-N, L)), transCascade);
            color += fr.transBRDF * lightRgb * shadowT;
        }
    }
    else
    {
        BRDFResult br = EvalBRDF(bi, sunAngularSize);
        if (br.NdotL > 0.0)
        {
            // Step 24f: VSM mode samples the directional clipmap; Legacy samples the CSM cascades.
            float shadow = SampleSunShadow(P, N, br.NdotL, csmCascade);
            // Boost the analytic sun specular on metals (1 + metal*sunMetalSpec) so the
            // highlight reads against the environment reflection. metal=0 -> no change.
            const float3 specSun = br.specBRDF * (1.0 + metal * sunMetalSpec * 1);
            float3 direct = (br.diffBRDF + specSun) * br.NdotL * lightRgb * shadow * causticsGain;
            color += direct;
        }
    }

    // S0.3: cascade tint. Applied to the whole lit result (not just the direct term) so the zones
    // read in shadow and ambient too. Index 4 = the sample fell past cascade 3, i.e. no shadow
    // data at all — grey, which is what makes the hard 300 m terminator visible.
    if (csmDebugMode == 1u)
    {
        const float3 kCascadeTint[5] = {
            float3(1.00, 0.35, 0.35),   // c0
            float3(0.35, 1.00, 0.35),   // c1
            float3(0.35, 0.55, 1.00),   // c2
            float3(1.00, 1.00, 0.35),   // c3
            float3(0.45, 0.45, 0.45) }; // beyond the last cascade
        color *= kCascadeTint[clamp(csmCascade, 0, 4)];
    }

    LightTarget[dispatchThreadId.xy] = float4(color * exposure, 1.0);
}
