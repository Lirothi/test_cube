// The transparent pass's shared per-view constant buffer (b1): glass AND particles read it (the
// C++ mirror is GlassViewCB in SceneRenderInternal.h). Extracted from glass.hlsl verbatim for
// the volumetric fog (plan A5) -- particles used to declare a prefix of it under another name.
#ifndef GLASS_VIEW_CB_HLSLI
#define GLASS_VIEW_CB_HLSLI

// Needs VSM_NUM_CLIPMAP_LEVELS (a static const in vsm_addressing.hlsli): include it first.

// Per-view/per-frame data shared by every glass object in the pass. Filled once.
cbuffer GlassView : register(b1)
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;
    float4x4 viewProjNoJitter;
    float4x4 prevViewProjNoJitter;
    float4x4 invView;
    float4x4 invProj;
    float4 camPosSky;             // xyz = camera position, w = sky intensity
    float4 sunDirAmbient;         // xyz = sun direction, w = ambient intensity
    float4 sunColorExposure;      // xyz = sun color, w = sun exposure
    float4 camDirWS;              // xyz = camera forward, w = P5 prefiltered sky mip count (0 = none)
    float4 screenSizeInv;         // xy = screen size, zw = inverse screen size
    float4 shadowAtlasSizeInv;    // xy = atlas size, zw = inverse atlas size
    float4 shadowBiasNDC;         // cascade depth bias
    float4 cascadeTexelWS;        // world size of one cascade texel
    float4 cascadeSplitsVS;       // cascade splits in view space
    float4 cascadeScaleBias[4];   // xy = scale, zw = bias per cascade
    float4 spotShadowInfo;        // xy = spot shadow size, zw = inverse size
    float4 lightCounts;           // x = point lights, y = spot lights, z = traced reflections, w = sky reflection enabled
    float4x4 lightViewProj[4];
    float4 vsmParams;             // x = useVsm, y = refDist, z = depth-bias floor, w = clip blend width
    float4 clipmapParams;         // Step 24f: x = baseExtent, y = normalBias (UE units), z = depthBias (NDC), w = depth-bias decay/level
    float4x4 clipmapViewProj[VSM_NUM_CLIPMAP_LEVELS]; // Step 24f: directional clipmap level viewProjs
    float4x4 clipmapUvNormal;     // P16.16: receiver-plane transform, must match lighting_cs
    // P16.1: x = the pre-exposure every writer of scene colour applies. Glass writes in the
    // transparent pass, which runs AFTER compose, so compose's own scaling never reaches it.
    float4 preExposureParams;
    // S8: x = 0 legacy 3x3 SampleCmp box, 1 = soft-occlusion ramp + 4x4 Gather tent. APPENDED at the
    // tail on purpose -- inserting anywhere else shifts every offset after it.
    float4 csmFilterMode;         // x = kernel mode, y = receiver bias, z = sharpen, w = over-blur
    float4 csmFilterParams;       // S10 x = far split, y = blend, z = distance fade; w = normal bias
    // SMRT (docs/vsm_smrt_plan.md). x = ray count (0 = single-tap path), y = samples per ray,
    // z = ray length scale, w = extrapolate max slope. APPENDED at the tail, same rule as above.
    // Glass samples the SAME clipmap lighting_cs does, so it has to get the same numbers or a
    // window shades against a differently-sampled shadow than the ground under it.
    float4 smrtParams;
    float4 smrtParams2;           // x = source radius (sin), y = texel dither, z = margin, w = frame
    // Volumetric fog plan A5 (APPENDED at the tail, same rule as above): the volume's lookup
    // parameters and the analytic medium (PackAtmosphere), so glass and particles sit in the
    // air the sand is in.
    float4 fogVolumeParams;       // (on, far view depth, 1/preExposure, slice count)
    float4 fogVolumeZParams;      // (B, O, S, 0)
    float4 fogParams0;            // density, height falloff, reference height, start distance
    float4 fogParams1;            // max opacity, sun scatter strength, sun scatter exponent, sun scatter start
    float4 fogParams2;            // sky blur, sky back-scatter, zw reserved
};

#endif // GLASS_VIEW_CB_HLSLI
