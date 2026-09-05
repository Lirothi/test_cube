// The lighting pass's per-frame constant buffer (b0) and the CSM parameter mapping, shared by
// lighting_cs.hlsl and the volumetric fog's scatter pass (fog_scatter_cs.hlsl): both sample the
// sun's shadow through the same CSM/VSM helpers and MUST read the same constants -- one
// definition, two shaders, no drift (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part A).
// The CPU fills it once per frame (SceneRenderer::FillLightingConstants) and each consumer binds
// its own copy of that allocation; the field offsets are the lighting material's.
// Include AFTER csm_sample.hlsli / vsm_sample.hlsli (CsmParams, VSM_NUM_CLIPMAP_LEVELS).
#ifndef LIGHTING_CB_HLSLI
#define LIGHTING_CB_HLSLI

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
    // P16.12 -- GROUND BOUNCE. The ground's diffuse REFLECTANCE, 0 = the term is off. It is an
    // albedo, not a light: it only ever scales illuminance the scene already has, so it cannot
    // brighten a night scene and it moves with the sun automatically. See GroundBounceOverPi.
    float3 groundAlbedoRgb;
    float _padGround;

    float4x4 invView;
    float4x4 invProj;

    float4x4 lightViewProj[4];
    float4 cascadeScaleBias[4];
    float4 cascadeSplitsVS;
    float2 shadowAtlasSize;
    float4 shadowBiasNDC;
    float4 cascadeTexelWS;
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
    float clipmapBaseExtent;  // finest clipmap level's world extent (level i = base * 2^i)
    // P16.16: UE units (their `r.Shadow.Virtual.NormalBias`, default 0.5). See VsmClipmapShadow.
    float clipmapNormalBias;
    // Per-level shaping of vsmDepthBias (see VsmClipmapShadow): bias(L) = max(bias * decay^L, floor).
    // decay 1 + floor 0 = the legacy constant-in-texels bias. Floor arrives already converted to NDC.
    float clipmapDepthBiasDecay;
    float clipmapDepthBiasFloorNdc;
    float clipmapBlendWidth; // outer fraction of a fine level blended into its parent; 0 = off
    // SMRT (docs/vsm_smrt_plan.md). smrtRayCount 0 = the single-tap SampleCmp path, unchanged.
    // MIRRORS LightingPassConstants; the four sit together in both so the 16-byte rows line up.
    uint smrtRayCount;
    uint smrtSamplesPerRay;
    float smrtRayLengthScale;
    float smrtExtrapolateMaxSlope;
    float smrtSourceRadius;      // Step 3: sin of the light's angular radius
    float smrtTexelDitherScale;
    float smrtLevelMargin;
    uint smrtFrameIndex;   // 0 = no temporal rotation of the sample set
    uint smrtAdaptiveRayCount;
    float smrtScreenRayLength;   // multiple of view depth; 0 = no screen trace
    uint smrtScreenRaySamples;
    float _padClipBias;
    float4x4 viewProj;           // camera world -> clip, for the screen-space rays
    float4x4 projMatrix;         // camera view -> clip; the contact ray's compare tolerance needs it
    // Contact shadows (S12). Length is a MULTIPLE OF VIEW DEPTH, as UE's is, so the trace covers a
    // constant number of screen pixels at any distance. 0 = off.
    float contactShadowLength;
    float contactShadowIntensity;
    uint  contactShadowSteps;
    uint  contactShadowLengthInWS;    // 1 = METRES; 0 = multiple of view depth (UE's screen scale)
    float contactShadowNormalOffset;  // ours: FRACTION of the ray length -- see ShadowSettings.h
    float contactShadowGrazingFade;
    float contactShadowMinDist;
    float contactShadowMaxDist;
    float contactShadowFadeBand;
    float contactShadowThickness;     // ours: FRACTION of the ray length; 0 = UE behaviour
    uint  contactShadowFrameId;       // UE StateFrameIndexMod8 + 1; 0 = static dither
    float _padContact;
    float4x4 clipmapViewProj[VSM_NUM_CLIPMAP_LEVELS]; // mirrored in LightingPassConstants
    // P16.16: inverse transpose of world -> shadow UVZ, for the receiver-plane depth bias. One
    // matrix covers every level (the extent cancels out of the gradient); UE build theirs the same
    // way in CalcTranslatedWorldToShadowUVNormalMatrix.
    float4x4 clipmapUvNormal;
    // Underwater caustics (see caustics.hlsli). causticsTint.w == 0 disables the whole block:
    // that is the state when the level has no ocean, or the ocean has caustics switched off.
    float4 causticsTint;          // rgb = tint, w = master enable
    float4 causticsParams0;       // x: intensity, y: metres per tile, z: frames/sec, w: water level Y
    float4 causticsParams1;       // x: depth fade, y: surface fade, z: up-facing gate, w: bias
    float4 causticsParams2;       // x: dispersion, y: second-layer blend, z: time, w: world metres per pixel
    // S0.3: Legacy CSM debug visualization. 0 = off, 1 = tint by the RESOLVED cascade.
    // Always 0 in VSM mode (the CPU side forces it), so the branch below is dead there.
    uint csmDebugMode;
    // S8: 0 = the legacy 3x3 SampleCmp box, 1 = soft-occlusion ramp + 4x4 Gather tent.
    uint csmFilterMode;
    // S8 knobs: x = receiver bias, y = sharpen (already in UE shader units), z = over-blur correct.
    float4 csmFilterParams;
    // S10: x = last cascade's far split, y = blend fraction, z = distance-fade fraction.
    float4 csmFadeParams;
    // The sky specular block, mirroring compose's own fields so both passes agree by construction.
    // `skySpecMipCount` 0 also selects the raw-cube fallback path (see IblSkyRadiance).
    uint enableSkySpecular;
    uint skySpecMipCount;
    float skyboxIntensity;
    float _padSkySpec;
}



// S3: cascade sampling itself lives in csm_sample.hlsli, shared with glass.hlsl. This wrapper is
// the only thing that stays here — it maps THIS shader's cbuffer field names onto CsmParams.
CsmParams MakeCsmParams()
{
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
    p.atlasSize    = shadowAtlasSize;
    p.camPosWS     = camPosWS;
    p.camDirWS     = camDirWS;
    p.pcfRadius    = 1.0f;
    // S8: the ramp width is 1 / (transition zone in NDC), and the zone we want is exactly the depth
    // bias this cascade already carries -- shadowBiasNDC[c] = depthBiasInTexels * unitsPerTexel[c] /
    // zRange[c], i.e. ALREADY proportional to the cascade's world texel, which is the proportionality
    // UE gets from TransitionSize. So no new constant buffer field is needed for it.
    [unroll]
    for (int t = 0; t < 4; ++t)
    {
        p.transitionScale[t] = 1.0f / max(1e-6f, shadowBiasNDC[t]);
    }
    p.receiverBiasMin  = csmFilterParams.x;
    p.sharpen          = csmFilterParams.y;
    p.overBlurCorrect  = csmFilterParams.z;
    p.normalBiasTexels = csmFilterParams.w;
    p.farSplit             = csmFadeParams.x;
    p.blendFraction        = csmFadeParams.y;
    p.distanceFadeFraction = csmFadeParams.z;
    p.useGatherPcf     = csmFilterMode;
    return p;
}


#endif // LIGHTING_CB_HLSLI
