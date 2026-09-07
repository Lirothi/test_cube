// Light shafts (plan A7, docs/volumetric_fog_sky_clouds_ssgi_plan.md): UE's LightShaftBloom,
// transcribed from Shaders/Private/LightShaftShader.usf (148 lines) and
// Renderer/Private/LightShaftRendering.cpp -- three compute kernels over ONE cbuffer:
//   CSDownsample  usf:39-85 (#else = the bloom branch): half-res masked bloom source from scene colour + depth
//   CSBlur        usf:93-121: one radial blur pass towards the sun's screen position, NUM_SAMPLES 12
//   CSApply       usf:141-149, blend BF_One/BF_One (LightShaftRendering.cpp:530): add the result into scene colour
// The renderer runs downsample -> 3 blurs (r.LightShaftBlurPasses) -> apply, half resolution
// (r.LightShaftDownSampleFactor 2), after the transparents. UE's TAA pass between the downsample and
// the blurs is not transcribed (no TAA helper here; DLSS smooths the output, see the plan).
//
// Units: scene colour is stored PRE-EXPOSED, exactly as UE's is ("post exposure brightness",
// usf:70), so BloomThreshold / BloomMaxBrightness mean what they mean in UE. Depths are metres on
// both sides (UE cm against a cm range): the range is the same unit as the depth it masks.
//
// t0 SrcColor  -- downsample: scene colour (render res); blur/apply: the previous half-res result
// t1 DepthTex  -- downsample only (a dummy in the other two kernels; never sampled there)
// u0 Out       -- downsample/blur: the half-res target; apply: scene colour (read-modify-write)
// s0 linear clamp (UE GlobalBilinearClampedSampler)  s1 point clamp (depth)
#define LIGHT_SHAFTS_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

Texture2D           SrcColor : register(t0);
Texture2D           DepthTex : register(t1);
RWTexture2D<float4> Out      : register(u0);
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
    float4 lsDepth;
    // xy = this dispatch's target size, z = the blur pass index (RadialBlurParameters.z), w = 0.
    uint4  lsSize;
    // xy = 1 / lsSize.xy, zw = half a texel of the SOURCE: UE's UVMinMax bilinear inset.
    float4 lsInvSize;
};

static const int kNumSamples = 12; // r.LightShaftNumSamples, baked into the shader like UE's define

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
    const float3 sceneColor = SrcColor.SampleLevel(gSmpLinear, uv, 0).rgb;
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
    // usf:103-105: the vector to the sun, never past the light's position.
    const float2 aspectCorrectedBlurVector = (lsOrigin.xy - aspectCorrectedUv) * min(lsTint.w * passScale, 1.0f);

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
    const float3 shafts = SrcColor.SampleLevel(gSmpLinear, clamp(uv, lsInvSize.zw, 1.0f - lsInvSize.zw), 0).rgb;
    float4 color = Out[tid.xy];
    color.rgb += shafts;
    Out[tid.xy] = color;
}
