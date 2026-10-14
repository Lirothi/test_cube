// Light shafts (plan A7, docs/volumetric_fog_sky_clouds_ssgi_plan.md): UE's LightShaftBloom,
// transcribed from Shaders/Private/LightShaftShader.usf (148 lines) and
// Renderer/Private/LightShaftRendering.cpp -- four compute kernels over ONE cbuffer:
//   CSDownsample  usf:39-85 (#else = the bloom branch): half-res masked bloom source from scene colour + depth
//   CSBlur        usf:93-121: one radial blur pass towards the sun's screen position, NUM_SAMPLES 12
//   CSApply       usf:141-149, blend BF_One/BF_One (LightShaftRendering.cpp:530): add the result into scene colour
//   CSTemporal    UE's TAA of the downsample (LightShaftRendering.cpp, the LightShafts history): this
//                 frame's mask blended into last frame's, reprojected by the G-buffer velocity
// The renderer runs downsample -> temporal -> 3 blurs (r.LightShaftBlurPasses) -> apply, half
// resolution (r.LightShaftDownSampleFactor 2), after the transparents. The temporal step was first
// left to DLSS, which cannot do it: the mask is drawn from the JITTERED scene at half resolution and
// three radial blurs stretch every sub-pixel change of a leaf edge into a whole ray -- the shafts
// trembled with the foliage frozen (frame-to-frame difference around the sun 2.8 against 0.7 with
// the shafts off) and more with it swaying ("солнечные лучи оч сильно дрожат сквозь листву").
//
// Units: scene colour is stored PRE-EXPOSED, exactly as UE's is ("post exposure brightness",
// usf:70), so BloomThreshold / BloomMaxBrightness mean what they mean in UE. Depths are metres on
// both sides (UE cm against a cm range): the range is the same unit as the depth it masks.
//
// t0 SrcColor    -- downsample: scene colour (render res); temporal: this frame's mask;
//                   blur/apply: the previous half-res result
// t1 DepthTex    -- downsample only
// t2 HistoryTex  -- temporal only: last frame's temporal output (the previous frame slot's)
// t3 VelocityTex -- temporal only: the G-buffer motion, currUv - prevUv, render res
//                   (unused slots are dummies, never sampled: the table is positional)
// u0 Out         -- downsample/temporal/blur: the half-res target; apply: scene colour (read-modify-write)
// s0 linear clamp (UE GlobalBilinearClampedSampler)  s1 point clamp (depth, velocity)
#define LIGHT_SHAFTS_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

Texture2D           SrcColor    : register(t0);
Texture2D           DepthTex    : register(t1);
Texture2D           HistoryTex  : register(t2);
Texture2D           VelocityTex : register(t3);
RWTexture2D<float4> Out         : register(u0);
SamplerState gSmpLinear : register(s0);
SamplerState gSmpPoint  : register(s1);

cbuffer LightShaftCB : register(b0)
{
    // xy = TextureSpaceBlurOrigin (uv of the sun, aspect-corrected: v * H/W), z = W/H
    // (AspectRatioAndInvAspectRatio.y), w = H/W (.w). UE: LightShaftRendering.cpp:160-176.
    float4 lsOrigin;
    // x = 1 / OcclusionDepthRange, y = BloomScale, z = BloomMaxBrightness, w = BloomThreshold
    // (LightShaftParameters.xy, BloomMaxBrightness, BloomTintAndThreshold.a).
    float4 lsBloom;
    // rgb = BloomTint, w = r.LightShaftFirstPassDistance (0.1).
    float4 lsTint;
    // x = depthA, y = depthB: view depth = depthB / (deviceZ - depthA), the engine's pair.
    // z = the light's Shaft Length under 1: the fraction of the way to the light every blur pass's vector
    // is scaled by (not UE's; 1 = theirs). w = its part over 1, 0..1: the apply's fall-off lift (0 = UE's).
    float4 lsDepth;
    // xy = this dispatch's target size, z = the blur pass index (RadialBlurParameters.z), w = 0.
    uint4  lsSize;
    // xy = 1 / lsSize.xy, zw = half a texel of the SOURCE: UE's UVMinMax bilinear inset.
    float4 lsInvSize;
    // Temporal only: x = this frame's weight (1 - exp(-dt / smoothing); 1 = no history), y = the
    // history's pre-exposure rescale (now / then), z = 1 when last frame's history is valid, w = 0.
    float4 lsTemporal;
    // Downsample only, the SUN GLARE in the seed (ours): rgb = its colour at the sun's centre in the scene's
    // units (0 = none: the moon's shafts, the glare off), w = the core's e-folding angle (rad).
    float4 lsGlareColor;
    // xyz = the direction to the sun in view space, w = the veil's weight.
    float4 lsGlareDir;
    // xy = the projection's x/y scales (ndc = tangent x scale), z = the veil's half-strength angle (rad).
    float4 lsGlareProj;
};

static const int kNumSamples = 12; // r.LightShaftNumSamples, baked into the shader like UE's define
// Shaft Length over 1: the distance from the light (aspect-corrected uv, ~4 degrees at a 80-degree view)
// inside which the shafts stay as UE draw them -- the disc and its glow -- and past which the lift grows.
static const float kShaftLengthRef = 0.05f;

// usf:39-85, the #else branch (bloom). One thread per HALF-RES texel.
[numthreads(8, 8, 1)]
[RootSignature(LIGHT_SHAFTS_RS)]
void CSDownsample(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= lsSize.x || tid.y >= lsSize.y)
    {
        return;
    }
    const float2 uv = (float2(tid.xy) + 0.5f) * lsInvSize.xy;
    // The half-res texel centre sits on the corner between four render-res texels, so one bilinear
    // tap IS the 2x2 box; UE's CalcSceneColor is one tap too (usf:62).
    float3 sceneColor = SrcColor.SampleLevel(gSmpLinear, uv, 0).rgb;
    // THE SUN GLARE IN THE SEED (ours; owner, 2026-10-14: "blur after the glare is laid"). UE seed on the
    // scene over the threshold; Ultra Dynamic Sky's sky is bright enough round the sun that a wide disc of
    // it passes, and its rays run long through the crowns -- ours is darker, day and night, and only the
    // disc passed. The glare (bloom_conv_cs.hlsl stage 9, the same core + veil about the sun's direction)
    // is that bright sky as the picture shows it; laid on here, it seeds wherever the sky shows through
    // the leaves -- the depth mask below still keeps it off the leaves themselves.
    if (any(lsGlareColor.rgb > 0.0f))
    {
        const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        const float3 dir = normalize(float3(ndc / lsGlareProj.xy, 1.0f));
        const float angle = acos(clamp(dot(dir, lsGlareDir.xyz), -1.0f, 1.0f));
        const float v = angle / max(lsGlareProj.z, 1.0e-4f);
        sceneColor += lsGlareColor.rgb * (exp(-angle / max(lsGlareColor.w, 1.0e-4f)) + lsGlareDir.w / (1.0f + v * v));
    }
    const float deviceZ = DepthTex.SampleLevel(gSmpPoint, uv, 0).r;
    const float sceneDepth = lsDepth.y / max(deviceZ - lsDepth.x, 1e-8f); // usf:63 CalcSceneDepth

    // usf:65-68: a mask that is 1 at the screen edges and 0 in the centre, fourth power.
    // UVMinMax is the whole target here (no view rect inside a larger extent).
    float edgeMask = 1.0f - uv.x * (1.0f - uv.x) * uv.y * (1.0f - uv.y) * 8.0f;
    edgeMask = edgeMask * edgeMask * edgeMask * edgeMask;

    // usf:70-74: bloom only what is over the threshold, capped at BloomMaxBrightness.
    const float luminance = max(dot(sceneColor, float3(0.3f, 0.59f, 0.11f)), 6.10352e-5f);
    const float adjustedLuminance = clamp(luminance - lsBloom.w, 0.0f, lsBloom.z);
    const float3 bloomColor = lsBloom.y * sceneColor / luminance * adjustedLuminance * 2.0f;

    // usf:78-79: only the FAR half of OcclusionDepthRange seeds the shafts (sky, distant ground).
    const float bloomDistanceMask = saturate((sceneDepth - 0.5f / lsBloom.x) * lsBloom.x);
    // usf:80-81: 0 at the sun, 1 half an aspect-corrected screen away.
    const float blurOriginDistanceMask =
        1.0f - saturate(length(lsOrigin.xy - uv * float2(1.0f, lsOrigin.w)) * 2.0f);
    // usf:83.
    Out[tid.xy] = float4(bloomColor * lsTint.rgb * bloomDistanceMask * (1.0f - edgeMask) *
                         blurOriginDistanceMask * blurOriginDistanceMask, 1.0f);
}

// The temporal step between the downsample and the blurs, half res. Last frame's mask is
// reprojected by the G-buffer velocity (every pixel has one, the sky included: currUv - prevUv) and
// this frame's is blended in at lsTemporal.x, a frame-rate-independent exponential with the sun's
// Light Shaft Smoothing as its time constant. No neighbourhood clamp, unlike a TAA: the point is the
// inertia -- a leaf swaying across the sun eases the rays over instead of switching them -- and the
// radial blur after it smears any lag into the ray it belongs to. Off the previous frame, or with no
// valid history (first frame, resize, camera cut, the pass off last frame), this frame's mask as is.
[numthreads(8, 8, 1)]
[RootSignature(LIGHT_SHAFTS_RS)]
void CSTemporal(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= lsSize.x || tid.y >= lsSize.y)
    {
        return;
    }
    const float3 current = SrcColor[tid.xy].rgb;
    float3 result = current;
    if (lsTemporal.z > 0.5f)
    {
        const float2 uv = (float2(tid.xy) + 0.5f) * lsInvSize.xy;
        const float2 prevUv = uv - VelocityTex.SampleLevel(gSmpPoint, uv, 0).xy;
        if (all(prevUv >= 0.0f) && all(prevUv <= 1.0f))
        {
            const float3 history = HistoryTex.SampleLevel(gSmpLinear, prevUv, 0).rgb * lsTemporal.y;
            result = lerp(history, current, saturate(lsTemporal.x));
        }
    }
    Out[tid.xy] = float4(result, 1.0f);
}

// usf:93-121: one radial blur pass, kNumSamples taps from the texel towards the sun. Half res.
[numthreads(8, 8, 1)]
[RootSignature(LIGHT_SHAFTS_RS)]
void CSBlur(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= lsSize.x || tid.y >= lsSize.y)
    {
        return;
    }
    const float2 uv = (float2(tid.xy) + 0.5f) * lsInvSize.xy;
    // usf:100: scale the UVs so the blur is the same PIXEL distance in x and y.
    const float2 aspectCorrectedUv = uv * float2(1.0f, lsOrigin.w);
    // usf:102: the blur distance grows exponentially with the pass index.
    const float passScale = pow(0.4f * (float)kNumSamples, (float)lsSize.z);
    // usf:103-105: the vector to the sun, never past the light's position -- then shortened by the
    // Shaft Length, every pass alike, so the three passes still tile the shorter ray.
    const float2 aspectCorrectedBlurVector =
        (lsOrigin.xy - aspectCorrectedUv) * (min(lsTint.w * passScale, 1.0f) * lsDepth.z);

    float3 blurred = 0.0f;
    [unroll]
    for (int sampleIndex = 0; sampleIndex < kNumSamples; ++sampleIndex)
    {
        // usf:112-115.
        const float2 sampleUv = (aspectCorrectedUv + aspectCorrectedBlurVector * ((float)sampleIndex / (float)kNumSamples))
                                * float2(1.0f, lsOrigin.z);
        const float2 clampedUv = clamp(sampleUv, lsInvSize.zw, 1.0f - lsInvSize.zw);
        blurred += SrcColor.SampleLevel(gSmpLinear, clampedUv, 0).rgb;
    }
    Out[tid.xy] = float4(blurred / (float)kNumSamples, 1.0f);
}

// usf:141-149 + the BF_One/BF_One blend: scene colour += the blurred half-res shafts. Render res.
[numthreads(8, 8, 1)]
[RootSignature(LIGHT_SHAFTS_RS)]
void CSApply(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= lsSize.x || tid.y >= lsSize.y)
    {
        return;
    }
    const float2 uv = (float2(tid.xy) + 0.5f) * lsInvSize.xy;
    float3 shafts = SrcColor.SampleLevel(gSmpLinear, clamp(uv, lsInvSize.zw, 1.0f - lsInvSize.zw), 0).rgb;
    // SHAFT LENGTH OVER 1 (not UE's; owner, 2026-10-13: "I wanted them LONGER"). After the full blur a pixel
    // averages the whole way back to the light, so a ray's brightness falls as 1/distance from it -- that,
    // not the blur's reach, is where UE's rays end. lsDepth.w = k lifts it to 1/distance^(1-k) past
    // kShaftLengthRef: 0 = UE's (no change anywhere), 1 = rays that do not fade before the screen's edge.
    if (lsDepth.w > 0.0f)
    {
        const float dist = length(lsOrigin.xy - uv * float2(1.0f, lsOrigin.w));
        shafts *= pow(max(dist, kShaftLengthRef) / kShaftLengthRef, lsDepth.w);
    }
    float4 color = Out[tid.xy];
    color.rgb += shafts;
    Out[tid.xy] = color;
}
