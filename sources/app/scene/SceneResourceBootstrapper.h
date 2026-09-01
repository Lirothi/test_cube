#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "core/math/Math.h"

#include "app/scene/SceneRenderConfig.h"
#include "rendering/core/PhotographicSettings.h" // P3: ColorPipelineSettings in the tonemap CB writer
#include "rendering/shadows/VirtualShadowMap.h" // vsm::kNumClipmapLevels (lighting CB mirror)
#include "materials/Material.h"

class Renderer;
class RenderableObjectBase;
class Skybox;
struct ID3D12GraphicsCommandList;

struct SceneLightingCBHandles
{
    Material::CBFieldHandle sunDir;
    Material::CBFieldHandle ambient;
    Material::CBFieldHandle lightRgb;
    Material::CBFieldHandle ambientRgb; // P4: the fill's own colour, see DirectionalLight
    Material::CBFieldHandle skyIrradianceEnabled; // F8
    Material::CBFieldHandle skyIrradianceScale;   // F8
    Material::CBFieldHandle gtaoEnabled;         // P6B
    Material::CBFieldHandle gtaoStrength;        // P6B
    Material::CBFieldHandle groundAlbedoRgb;     // P16.12 ground bounce
    Material::CBFieldHandle exposure;
    Material::CBFieldHandle camPos;
    Material::CBFieldHandle camDir;
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle lightViewProj;
    Material::CBFieldHandle cascadeScaleBias;
    Material::CBFieldHandle cascadeSplits;
    Material::CBFieldHandle shadowAtlasSize;
    Material::CBFieldHandle shadowBiasNDC;
    Material::CBFieldHandle cascadeTexelWS;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;
    Material::CBFieldHandle sunMetalSpec;
    Material::CBFieldHandle sunAngularSize;
    Material::CBFieldHandle useVsm;           // Step 24f: directional clipmap sampling
    Material::CBFieldHandle vsmDepthBias;
    Material::CBFieldHandle clipmapBaseExtent;
    Material::CBFieldHandle clipmapNormalBias;
    Material::CBFieldHandle clipmapDepthBiasDecay;    // per-level depth-bias shaping (see VsmClipmapShadow)
    Material::CBFieldHandle clipmapDepthBiasFloorNdc;
    Material::CBFieldHandle clipmapBlendWidth;
    Material::CBFieldHandle smrtRayCount;             // SMRT, docs/vsm_smrt_plan.md
    Material::CBFieldHandle smrtSamplesPerRay;
    Material::CBFieldHandle smrtRayLengthScale;
    Material::CBFieldHandle smrtExtrapolateMaxSlope;
    Material::CBFieldHandle smrtSourceRadius;
    Material::CBFieldHandle smrtTexelDitherScale;
    Material::CBFieldHandle smrtLevelMargin;
    Material::CBFieldHandle smrtFrameIndex;
    Material::CBFieldHandle smrtAdaptiveRayCount;
    Material::CBFieldHandle smrtScreenRayLength;
    Material::CBFieldHandle smrtScreenRaySamples;
    Material::CBFieldHandle viewProj;
    Material::CBFieldHandle projMatrix;
    Material::CBFieldHandle contactShadowLength;
    Material::CBFieldHandle contactShadowIntensity;
    Material::CBFieldHandle contactShadowSteps;
    Material::CBFieldHandle contactShadowLengthInWS;
    Material::CBFieldHandle contactShadowNormalOffset;
    Material::CBFieldHandle contactShadowGrazingFade;
    Material::CBFieldHandle contactShadowMinDist;
    Material::CBFieldHandle contactShadowMaxDist;
    Material::CBFieldHandle contactShadowFadeBand;
    Material::CBFieldHandle contactShadowThickness;
    Material::CBFieldHandle contactShadowFrameId;
    Material::CBFieldHandle clipmapViewProj;
    Material::CBFieldHandle clipmapUvNormal; // P16.16
    Material::CBFieldHandle causticsTint;      // rgb = tint, w = master enable
    Material::CBFieldHandle causticsParams0;
    Material::CBFieldHandle causticsParams1;
    Material::CBFieldHandle causticsParams2;
    Material::CBFieldHandle csmDebugMode;      // S0.3: Legacy CSM cascade-tint visualization
    Material::CBFieldHandle csmFilterMode;     // S8: soft-occlusion ramp + Gather tent kernel
    Material::CBFieldHandle csmFilterParams;   // S8: receiver bias / sharpen / over-blur
    Material::CBFieldHandle csmFadeParams;     // S10: cascade cross-fade + distance fade
    // The sky's indirect specular, moved here from compose so the SSR pass can see it.
    Material::CBFieldHandle enableSkySpecular, skySpecMipCount, skyboxIntensity;

    void Populate(Material* material);
};

struct ScenePointLightCBHandles
{
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle camPos;
    Material::CBFieldHandle lightCount;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;
    Material::CBFieldHandle invPointShadowSize;
    Material::CBFieldHandle useVsm;
    Material::CBFieldHandle vsmRefDist;
    Material::CBFieldHandle localLateralTexels;
    Material::CBFieldHandle localDepthPushTexels;
    Material::CBFieldHandle viewProj, projMatrix;          // contact shadows (S12)
    Material::CBFieldHandle contactShadowLength, contactShadowIntensity, contactShadowSteps,
                            contactShadowLengthInWS, contactShadowNormalOffset,
                            contactShadowGrazingFade, contactShadowMinDist, contactShadowMaxDist,
                            contactShadowFadeBand, contactShadowThickness, contactShadowFrameId,
                            contactShadowLocalMode;

    void Populate(Material* material);
};

struct SceneSpotLightCBHandles
{
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle camPos;
    Material::CBFieldHandle lightCount;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;
    Material::CBFieldHandle invShadowSize;
    Material::CBFieldHandle useVsm;
    Material::CBFieldHandle vsmRefDist;
    Material::CBFieldHandle localLateralTexels;
    Material::CBFieldHandle localDepthPushTexels;
    Material::CBFieldHandle viewProj, projMatrix;          // contact shadows (S12)
    Material::CBFieldHandle contactShadowLength, contactShadowIntensity, contactShadowSteps,
                            contactShadowLengthInWS, contactShadowNormalOffset,
                            contactShadowGrazingFade, contactShadowMinDist, contactShadowMaxDist,
                            contactShadowFadeBand, contactShadowThickness, contactShadowFrameId,
                            contactShadowLocalMode;

    void Populate(Material* material);
};

struct SceneSsrCBHandles
{
    Material::CBFieldHandle view;
    Material::CBFieldHandle proj;
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle clipToPrevClip;
    Material::CBFieldHandle depthA;
    Material::CBFieldHandle depthB;
    Material::CBFieldHandle zNear;
    Material::CBFieldHandle zFar;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;
    Material::CBFieldHandle technique;
    // UE SSRT marches the furthest-depth HZB; the closest pyramid is retained for debug/P9.
    Material::CBFieldHandle useHzb, hzbMipCount, frameIndexMod8, hzbSize, hzbInvSize;
    Material::CBFieldHandle sceneColorHistoryValid;
    Material::CBFieldHandle ueNumSteps, ueNumRays, ueGlossyRays;
    Material::CBFieldHandle ueIntensity, ueRoughnessMaskScale;
    Material::CBFieldHandle ueUseRoughnessTexture, ueRoughnessOverride;
    Material::CBFieldHandle invPrevPreExposure;
    Material::CBFieldHandle preExposure;

    void Populate(Material* material);
};

struct SceneBlurCBHandles
{
    Material::CBFieldHandle dir;
    Material::CBFieldHandle radius;
    Material::CBFieldHandle glossyScale;

    void Populate(Material* material);
};

struct SceneComposeCBHandles
{
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle skyboxIntensity;
    Material::CBFieldHandle camPos;
    Material::CBFieldHandle enableSkySpecular;
    Material::CBFieldHandle skySpecMipCount; // F8
    Material::CBFieldHandle gtaoEnabled;    // P6B
    Material::CBFieldHandle gtaoStrength;   // P6B
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;
    Material::CBFieldHandle shoreWetnessWindow;
    Material::CBFieldHandle shoreWetnessAppearance;
    Material::CBFieldHandle shoreWetnessFallback;
    Material::CBFieldHandle shoreWetnessBreakup;
    // P7 aerial perspective.
    Material::CBFieldHandle fogParams0, fogParams1, fogParams2, fogSunDir, fogSunColor, fogDebugView;
    Material::CBFieldHandle preExposure;

    void Populate(Material* material);
};

struct SceneFxaaCBHandles
{
    Material::CBFieldHandle invResolution;
    Material::CBFieldHandle subpix;
    Material::CBFieldHandle edgeThreshold;
    Material::CBFieldHandle edgeThresholdMin;

    void Populate(Material* material);
};

// P2 photographic camera.
struct SceneTonemapCBHandles
{
    Material::CBFieldHandle exposureEnabled;
    Material::CBFieldHandle preExposure;
    Material::CBFieldHandle preExposureActive;
    Material::CBFieldHandle toneCurve;
    Material::CBFieldHandle agxSlope;
    Material::CBFieldHandle agxPower;
    Material::CBFieldHandle agxSaturation;
    Material::CBFieldHandle gradeSaturation;
    Material::CBFieldHandle gradeContrast;
    Material::CBFieldHandle gradeGamma;
    Material::CBFieldHandle gradeGain;
    Material::CBFieldHandle gradeOffset;
    Material::CBFieldHandle filmSlope;
    Material::CBFieldHandle filmToe;
    Material::CBFieldHandle filmShoulder;
    Material::CBFieldHandle filmBlackClip;
    Material::CBFieldHandle filmWhiteClip;
    Material::CBFieldHandle localHighlightContrast;
    Material::CBFieldHandle localShadowContrast;
    Material::CBFieldHandle localDetailStrength;
    Material::CBFieldHandle localHighlightThreshold;
    Material::CBFieldHandle localShadowThreshold;
    // P8C-2o: UE's split -- the scene's own factor and the flare's, which PARTITION the light
    // rather than adding to it. See the block comment in tonemap_cs.hlsl.
    Material::CBFieldHandle bloomSceneApply;
    Material::CBFieldHandle bloomScatterApply;

    void Populate(Material* material);
};

struct SceneExposureHistogramCBHandles
{
    Material::CBFieldHandle sampleGridX;
    Material::CBFieldHandle sampleGridY;
    Material::CBFieldHandle minLogLum;
    Material::CBFieldHandle invPreExposure;
    Material::CBFieldHandle invLogLumRange;
    Material::CBFieldHandle maskStrength;
    Material::CBFieldHandle maskInnerRadius;
    Material::CBFieldHandle maskOuterRadius;
    Material::CBFieldHandle maskSkyBias;

    void Populate(Material* material);
};

// P3B base log-luminance layer.
struct SceneExposureBaseLumCBHandles
{
    Material::CBFieldHandle baseWidth;
    Material::CBFieldHandle baseHeight;
    Material::CBFieldHandle invPreExposure;   // P16.1

    void Populate(Material* material);
};

struct SceneExposureSolveCBHandles
{
    Material::CBFieldHandle minLogLum;
    Material::CBFieldHandle invPreExposure;
    Material::CBFieldHandle logLumRange;
    Material::CBFieldHandle lowPercentile;
    Material::CBFieldHandle highPercentile;
    Material::CBFieldHandle compensationEv;
    Material::CBFieldHandle manualCompensationEv; // P16.13
    Material::CBFieldHandle minEv100;
    Material::CBFieldHandle maxEv100;
    Material::CBFieldHandle deltaTime;
    Material::CBFieldHandle speedUp;
    Material::CBFieldHandle speedDown;
    Material::CBFieldHandle manualEv100;
    Material::CBFieldHandle autoExposure;
    Material::CBFieldHandle resetHistory;
    Material::CBFieldHandle startDistance;
    Material::CBFieldHandle exponentialUpM;
    Material::CBFieldHandle exponentialDownM;
    Material::CBFieldHandle blackBucketInfluence;

    void Populate(Material* material);
};

#if WITH_EDITOR
struct SceneSelectionOutlineCBHandles
{
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle selectedBit;
    Material::CBFieldHandle outlineRadius;
    Material::CBFieldHandle outlineColor;

    void Populate(Material* material);
};
#endif

// P6B GTAO. Mirrors the `GtaoCB` in shaders/gtao_cs.hlsl.
struct GtaoPassConstants
{
    mat4 view{};
    mat4 invProj{};
    float2 aoSize{};
    float2 invAoSize{};
    float depthA = 0.0f;
    float depthB = 0.0f;
    // World units, not pixels: a pixel radius makes occlusion grow and shrink with camera distance,
    // which reads as the whole scene breathing.
    float worldRadius = 0.75f;
    float thickness = 0.6f;
    float intensity = 1.0f;
    float fadeStart = 60.0f;
    float fadeEnd = 120.0f;
    float invTanHalfFovY = 1.0f;
    uint32_t numAngles = 2u;
    uint32_t numSteps = 6u;
    uint32_t frameIndex = 0u;
    // 0 = geometric normal rebuilt from depth (UE's default, r.GTAO.UseNormals=0).
    uint32_t useGBufferNormal = 0u;
    // P6C retrofit: sample the depth pyramid instead of flat depth during the horizon walk.
    uint32_t useHzb = 0u;
    uint32_t hzbMipBias = 0u;
    uint32_t hzbMipCount = 1u;
    // P16.4: the medium radius for the sky-fill channel, and its own mip bias. <= worldRadius
    // switches the second horizon walk off. See GtaoSettings for why one radius cannot do both.
    float skyRadius = 0.0f;
    uint32_t skyMipBias = 2u;
    // Mid-range intensity (0 = the sky walk's compute path off entirely).
    float skyIntensity = 1.0f;
    uint32_t pad2 = 0u;
    uint32_t pad3 = 0u;
};

struct GtaoHandles
{
    Material::CBFieldHandle view, invProj, aoSize, invAoSize, depthA, depthB;
    Material::CBFieldHandle worldRadius, thickness, intensity, fadeStart, fadeEnd;
    Material::CBFieldHandle invTanHalfFovY, numAngles, numSteps, frameIndex, useGBufferNormal;
    Material::CBFieldHandle useHzb, hzbMipBias, hzbMipCount;
    Material::CBFieldHandle skyRadius, skyMipBias, skyIntensity;
    void Populate(Material* material);
};

// P6B items 3-5. ONE layout shared by the denoise, temporal and upsample kernels (`GtaoFilterCB`
// in all three shaders): they need overlapping subsets, and one struct with one writer is one
// place to keep in sync instead of three that drift.
struct GtaoFilterConstants
{
    float2 aoSize{};        // half-res AO grid
    float2 invAoSize{};
    float2 outSize{};       // == aoSize for denoise/temporal, render resolution for the upsample
    float2 invOutSize{};
    float depthA = 0.0f;
    float depthB = 0.0f;
    float planeTolerance = 0.05f;
    float blendWeight = 0.1f;
    float upsampleTolerance = 0.02f;
    uint32_t historyValid = 0u;
    uint32_t filterRadius = 2u;
    float temporalClampRange = 0.35f;
};

// P6C hierarchical depth. Mirrors `HzbCB` in shaders/hzb_build_cs.hlsl.
// SSR temporal resolve. Mirrors `SsrTemporalCB` in shaders/ssr_temporal_cs.hlsl.
struct SsrTemporalConstants
{
    float2 texSize{ 1.0f, 1.0f };
    float2 invTexSize{ 1.0f, 1.0f };
    // UE's `AA_LERP 8` for ETAAPassConfig::ScreenSpaceReflections: this frame is worth 1/8.
    float blendWeight = 0.125f;
    uint32_t historyValid = 0u;
    float clampExpand = 0.5f;
    float pad0 = 0.0f;
};

struct SsrTemporalHandles
{
    Material::CBFieldHandle texSize, invTexSize, blendWeight, historyValid, clampExpand;
    void Populate(Material* material);
};

struct HzbPassConstants
{
    uint2 dstSize{ 1u, 1u };
    uint2 srcSize{ 1u, 1u };
    uint32_t fromDepth = 0u;
    uint32_t writeClosest = 0u; // Build the CLOSEST chain for debug/P9; UE SSR reads FURTHEST.
    uint32_t pad1 = 0u, pad2 = 0u;
};

// Texture-inspector preview. Mirrors `PreviewCB` in shaders/debug_preview_cs.hlsl.
struct DebugPreviewConstants
{
    uint2 previewSize{ 1u, 1u };
    float gain = 1.0f;
    uint32_t stretch = 0u;
    uint32_t showAlpha = 0u;
    uint32_t pad0 = 0u, pad1 = 0u, pad2 = 0u;
};

struct DebugPreviewHandles
{
    Material::CBFieldHandle previewSize, gain, stretch, showAlpha;
    void Populate(Material* material);
};

struct HzbHandles
{
    Material::CBFieldHandle dstSize, srcSize, fromDepth, writeClosest;
    void Populate(Material* material);
};

// P8 bloom. Mirrors `BloomCB` in shaders/bloom_cs.hlsl -- one struct for all three stages, because
// they are one shader and one PSO; `stage` selects which of the three produces the destination.
struct BloomPassConstants
{
    uint32_t stage = 0u;            // 0 setup, 1 downsample, 2 upsample
    uint32_t exposureEnabled = 0u;
    uint2 dstSize{ 1u, 1u };
    uint2 srcSize{ 1u, 1u };
    float threshold = 1.0f;
    float softKnee = 0.5f;
    float radius = 1.0f;
    uint32_t fireflyClamp = 1u;
    uint32_t pad0 = 0u, pad1 = 0u;
};

struct BloomHandles
{
    Material::CBFieldHandle stage, exposureEnabled, dstSize, srcSize;
    Material::CBFieldHandle threshold, softKnee, radius, fireflyClamp;
    void Populate(Material* material);
};

// P8C. Mirrors `BloomFftCB` in shaders/bloom_fft_cs.hlsl.
struct BloomFftConstants
{
    uint2 transformSize{ 1u, 1u };
    uint32_t isVertical = 0u;
    uint32_t isInverse = 0u;
    // 0 = transform, 1 = Hermitian spectral multiply, 2 = accumulate. See bloom_fft_cs.hlsl.
    uint32_t mode = 0u;
    uint32_t pad0 = 0u, pad1 = 0u, pad2 = 0u;
};

struct BloomFftHandles
{
    Material::CBFieldHandle transformSize, isVertical, isInverse, mode;
    void Populate(Material* material);
};

// P8C. Mirrors `BloomConvCB` in shaders/bloom_conv_cs.hlsl.
struct BloomConvConstants
{
    uint32_t convStage = 0u;
    uint32_t exposureEnabled = 0u;
    uint2 transformSize{ 1u, 1u };
    uint2 imageSize{ 1u, 1u };
    uint2 sourceSize{ 1u, 1u };
    float threshold = 1.0f;
    float softKnee = 0.5f;
    // P8C-5: UE's BloomConvolutionPreFilterMin/Max/Mult, all three ABSOLUTE
    // (EV14 units) like every other threshold in the bloom. Mult <= 0 = inactive.
    float preFilterMin = 0.0f;
    float preFilterMax = 0.0f;
    float preFilterMult = 0.0f;
    float kernelTint[3] = { 1.0f, 1.0f, 1.0f };   // P8C-6
    // P8C-2: the kernel is an image; these place it in the grid. See bloom_conv_cs.hlsl.
    float kernelSpanTexels = 1024.0f;
    uint32_t kernelBoxTaps = 1u;
    float kernelBoxStep = 0.0f;
    float kernelCoreRingUV = 0.0f;
    float kernelCenterUV[2] = { 0.5f, 0.5f };
    float anamorphicIntensity = 0.0f;
    float anamorphicLength = 0.28f;
    float anamorphicSigma = 1.5f;
    float anamorphicThreshold = 4.0f;
    float anamorphicChroma = 0.5f;
    float anamorphicTint[3] = { 1.0f, 1.0f, 1.0f };
    float streakWeight[3] = { 0.0f, 0.0f, 0.0f };
    float streakSrcWeight[3] = { 1.0f, 1.0f, 1.0f };
    uint2 streakOffsets{ 0u, 0u };
    uint32_t ghostCount = 0u;
    float ghostIntensity = 0.6f;
};

struct BloomConvHandles
{
    Material::CBFieldHandle convStage, exposureEnabled, transformSize, imageSize, sourceSize;
    Material::CBFieldHandle threshold, softKnee;
    Material::CBFieldHandle preFilterMin, preFilterMax, preFilterMult;
    Material::CBFieldHandle kernelTint;   // P8C-6
    Material::CBFieldHandle kernelSpanTexels, kernelBoxTaps, kernelBoxStep;
    Material::CBFieldHandle kernelCoreRingUV, kernelCenterUV;
    Material::CBFieldHandle anamorphicIntensity, anamorphicLength, anamorphicSigma;
    Material::CBFieldHandle anamorphicThreshold, anamorphicChroma;
    Material::CBFieldHandle anamorphicTint, streakWeight, streakSrcWeight, streakOffsets;
    Material::CBFieldHandle ghostCount, ghostIntensity;
    void Populate(Material* material);
};

// P8C-2 step 5a: the lens-flare bokeh scatter (lens_flare.hlsl), a tiny instanced graphics pass.
struct LensFlareConstants
{
    uint2 tileCount{ 1u, 1u };
    float flareRTSize[2] = { 1.0f, 1.0f };
    float srcInvSize[2] = { 1.0f, 1.0f };
    float tileSizeTexels = 2.0f;
    float kernelSizePx = 16.0f;
    float threshold = 1.0e-4f;
    float kernelAreaInverse = 1.0f;
};

struct LensFlareHandles
{
    Material::CBFieldHandle tileCount, flareRTSize, srcInvSize, tileSizeTexels;
    Material::CBFieldHandle kernelSizePx, threshold, kernelAreaInverse;
    void Populate(Material* material);
};

struct GtaoFilterHandles
{
    Material::CBFieldHandle aoSize, invAoSize, outSize, invOutSize, depthA, depthB;
    Material::CBFieldHandle planeTolerance, blendWeight, upsampleTolerance;
    Material::CBFieldHandle historyValid, filterRadius, temporalClampRange;
    void Populate(Material* material);
};

struct LightingPassConstants
{
    float3 sunDir{};
    float3 ambient{};
    float3 lightRgb{};
    float3 ambientRgb{ 1.0f, 1.0f, 1.0f }; // P4: sun colour by default, see GetEffectiveAmbientColor
    // F8: 0 keeps the flat fill, i.e. every level without prefiltered sky derivatives.
    uint32_t skyIrradianceEnabled = 0u;
    float skyIrradianceScale = 1.0f;
    // P6B items 6-7: dynamic AO. `enabled` 0 means the target is unread (it is not written when the
    // pass is off); `strength` is UE's AmbientOcclusionStaticFraction.
    uint32_t gtaoEnabled = 0u;
    float gtaoStrength = 1.0f;
    // P16.12: the ground's diffuse reflectance. 0 switches the bounce off; see DirectionalLight.
    float3 groundAlbedoRgb{ 0.25f, 0.25f, 0.25f };
    float exposure = 1.0f;
    float3 camPos{};
    float3 camDir{};
    mat4 invView{};
    mat4 invProj{};
    std::array<mat4, 4> lightViewProj{};
    std::array<float4, 4> cascadeScaleBias{};
    float4 cascadeSplits{};
    float2 shadowAtlasSize{};
    float4 shadowBiasNDC{};
    float4 cascadeTexelWS{};
    float2 screenSize{};
    float2 invScreenSize{};
    float sunMetalSpec = 0.0f;
    float sunAngularSize = 0.0f;
    uint32_t useVsm = 0;                          // Step 24f: 1 = sample the directional VSM clipmap
    float vsmDepthBias = 0.0f;
    float clipmapBaseExtent = 0.0f;               // finest clipmap level's world extent
    float clipmapNormalBias = 0.0f;               // receiver normal offset, UE units (P16.16)
    float clipmapDepthBiasDecay = 1.0f;           // bias(L) = max(vsmDepthBias * decay^L, floorNdc)
    float clipmapDepthBiasFloorNdc = 0.0f;        // already converted texels -> NDC on the CPU
    float clipmapBlendWidth = 0.0f;               // outer level fraction blended into parent; 0 = off
    // SMRT (docs/vsm_smrt_plan.md). MIRRORS lighting_cs.hlsl's block of the same names -- these
    // four sit together in both places, and the 16-byte rule is what decides where the pad goes.
    uint32_t smrtRayCount = 0;                    // 0 = single-tap SampleCmp path (default)
    uint32_t smrtSamplesPerRay = 8;
    float smrtRayLengthScale = 1.5f;
    float smrtExtrapolateMaxSlope = 0.05f;  // UE's 5.0 is CENTIMETRES; ours is metres
    float smrtSourceRadius = 0.0f;          // sin of the light's angular radius (Step 3)
    float smrtTexelDitherScale = 2.0f;
    float smrtLevelMargin = 1.0f;
    uint32_t smrtFrameIndex = 0;            // 0 = temporal dither off; else the frame phase
    uint32_t smrtAdaptiveRayCount = 1;
    float smrtScreenRayLength = 0.015f;
    uint32_t smrtScreenRaySamples = 4;
    mat4 viewProj{};              // camera world->clip, for the screen-space rays
    mat4 projMatrix{};            // camera view->clip; the contact ray's compare tolerance
    float contactShadowLength = 0.0f;       // 0 = off (master switch folds into this)
    float contactShadowIntensity = 1.0f;
    uint32_t contactShadowSteps = 8;
    uint32_t contactShadowLengthInWS = 0;   // 1 = length is METRES, 0 = multiple of view depth
    float contactShadowNormalOffset = 0.0f; // ours: FRACTION of the ray length
    float contactShadowGrazingFade = 0.0f;  // ours: NdotL below which the march is unreliable
    float contactShadowMinDist = 0.0f;      // ours: metres
    float contactShadowMaxDist = 0.0f;      // ours: metres, 0 = no far limit
    float contactShadowFadeBand = 10.0f;
    float contactShadowThickness = 0.5f;    // ours: FRACTION of the ray length; 0 = UE behaviour
    uint32_t contactShadowFrameId = 0;      // 0 = static dither; else (frame mod 8) + 1
    std::array<mat4, vsm::kNumClipmapLevels> clipmapViewProj{}; // == lighting_cs's clipmapViewProj
    mat4 clipmapUvNormal{}; // P16.16: receiver-plane transform (inverse transpose world->shadow UV)        // camera-centered ortho viewProj per clipmap level
    // Underwater caustics (see shaders/caustics.hlsli). causticsTint.w == 0 disables the block,
    // which is what a level without an ocean produces.
    float4 causticsTint{};
    float4 causticsParams0{};   // intensity, metres per tile, frames/sec, water level Y
    float4 causticsParams1{};   // depth fade, surface fade, up-facing gate, bias
    float4 causticsParams2{};   // dispersion, second-layer blend, time, world metres per pixel
    uint32_t csmDebugMode = 0;  // S0.3: 0 = off, 1 = cascade tint (Legacy CSM only)
    uint32_t csmFilterMode = 2; // S8 kernel: 0 = 3x3 box, 1 = 4x4 tent, 2 = 6x6 tent (UE default)
    float4 csmFilterParams{};   // S8: x = receiver bias, y = sharpen (shader units), z = over-blur
    float4 csmFadeParams{};     // S10: x = far split, y = blend fraction, z = distance fade
    // Mirrors the same three fields in ComposePassConstants; both passes must agree or the sky
    // term one adds and the other subtracts stop cancelling.
    uint32_t enableSkySpecular = 1;
    uint32_t skySpecMipCount = 0;
    float skyboxIntensity = 1.0f;
};

struct PointLightPassConstants
{
    mat4 invView{};
    mat4 invProj{};
    float3 camPos{};
    uint32_t lightCount = 0;
    float2 screenSize{};
    float2 invScreenSize{};
    float invPointShadowSize = 0.0f; // 1 / pointShadowRes (cube face texel, for PCF)
    uint32_t useVsm = 0;      // Rung 2 / Step 21
    float vsmRefDist = 10.0f;
    float localLateralTexels = 1.0f;   // VSM local-light bias (texels) — mirrors HLSL PointLightFrame
    float localDepthPushTexels = 0.5f;
    // Contact shadows (S12). Same member names as LightingPassConstants -- one shader function
    // and one CPU writer (render::contact::FillConstants) serve every light pass.
    mat4 viewProj{};
    mat4 projMatrix{};
    float contactShadowLength = 0.0f;
    float contactShadowIntensity = 1.0f;
    uint32_t contactShadowSteps = 8;
    uint32_t contactShadowLengthInWS = 0;
    float contactShadowNormalOffset = 0.0f;
    float contactShadowGrazingFade = 0.0f;
    float contactShadowMinDist = 0.0f;
    float contactShadowMaxDist = 0.0f;
    float contactShadowFadeBand = 10.0f;
    float contactShadowThickness = 0.5f;
    uint32_t contactShadowFrameId = 0;
    uint32_t contactShadowLocalMode = 1;    // local lights only; see render::contact::g_localMode
    float _vsmPad0 = 0.0f;
    float _vsmPad1 = 0.0f;
    float _vsmPad2 = 0.0f;
};

struct SpotLightPassConstants
{
    mat4 invView{};
    mat4 invProj{};
    float3 camPos{};
    uint32_t lightCount = 0;
    float2 screenSize{};
    float2 invScreenSize{};
    float2 invShadowSize{};
    uint32_t useVsm = 0;      // Rung 2 / Step 21
    float vsmRefDist = 10.0f;
    float localLateralTexels = 1.0f;   // VSM local-light bias (texels) — mirrors HLSL SpotLightFrame
    float localDepthPushTexels = 0.5f;
    // Contact shadows (S12). Same member names as LightingPassConstants -- one shader function
    // and one CPU writer (render::contact::FillConstants) serve every light pass.
    mat4 viewProj{};
    mat4 projMatrix{};
    float contactShadowLength = 0.0f;
    float contactShadowIntensity = 1.0f;
    uint32_t contactShadowSteps = 8;
    uint32_t contactShadowLengthInWS = 0;
    float contactShadowNormalOffset = 0.0f;
    float contactShadowGrazingFade = 0.0f;
    float contactShadowMinDist = 0.0f;
    float contactShadowMaxDist = 0.0f;
    float contactShadowFadeBand = 10.0f;
    float contactShadowThickness = 0.5f;
    uint32_t contactShadowFrameId = 0;
    uint32_t contactShadowLocalMode = 1;    // local lights only; see render::contact::g_localMode
    float _vsmPad0 = 0.0f;
    float _vsmPad1 = 0.0f;
};

struct SsrPassConstants
{
    mat4 view{};
    mat4 proj{};
    mat4 invView{};
    mat4 invProj{};
    // UE SSRT ReprojectHit camera fallback: current jittered clip -> previous jittered clip.
    mat4 clipToPrevClip{};
    float depthA = 0.0f;
    float depthB = 0.0f;
    float zNear = 0.1f;
    float zFar = 1000.0f;
    float2 screenSize{};
    float2 invScreenSize{};
    uint32_t technique = 0;
    // P6C step 6. `useHzb` = 0 makes the HiZ technique fall back to the log march, so a pyramid
    // that was not built this frame can never be traced against.
    uint32_t useHzb = 0;
    uint32_t hzbMipCount = 1;
    uint32_t frameIndexMod8 = 0;
    float2 hzbSize{};     // mip 0 dimensions in texels
    float2 hzbInvSize{};  // 1/hzbSize -- the tracer works in tile units, so it needs both
    // Previous full-HDR SceneColor can be sampled only after one matching-size frame and no cut.
    uint32_t sceneColorHistoryValid = 0;
    uint32_t ueNumSteps = 8u;
    uint32_t ueNumRays = 4u;
    uint32_t ueGlossyRays = 1u;
    // SSRParams.r/.g of SSRTReflections.usf (intensity, roughness-fade scale).
    float ueIntensity = 1.0f;
    float ueRoughnessMaskScale = -2.0f / 0.6f;
    uint32_t ueUseRoughnessTexture = 1u;
    float ueRoughnessOverride = 0.0f;
    // P16.1: 1 / the factor the SceneColor history was written with (1 = not pre-exposed).
    float invPrevPreExposure = 1.0f;
    float preExposure = 1.0f; // P16.8
};

struct BlurPassConstants
{
    float2 direction{};
    float radius = 1.0f;
    float glossyScale = 0.0f; // extra blur radius at full roughness (0 = sharp); drives glossy reflections
};

struct ComposePassConstants
{
    mat4 invView{};
    mat4 invProj{};
    float skyboxIntensity = 1.0f;
    float3 camPos{};
    uint32_t enableSkySpecular = 1u;
    // F8: 0 = no prefiltered derivatives for this sky, so compose keeps the legacy mip-chain
    // path and the image is unchanged. Otherwise the prefiltered cube's real mip count.
    uint32_t skySpecMipCount = 0u;
    // P6B items 6-7, same pair as the lighting CB.
    uint32_t gtaoEnabled = 0u;
    float gtaoStrength = 1.0f;
    float2 screenSize{};
    float2 invScreenSize{};
    float4 shoreWetnessWindow{};
    float4 shoreWetnessAppearance{};
    float4 shoreWetnessFallback{};
    float4 shoreWetnessBreakup{};
    float4 fogParams0{};   // density, height falloff, reference height, start distance
    float4 fogParams1{};   // max opacity, sun scatter strength, sun scatter exponent, sun scatter start
    float4 fogParams2{};   // sky blur, yzw reserved
    float4 fogSunDir{};
    float4 fogSunColor{};
    uint32_t fogDebugView = 0u;
    float preExposure = 1.0f;
};

struct FxaaPassConstants
{
    float2 invResolution{};
    float subpix = 0.75f;
    float edgeThreshold = 0.166f;
    float edgeThresholdMin = 0.0625f;
};

// P8C-2o: how the tonemap combines the scene with the bloom. These are UE's
// SceneColorApplyParameters and FFTMulitplyParameters (BloomFinalizeApplyConstants.usf), and they
// PARTITION the light rather than adding to it -- the defaults below are the pyramid's neutral
// case, where the scene passes through untouched and the flare is a plain additive term.
struct BloomApplyConstants
{
    std::array<float, 3> sceneApply{ 1.0f, 1.0f, 1.0f };
    std::array<float, 3> scatterApply{ 0.0f, 0.0f, 0.0f };
};

// P2 photographic camera. The log-luminance window is a compile-time constant of the metering,
// not an authored setting: it only has to be wide enough to contain any scene the histogram will
// ever see, and moving it would silently reinterpret every stored bin.
struct ExposureMeteringConstants
{
    static constexpr uint32_t kSampleGridX = 256;
    static constexpr uint32_t kSampleGridY = 144;
    static constexpr float kMinLogLum = -10.0f; // log2 luminance
    static constexpr float kMaxLogLum = 14.0f;  // 24 stops over 256 bins = 0.094 stops per bin

    float compensationEv = -0.15f;
    float manualCompensationEv = 0.0f; // P16.13: manual mode's own trim
    float minEv100 = -6.0f;
    float maxEv100 = 16.0f;
    float lowPercentile = 0.15f;
    float maskStrength = 0.7f;
    float maskInnerRadius = 0.35f;
    float maskOuterRadius = 1.0f;
    float maskSkyBias = 0.6f;
    float highPercentile = 0.80f;
    float speedUp = 3.0f;
    float speedDown = 1.0f;
    float manualEv100 = 0.0f;
    float deltaTime = 0.0f;
    uint32_t autoExposure = 1;
    uint32_t resetHistory = 0;
    float startDistance = 1.5f;
    // Slope-match factors so the exponential and linear halves of the adaptation join smoothly.
    // Derived from speed and startDistance on the CPU (see SceneRenderer), exactly as UE does.
    float exponentialUpM = 1.0f;
    float exponentialDownM = 1.0f;
    float blackBucketInfluence = 1.0f;
};

#if WITH_EDITOR
struct SelectionOutlinePassConstants
{
    float2 screenSize{};
    uint32_t selectedBit = 0;
    uint32_t outlineRadius = 1;
    float4 outlineColor{};
};
#endif

class SceneResourceBootstrapper
{
public:
    using UploadList = std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>;

    void Initialize(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, UploadList* uploadKeepAlive);
    void Finalize(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        ID3D12GraphicsCommandList* uploadCmdList, UploadList* uploadKeepAlive,
        Skybox* skybox);

    void RefreshMaterialHandles(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        Skybox* skybox);

    void EnsureMaterials(Renderer* renderer);

    std::shared_ptr<Material> GetLightingMaterial() const { return matLighting_; }
    std::shared_ptr<Material> GetPointLightMaterial() const { return matPointLightCS_; }
    std::shared_ptr<Material> GetSpotLightMaterial() const { return matSpotLightCS_; }
    std::shared_ptr<Material> GetComposeMaterial() const { return matComposeCS_; }
    std::shared_ptr<Material> GetTonemapMaterial() const { return matTonemapCS_; }
    std::shared_ptr<Material> GetFxaaMaterial() const { return matFxaaCS_; }
    std::shared_ptr<Material> GetExposureClearMaterial() const { return matExposureClearCS_; }
    std::shared_ptr<Material> GetExposureBuildMaterial() const { return matExposureBuildCS_; }
    std::shared_ptr<Material> GetExposureSolveMaterial() const { return matExposureSolveCS_; }
    std::shared_ptr<Material> GetExposureBaseLumMaterial() const { return matExposureBaseLumCS_; }
    std::shared_ptr<Material> GetGtaoMaterial() const { return matGtaoCS_; }
    UINT GetGtaoCBSizeBytes() const;
    void WriteGtaoConstants(const GtaoPassConstants& data, uint8_t* dest) const;
    std::shared_ptr<Material> GetGtaoFilterMaterial() const { return matGtaoFilterCS_; }
    std::shared_ptr<Material> GetGtaoTemporalMaterial() const { return matGtaoTemporalCS_; }
    std::shared_ptr<Material> GetSsrTemporalMaterial() const { return matSsrTemporalCS_; }
    std::shared_ptr<Material> GetGtaoUpsampleMaterial() const { return matGtaoUpsampleCS_; }
    std::shared_ptr<Material> GetHzbMaterial() const { return matHzbCS_; }
    std::shared_ptr<Material> GetBloomMaterial() const { return matBloomCS_; }
    std::shared_ptr<Material> GetBloomFftMaterial() const { return matBloomFftCS_; }
    std::shared_ptr<Material> GetBloomConvMaterial() const { return matBloomConvCS_; }
    std::shared_ptr<Material> GetDebugPreviewMaterial() const { return matDebugPreviewCS_; }
    UINT GetDebugPreviewCBSizeBytes() const;
    void WriteDebugPreviewConstants(const DebugPreviewConstants& data, uint8_t* dest) const;
    UINT GetHzbCBSizeBytes() const;
    void WriteHzbConstants(const HzbPassConstants& data, uint8_t* dest) const;
    UINT GetBloomCBSizeBytes() const;
    void WriteBloomConstants(const BloomPassConstants& data, uint8_t* dest) const;
    UINT GetBloomFftCBSizeBytes() const;
    void WriteBloomFftConstants(const BloomFftConstants& data, uint8_t* dest) const;
    UINT GetBloomConvCBSizeBytes() const;
    void WriteBloomConvConstants(const BloomConvConstants& data, uint8_t* dest) const;
    // P8C-2 step 5a: the lens-flare bokeh scatter (graphics: instanced quads, additive).
    std::shared_ptr<Material> GetLensFlareMaterial() const { return matLensFlare_; }
    UINT GetLensFlareCBSizeBytes() const;
    void WriteLensFlareConstants(const LensFlareConstants& data, uint8_t* dest) const;
    UINT GetSsrTemporalCBSizeBytes() const;
    void WriteSsrTemporalConstants(const SsrTemporalConstants& data, uint8_t* dest) const;
    UINT GetGtaoFilterCBSizeBytes() const;
    UINT GetGtaoTemporalCBSizeBytes() const;
    UINT GetGtaoUpsampleCBSizeBytes() const;
    void WriteGtaoFilterConstants(const GtaoFilterConstants& data, uint8_t* dest) const;
    void WriteGtaoTemporalConstants(const GtaoFilterConstants& data, uint8_t* dest) const;
    void WriteGtaoUpsampleConstants(const GtaoFilterConstants& data, uint8_t* dest) const;
    std::shared_ptr<Material> GetSsrMaterial() const { return matSSR_; }
    std::shared_ptr<Material> GetOceanReflectionMaterial() const { return matOceanReflection_; }
    std::shared_ptr<Material> GetBlurMaterial() const { return matBlur_; }
    std::shared_ptr<Material> GetDebugMaterial() const { return matDebug_; }
    // RW: rt_wind_deform_cs PSO (null on non-RT HW / before Finalize).
    std::shared_ptr<Material> GetRtWindDeformMaterial() const { return matRtWindDeform_; }
    std::shared_ptr<Material> GetRtDebugMaterial() const { return matRtDebug_; } // S6, null on non-RT HW
    std::shared_ptr<Material> GetRtReflectMaterial() const { return matRtReflect_; } // S7, null on non-RT HW
    // Gather-then-shade split of the opaque RT reflection (async prep); null on non-RT HW.
    std::shared_ptr<Material> GetRtTraceMaterial() const { return matRtTrace_; }
    std::shared_ptr<Material> GetRtResolveMaterial() const { return matRtResolve_; }
    std::shared_ptr<Material> GetGlassReflPrepassMaterial() const { return matGlassReflPrepass_; } // S15b, null on non-RT HW
#if WITH_EDITOR
    std::shared_ptr<Material> GetSelectionOutlineMaterial() const { return matSelectionOutlineCS_; }
    std::shared_ptr<Material> GetSelectionStencilMaterial() const { return matSelectionStencil_; }
#endif

    const SceneLightingCBHandles& LightingHandles() const { return lightingHandles_; }
    const ScenePointLightCBHandles& PointHandles() const { return pointHandles_; }
    const SceneSpotLightCBHandles& SpotHandles() const { return spotHandles_; }
    const SceneSsrCBHandles& SsrHandles() const { return ssrHandles_; }
    const SceneBlurCBHandles& BlurHandles() const { return blurHandles_; }
    const SceneComposeCBHandles& ComposeHandles() const { return composeHandles_; }
    const SceneFxaaCBHandles& FxaaHandles() const { return fxaaHandles_; }
#if WITH_EDITOR
    const SceneSelectionOutlineCBHandles& SelectionOutlineHandles() const { return selectionOutlineHandles_; }
#endif

    UINT GetLightingCBSizeBytes() const;
    UINT GetPointLightCBSizeBytes() const;
    UINT GetSpotLightCBSizeBytes() const;
    UINT GetSsrCBSizeBytes() const;
    UINT GetOceanReflectionCBSizeBytes() const;
    UINT GetBlurCBSizeBytes() const;
    UINT GetComposeCBSizeBytes() const;
    UINT GetFxaaCBSizeBytes() const;
    UINT GetTonemapCBSizeBytes() const;
    UINT GetExposureHistogramCBSizeBytes() const;
    UINT GetExposureSolveCBSizeBytes() const;
    UINT GetExposureBaseLumCBSizeBytes() const;
#if WITH_EDITOR
    UINT GetSelectionOutlineCBSizeBytes() const;
#endif

    void WriteLightingConstants(const LightingPassConstants& data, uint8_t* dest) const;
    void WritePointLightConstants(const PointLightPassConstants& data, uint8_t* dest) const;
    void WriteSpotLightConstants(const SpotLightPassConstants& data, uint8_t* dest) const;
    void WriteSsrConstants(const SsrPassConstants& data, uint8_t* dest) const;
    void WriteBlurConstants(const BlurPassConstants& data, uint8_t* dest) const;
    void WriteComposeConstants(const ComposePassConstants& data, uint8_t* dest) const;
    void WriteFxaaConstants(const FxaaPassConstants& data, uint8_t* dest) const;
    // P3B lives on the CAMERA now, so the tonemap needs both blocks: the colour pipeline for the
    // curve and grade, the camera for the local-exposure scales.
    void WriteTonemapConstants(bool exposureEnabled,
                               const render::ColorPipelineSettings& color,
                               const render::CameraExposureSettings& camera,
                               const BloomApplyConstants& bloomApply,
                               uint8_t* dest) const;
    void WriteExposureHistogramConstants(const ExposureMeteringConstants& data, uint8_t* dest) const;
    void WriteExposureSolveConstants(const ExposureMeteringConstants& data, uint8_t* dest) const;
    void WriteExposureBaseLumConstants(uint8_t* dest) const;
#if WITH_EDITOR
    void WriteSelectionOutlineConstants(const SelectionOutlinePassConstants& data, uint8_t* dest) const;
#endif

private:
    void RefreshObjectMaterials(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        Skybox* skybox);

    void RefreshHandles();

    std::shared_ptr<Material> matLighting_;
    std::shared_ptr<Material> matPointLightCS_;
    std::shared_ptr<Material> matSpotLightCS_;
    std::shared_ptr<Material> matComposeCS_;
    std::shared_ptr<Material> matTonemapCS_;
    std::shared_ptr<Material> matFxaaCS_;
    std::shared_ptr<Material> matExposureClearCS_;
    std::shared_ptr<Material> matExposureBuildCS_;
    std::shared_ptr<Material> matExposureSolveCS_;
    std::shared_ptr<Material> matExposureBaseLumCS_;
    std::shared_ptr<Material> matGtaoCS_;
    std::shared_ptr<Material> matGtaoFilterCS_;
    std::shared_ptr<Material> matGtaoTemporalCS_;
    std::shared_ptr<Material> matSsrTemporalCS_;
    std::shared_ptr<Material> matGtaoUpsampleCS_;
    std::shared_ptr<Material> matHzbCS_;
    std::shared_ptr<Material> matBloomCS_;
    std::shared_ptr<Material> matBloomFftCS_;
    std::shared_ptr<Material> matBloomConvCS_;
    std::shared_ptr<Material> matLensFlare_;
    std::shared_ptr<Material> matDebugPreviewCS_;
    std::shared_ptr<Material> matSSR_;
    std::shared_ptr<Material> matOceanReflection_;
    std::shared_ptr<Material> matBlur_;
    std::shared_ptr<Material> matDebug_;
    std::shared_ptr<Material> matRtDebug_;   // S6 RT debug viz (RayQuery cs_6_5); only on RT HW
    std::shared_ptr<Material> matRtReflect_;
    std::shared_ptr<Material> matRtTrace_;   // gather phase of the opaque RT reflection split
    std::shared_ptr<Material> matRtResolve_; // shade phase (the only RT consumer of lightT)
    std::shared_ptr<Material> matRtWindDeform_; // RW wind deform for dynamic BLASes  // S7 Tier-1 RT reflections (RayQuery cs_6_5); only on RT HW
    std::shared_ptr<Material> matGlassReflPrepass_; // S15b glass refl G-buffer prepass; only on RT HW
#if WITH_EDITOR
    std::shared_ptr<Material> matSelectionOutlineCS_;
    std::shared_ptr<Material> matSelectionStencil_;
#endif

    SceneLightingCBHandles lightingHandles_{};
    GtaoHandles gtaoHandles_{};
    // Same layout, three materials: field handles are per-material, so the shared struct still
    // needs one handle set each.
    GtaoFilterHandles gtaoFilterHandles_{};
    GtaoFilterHandles gtaoTemporalHandles_{};
    GtaoFilterHandles gtaoUpsampleHandles_{};
    HzbHandles hzbHandles_{};
    BloomHandles bloomHandles_{};
    BloomFftHandles bloomFftHandles_{};
    BloomConvHandles bloomConvHandles_{};
    LensFlareHandles lensFlareHandles_{};
    SsrTemporalHandles ssrTemporalHandles_{};
    DebugPreviewHandles debugPreviewHandles_{};
    ScenePointLightCBHandles pointHandles_{};
    SceneSpotLightCBHandles spotHandles_{};
    SceneSsrCBHandles ssrHandles_{};
    SceneBlurCBHandles blurHandles_{};
    SceneComposeCBHandles composeHandles_{};
    SceneFxaaCBHandles fxaaHandles_{};
    SceneTonemapCBHandles tonemapHandles_{};
    SceneExposureHistogramCBHandles exposureHistogramHandles_{};
    SceneExposureSolveCBHandles exposureSolveHandles_{};
    SceneExposureBaseLumCBHandles exposureBaseLumHandles_{};
#if WITH_EDITOR
    SceneSelectionOutlineCBHandles selectionOutlineHandles_{};
#endif
};
