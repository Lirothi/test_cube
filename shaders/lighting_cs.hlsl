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
#include "ibl_common.hlsli"
// SMRT's adaptive ray count uses wave intrinsics, which UE gate with #if COMPUTESHADER because
// the "all lanes agree" test is only meaningful outside a pixel shader's helper lanes.
#define VSM_SMRT_COMPUTE 1
#include "contact_shadow.hlsli"
#include "vsm_screen_ray.hlsli"
#include "vsm_sample.hlsli"
#include "csm_sample.hlsli"
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
// The sky's INDIRECT SPECULAR moved here from compose. It has to be added before the screen-space
// reflection pass runs, because that pass samples this very target: with the term still in compose,
// a metal seen inside a reflection was a black disc with a highlight -- a metal has no diffuse, so
// until this is added it has nothing else in the light buffer at all. See ibl_common.hlsli for the
// split and why the total is unchanged.
// t12: GGX-prefiltered sky radiance   t13: split-sum environment BRDF   t14: the raw sky cube,
// used only by levels whose sky arrived without prefiltered derivatives.
TextureCube SkySpecular : register(t12);
Texture2D BrdfLut : register(t13);
TextureCube SkyboxTex : register(t14);
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpPoint : register(s0);
SamplerComparisonState gSmpLinear : register(s1);
SamplerState gSmpLinearWrap : register(s2);
// s3: LinearCLAMP. The BRDF LUT is a 2D table indexed by (NdotV, roughness) and MUST be clamped --
// reading it through the caustics sampler (LinearWrap, the only linear one this pass had) wraps
// both edges and quietly returns the wrong Fresnel there. compose has always read it clamped, and
// the two passes now have to agree exactly or the sky term one adds and the other subtracts stop
// cancelling.
SamplerState gSmpLinearClamp : register(s3);

#include "lighting_cb.hlsli" // the b0 cbuffer + MakeCsmParams, shared with fog_scatter_cs.hlsl

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
    //
    // P16.4: the GTAO target carries TWO scales -- .x a contact radius, .y a medium one sized to
    // canopies and buildings -- and this is where they meet. THE COMBINE IS A MIN, NOT A PRODUCT,
    // and the difference matters: the two estimates are the SAME physical quantity (the fraction of
    // the hemisphere this pixel can see) measured at two scales, not two independent occluders. The
    // wide walk sees the trunk at your feet as well as the crown overhead, so multiplying would
    // square that trunk. `min` takes whichever walk found more occlusion, which is exactly the
    // right answer when each one is blind at the other's scale. It is still bounded in [0,1],
    // monotonic, and an identity at 1, so the reasoning about the product below still holds.
    //
    // (The product with `materialAo` stays a product: cavities in a texture ARE independent of the
    // geometry a depth buffer can see.)
    const float2 dynamicAo = saturate(GtaoTex.SampleLevel(gSmpPoint, uv, 0).rg);
    return saturate(materialAo * lerp(1.0f, min(dynamicAo.x, dynamicAo.y), saturate(gtaoStrength)));
}

// P16.12 -- THE LIGHT THAT COMES BACK UP OFF THE GROUND.
//
// There is no bounce term in this engine: the only fill is the sky cube, and the cube's lower
// hemisphere carries whatever the HDRI's own ground was, not the sunlit sand this scene is
// standing on. So a frond in shade over a beach at 54,400 lx received about 8,000 lx -- sky only.
// Measured on `wind_test`: the missing bounce is worth about 1.2 stops on exactly the surfaces
// that read as too dark for a sunny day.
//
// The approximation is the classic infinite-ground-plane split, the same one UE spend on
// SkyLight's LowerHemisphereColor and Godot on its ambient:
//
//   E_bounce(N) = groundAlbedo * E_ground * F(N),   F(N) = (1 - cos theta) / 2
//
// with theta the angle between N and world up, so `cos theta` is just `N.y`. The view factor is
// exact for a flat infinite plane of uniform radiance: an up-facing surface sees NONE of the
// ground, a vertical one sees half, a down-facing one sees all of it. That is the whole reason to
// prefer it to a flat ambient add -- the direction dependence is what makes it read as light off
// the floor instead of a grey wash.
//
// `E_ground` is the illuminance ON a level surface: the sun's own cosine plus the sky's up-facing
// irradiance. It is deliberately a SCENE average and not shadowed -- a point standing in a palm's
// shadow is still surrounded by lit sand. What does localise it is the AO the caller applies to
// the whole indirect-diffuse term afterwards, which is what stops this lighting the inside of a
// closed box.
//
// Everything here is divided by PI because the irradiance cube stores E/PI and the caller
// multiplies the sum by albedo alone.
//
// DIFFUSE ONLY. A down-facing glossy surface really does reflect bright sand, but the prefiltered
// sky cube has no such term either, and adding one on this side only would make the two disagree.
float3 GroundBounceOverPi(float3 N)
{
    if (dot(groundAlbedoRgb, groundAlbedoRgb) <= 0.0f)
    {
        return 0.0f.xxx;
    }
    // `sunDirWS` is the direction the light TRAVELS, so its negated y is the cosine on a level
    // surface, and a sun below the horizon contributes nothing without a special case.
    const float3 sunOnGroundOverPi = lightRgb * saturate(-sunDirWS.y) * kInvPi;
    const float3 skyOnGroundOverPi =
        SkyIrradiance.SampleLevel(gSmpLinearWrap, float3(0.0f, 1.0f, 0.0f), 0).rgb *
        skyIrradianceScale;
    const float groundViewFactor = (1.0f - N.y) * 0.5f;
    return groundAlbedoRgb * (sunOnGroundOverPi + skyOnGroundOverPi) * groundViewFactor;
}

// Directional sun visibility for a receiver at P with surface normal N and cosine ndl. Wraps
// the VSM-vs-CSM selection so the front and transmission samples share one code path. For the
// foliage transmission lobe the caller passes the flipped normal (-N) so the receiver is offset
// toward the sun-facing side (see CSMain).
// outCascade: the resolved CSM cascade (S0.3 debug tint). Left untouched in VSM mode — the
// clipmap has no cascades, and the CPU side forces csmDebugMode to 0 there anyway.
// CONTACT SHADOWS. Applied to whatever the shadow map returned, in BOTH modes, because it is a
// property of the camera depth buffer and not of any shadow map: a cascade covering hundreds of
// metres cannot resolve the scale this recovers, and neither can a coarse clipmap level.
// UE combine it multiplicatively (`OutShadow.SurfaceShadow *= ContactShadow`), not with a min.
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

float SampleSunShadow(float3 P, float3 N, float ndl, out int outCascade, uint2 pixel)
{
    if (useVsm != 0u)
    {
        outCascade = 0;
        // invProj._11 is tan(hFov/2) for this projection, which is the term UE's normal offset
        // needs; no extra constant for something the matrix already carries.
        VsmSmrtParams smrt;
        smrt.rayCount = smrtRayCount;
        smrt.samplesPerRay = smrtSamplesPerRay;
        smrt.rayLengthScale = smrtRayLengthScale;
        smrt.extrapolateMaxSlope = smrtExtrapolateMaxSlope;
        smrt.sourceRadius = smrtSourceRadius;
        smrt.texelDitherScale = smrtTexelDitherScale;
        smrt.levelMargin = smrtLevelMargin;
        smrt.frameIndex = smrtFrameIndex;
        smrt.adaptiveRayCount = smrtAdaptiveRayCount;
        // `sunDirWS` is the direction the light TRAVELS; SMRT marches TOWARDS the light.
        const float3 dirToLight = normalize(-sunDirWS);

        // SCREEN-SPACE RAY, ahead of the shadow-map march. UE's ScreenRayLength is a MULTIPLE OF
        // VIEW DEPTH (their default 0.015), so the trace is short near the camera and grows with
        // distance -- which is what keeps it the same size in SCREEN space, where it is sampled.
        // Skipped entirely when SMRT is off: there is no ray for it to offset.
        float rayStartOffset = 0.0f;
        if (smrt.rayCount > 0u && smrtScreenRayLength > 0.0f)
        {
            const float viewDepth = length(P - camPosWS);
            // Reuses the SMRT dither so the screen trace and the march are decorrelated the same
            // way, rather than inventing a second noise source with its own banding.
            const float dither = frac(VsmSmrtHash2(P).x + VsmSmrtR2(smrt.frameIndex).x);
            rayStartOffset = VsmScreenRayCast(P, dirToLight,
                                              smrtScreenRayLength * viewDepth,
                                              dither, smrtScreenRaySamples,
                                              viewProj, DepthT, gSmpPoint);
        }

        const float vsmShadow = VsmClipmapShadow(P, N, camPosWS, clipmapNormalBias, vsmDepthBias,
                                clipmapDepthBiasDecay, clipmapDepthBiasFloorNdc, clipmapBlendWidth,
                                invProj._11, clipmapUvNormal, dirToLight, smrt, rayStartOffset,
                                clipmapViewProj, VsmPageTable, VsmPool, gSmpLinear);
        return ApplyContactShadow(vsmShadow, P, N, dirToLight, ndl, pixel, camPosWS,
                                  viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                                  MakeContactParams());
    }
    const float csmShadow =
        CsmSampleShadow(MakeCsmParams(), ShadowAtlas, gSmpLinear, gSmpPoint, P, N, ndl, outCascade);
    return ApplyContactShadow(csmShadow, P, N, normalize(-sunDirWS), ndl, pixel, camPosWS,
                              viewProj, projMatrix, invProj, invView, DepthT, gSmpPoint,
                              MakeContactParams());
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
    "DescriptorTable(SRV(t0, numDescriptors=15, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE))"

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
        // P16.12: the sky above and the ground below are the two halves of the same fill, so they
        // are summed BEFORE the albedo multiply rather than added as a second lit result.
        // Deliberately not extended to the flat-fill branch below: `ambient` there is a fraction
        // of the SUN COLOUR authored against a different equation, and a physical bounce added to
        // it would be two units in one sum.
        color = albedo * (1.0 - metal) * (irradiance * skyIrradianceScale + GroundBounceOverPi(N));
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
    int csmCascade = (csmDebugMode != 0u) ? CsmChooseCascade(MakeCsmParams(), P) : 0;

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
            float shadow = SampleSunShadow(P, N, fr.NdotL, csmCascade, dispatchThreadId.xy);
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
            float shadowT = SampleSunShadow(P, -N, saturate(dot(-N, L)), transCascade, dispatchThreadId.xy);
            color += fr.transBRDF * lightRgb * shadowT;
        }
    }
    else
    {
        BRDFResult br = EvalBRDF(bi, sunAngularSize);
        if (br.NdotL > 0.0)
        {
            // Step 24f: VSM mode samples the directional clipmap; Legacy samples the CSM cascades.
            float shadow = SampleSunShadow(P, N, br.NdotL, csmCascade, dispatchThreadId.xy);
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

    // INDIRECT SKY SPECULAR, added AFTER the exposure multiply on purpose: compose used to add it
    // to an already-exposed light value and never scaled it, so folding it in before the multiply
    // would change every existing frame. Same inputs and same helpers as compose, so the two sum
    // to exactly what compose alone used to produce.
    float3 skySpecular = 0.0f.xxx;
    if (enableSkySpecular != 0u)
    {
        const float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
        const float3 R = NormalizeSafe(reflect(-V, N), N);
        float3 skyCol = IblSkyRadiance(SkySpecular, SkyboxTex, gSmpLinearClamp, R, rough,
                                       skySpecMipCount, skyboxIntensity);
        const float cosT = saturate(dot(N, V));
        // F9: the FALLBACK SKY is the only indirect specular that gets occluded -- a traced hit
        // already saw the geometry AO stands in for. compose applies the identical line to the
        // term it subtracts, so the two stay each other's exact inverse.
        skyCol *= IblSpecularOcclusion(cosT, CombinedAo(materialAo, uv), rough);
        const float indirectSpecularScale = saturate(gbAux.g);
        skySpecular = skyCol * indirectSpecularScale *
                      IblSpecularWeight(BrdfLut, gSmpLinearClamp, F0, cosT, rough, skySpecMipCount);
    }

    LightTarget[dispatchThreadId.xy] = float4(color * exposure + skySpecular, 1.0);
}
