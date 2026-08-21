#include "app/scene/SceneResourceBootstrapper.h"
#include "rendering/core/RenderConstants.h" // P16.1 g_preExposure

#include "rendering/core/Renderer.h"
#include "rendering/debug/DebugDraw.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/renderables/RenderableObject.h"

namespace
{
#if WITH_EDITOR
    constexpr UINT kEditorSelectionStencilBit = 0x80u;
#endif
}

void SceneLightingCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }

    sunDir = material->ComputeCB0FieldHandle("sunDirWS");
    ambient = material->ComputeCB0FieldHandle("ambientIntensity");
    lightRgb = material->ComputeCB0FieldHandle("lightRgb");
    ambientRgb = material->ComputeCB0FieldHandle("ambientRgb");
    skyIrradianceEnabled = material->ComputeCB0FieldHandle("skyIrradianceEnabled");
    skyIrradianceScale = material->ComputeCB0FieldHandle("skyIrradianceScale");
    gtaoEnabled = material->ComputeCB0FieldHandle("gtaoEnabled");
    gtaoStrength = material->ComputeCB0FieldHandle("gtaoStrength");
    groundAlbedoRgb = material->ComputeCB0FieldHandle("groundAlbedoRgb");
    exposure = material->ComputeCB0FieldHandle("exposure");
    camPos = material->ComputeCB0FieldHandle("camPosWS");
    camDir = material->ComputeCB0FieldHandle("camDirWS");
    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    lightViewProj = material->ComputeCB0FieldHandle("lightViewProj");
    cascadeScaleBias = material->ComputeCB0FieldHandle("cascadeScaleBias");
    cascadeSplits = material->ComputeCB0FieldHandle("cascadeSplitsVS");
    shadowAtlasSize = material->ComputeCB0FieldHandle("shadowAtlasSize");
    shadowBiasNDC = material->ComputeCB0FieldHandle("shadowBiasNDC");
    normalBiasWS = material->ComputeCB0FieldHandle("normalBiasWS");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
    sunMetalSpec = material->ComputeCB0FieldHandle("sunMetalSpec");
    sunAngularSize = material->ComputeCB0FieldHandle("sunAngularSize");
    useVsm = material->ComputeCB0FieldHandle("useVsm");
    vsmDepthBias = material->ComputeCB0FieldHandle("vsmDepthBias");
    clipmapBaseExtent = material->ComputeCB0FieldHandle("clipmapBaseExtent");
    clipmapNormalBias = material->ComputeCB0FieldHandle("clipmapNormalBias");
    clipmapDepthBiasDecay = material->ComputeCB0FieldHandle("clipmapDepthBiasDecay");
    clipmapDepthBiasFloorNdc = material->ComputeCB0FieldHandle("clipmapDepthBiasFloorNdc");
    clipmapBlendWidth = material->ComputeCB0FieldHandle("clipmapBlendWidth");
    clipmapViewProj = material->ComputeCB0FieldHandle("clipmapViewProj");
    clipmapUvNormal = material->ComputeCB0FieldHandle("clipmapUvNormal");
    causticsTint = material->ComputeCB0FieldHandle("causticsTint");
    causticsParams0 = material->ComputeCB0FieldHandle("causticsParams0");
    causticsParams1 = material->ComputeCB0FieldHandle("causticsParams1");
    causticsParams2 = material->ComputeCB0FieldHandle("causticsParams2");
    csmDebugMode = material->ComputeCB0FieldHandle("csmDebugMode");
    enableSkySpecular = material->ComputeCB0FieldHandle("enableSkySpecular");
    skySpecMipCount = material->ComputeCB0FieldHandle("skySpecMipCount");
    skyboxIntensity = material->ComputeCB0FieldHandle("skyboxIntensity");
}

void ScenePointLightCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }

    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    camPos = material->ComputeCB0FieldHandle("camPosWS");
    lightCount = material->ComputeCB0FieldHandle("lightCount");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
    invPointShadowSize = material->ComputeCB0FieldHandle("invPointShadowSize");
    useVsm = material->ComputeCB0FieldHandle("useVsm");
    vsmRefDist = material->ComputeCB0FieldHandle("vsmRefDist");
    localLateralTexels = material->ComputeCB0FieldHandle("localLateralTexels");
    localDepthPushTexels = material->ComputeCB0FieldHandle("localDepthPushTexels");
}

void SceneSpotLightCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }

    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    camPos = material->ComputeCB0FieldHandle("camPosWS");
    lightCount = material->ComputeCB0FieldHandle("lightCount");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
    invShadowSize = material->ComputeCB0FieldHandle("invShadowSize");
    useVsm = material->ComputeCB0FieldHandle("useVsm");
    vsmRefDist = material->ComputeCB0FieldHandle("vsmRefDist");
    localLateralTexels = material->ComputeCB0FieldHandle("localLateralTexels");
    localDepthPushTexels = material->ComputeCB0FieldHandle("localDepthPushTexels");
}

void SceneSsrCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }

    view = material->ComputeCB0FieldHandle("view");
    proj = material->ComputeCB0FieldHandle("proj");
    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    clipToPrevClip = material->ComputeCB0FieldHandle("clipToPrevClip");
    depthA = material->ComputeCB0FieldHandle("depthA");
    depthB = material->ComputeCB0FieldHandle("depthB");
    zNear = material->ComputeCB0FieldHandle("zNear");
    zFar = material->ComputeCB0FieldHandle("zFar");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
    technique = material->ComputeCB0FieldHandle("tech");
    useHzb = material->ComputeCB0FieldHandle("useHzb");
    hzbMipCount = material->ComputeCB0FieldHandle("hzbMipCount");
    frameIndexMod8 = material->ComputeCB0FieldHandle("frameIndexMod8");
    hzbSize = material->ComputeCB0FieldHandle("hzbSize");
    hzbInvSize = material->ComputeCB0FieldHandle("hzbInvSize");
    sceneColorHistoryValid = material->ComputeCB0FieldHandle("sceneColorHistoryValid");
    ueNumSteps = material->ComputeCB0FieldHandle("ueNumSteps");
    ueNumRays = material->ComputeCB0FieldHandle("ueNumRays");
    ueGlossyRays = material->ComputeCB0FieldHandle("ueGlossyRays");
    ueStartMipLevel = material->ComputeCB0FieldHandle("ueStartMipLevel");
    ueSlopeCompareToleranceScale = material->ComputeCB0FieldHandle("ueSlopeCompareToleranceScale");
    ueConfirmRetries = material->ComputeCB0FieldHandle("ueConfirmRetries");
    ueRefineSteps = material->ComputeCB0FieldHandle("ueRefineSteps");
    ueUseRoughnessTexture = material->ComputeCB0FieldHandle("ueUseRoughnessTexture");
    ueRoughnessOverride = material->ComputeCB0FieldHandle("ueRoughnessOverride");
    invPrevPreExposure = material->ComputeCB0FieldHandle("invPrevPreExposure");
    preExposure = material->ComputeCB0FieldHandle("preExposure");
}

void SceneBlurCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }

    dir = material->ComputeCB0FieldHandle("dir");
    radius = material->ComputeCB0FieldHandle("radius");
    glossyScale = material->ComputeCB0FieldHandle("glossyScale");
}

void SceneComposeCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }

    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    skyboxIntensity = material->ComputeCB0FieldHandle("skyboxIntensity");
    camPos = material->ComputeCB0FieldHandle("camPosWS");
    enableSkySpecular = material->ComputeCB0FieldHandle("enableSkySpecular");
    skySpecMipCount = material->ComputeCB0FieldHandle("skySpecMipCount");
    gtaoEnabled = material->ComputeCB0FieldHandle("gtaoEnabled");
    gtaoStrength = material->ComputeCB0FieldHandle("gtaoStrength");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
    shoreWetnessWindow = material->ComputeCB0FieldHandle("shoreWetnessWindow");
    shoreWetnessAppearance = material->ComputeCB0FieldHandle("shoreWetnessAppearance");
    shoreWetnessFallback = material->ComputeCB0FieldHandle("shoreWetnessFallback");
    shoreWetnessBreakup = material->ComputeCB0FieldHandle("shoreWetnessBreakup");
    fogParams0 = material->ComputeCB0FieldHandle("fogParams0");
    fogParams1 = material->ComputeCB0FieldHandle("fogParams1");
    fogParams2 = material->ComputeCB0FieldHandle("fogParams2");
    fogSunDir = material->ComputeCB0FieldHandle("fogSunDir");
    fogSunColor = material->ComputeCB0FieldHandle("fogSunColor");
    fogDebugView = material->ComputeCB0FieldHandle("fogDebugView");
    preExposure = material->ComputeCB0FieldHandle("preExposure");
}

void SceneFxaaCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }

    invResolution = material->ComputeCB0FieldHandle("invResolution");
    subpix = material->ComputeCB0FieldHandle("subpix");
    edgeThreshold = material->ComputeCB0FieldHandle("edgeThreshold");
    edgeThresholdMin = material->ComputeCB0FieldHandle("edgeThresholdMin");
}

void SceneTonemapCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }
    exposureEnabled = material->ComputeCB0FieldHandle("exposureEnabled");
    preExposure = material->ComputeCB0FieldHandle("preExposure");
    preExposureActive = material->ComputeCB0FieldHandle("preExposureActive");
    toneCurve = material->ComputeCB0FieldHandle("toneCurve");
    agxSlope = material->ComputeCB0FieldHandle("agxSlope");
    agxPower = material->ComputeCB0FieldHandle("agxPower");
    agxSaturation = material->ComputeCB0FieldHandle("agxSaturation");
    gradeSaturation = material->ComputeCB0FieldHandle("gradeSaturation");
    gradeContrast = material->ComputeCB0FieldHandle("gradeContrast");
    gradeGamma = material->ComputeCB0FieldHandle("gradeGamma");
    gradeGain = material->ComputeCB0FieldHandle("gradeGain");
    gradeOffset = material->ComputeCB0FieldHandle("gradeOffset");
    filmSlope = material->ComputeCB0FieldHandle("filmSlope");
    filmToe = material->ComputeCB0FieldHandle("filmToe");
    filmShoulder = material->ComputeCB0FieldHandle("filmShoulder");
    filmBlackClip = material->ComputeCB0FieldHandle("filmBlackClip");
    filmWhiteClip = material->ComputeCB0FieldHandle("filmWhiteClip");
    localHighlightContrast = material->ComputeCB0FieldHandle("localHighlightContrast");
    localShadowContrast = material->ComputeCB0FieldHandle("localShadowContrast");
    localDetailStrength = material->ComputeCB0FieldHandle("localDetailStrength");
    localHighlightThreshold = material->ComputeCB0FieldHandle("localHighlightThreshold");
    localShadowThreshold = material->ComputeCB0FieldHandle("localShadowThreshold");
    bloomIntensity = material->ComputeCB0FieldHandle("bloomIntensity");
}

void SceneExposureHistogramCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }
    sampleGridX = material->ComputeCB0FieldHandle("sampleGridX");
    sampleGridY = material->ComputeCB0FieldHandle("sampleGridY");
    minLogLum = material->ComputeCB0FieldHandle("minLogLum");
    invPreExposure = material->ComputeCB0FieldHandle("invPreExposure");
    invLogLumRange = material->ComputeCB0FieldHandle("invLogLumRange");
    maskStrength = material->ComputeCB0FieldHandle("maskStrength");
    maskInnerRadius = material->ComputeCB0FieldHandle("maskInnerRadius");
    maskOuterRadius = material->ComputeCB0FieldHandle("maskOuterRadius");
    maskSkyBias = material->ComputeCB0FieldHandle("maskSkyBias");
}

void SceneExposureBaseLumCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }
    baseWidth = material->ComputeCB0FieldHandle("baseWidth");
    invPreExposure = material->ComputeCB0FieldHandle("invPreExposure");
    baseHeight = material->ComputeCB0FieldHandle("baseHeight");
}

void SceneExposureSolveCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }
    minLogLum = material->ComputeCB0FieldHandle("minLogLum");
    invPreExposure = material->ComputeCB0FieldHandle("invPreExposure");
    logLumRange = material->ComputeCB0FieldHandle("logLumRange");
    lowPercentile = material->ComputeCB0FieldHandle("lowPercentile");
    highPercentile = material->ComputeCB0FieldHandle("highPercentile");
    compensationEv = material->ComputeCB0FieldHandle("compensationEv");
    manualCompensationEv = material->ComputeCB0FieldHandle("manualCompensationEv");
    minEv100 = material->ComputeCB0FieldHandle("minEv100");
    maxEv100 = material->ComputeCB0FieldHandle("maxEv100");
    deltaTime = material->ComputeCB0FieldHandle("deltaTime");
    speedUp = material->ComputeCB0FieldHandle("speedUp");
    speedDown = material->ComputeCB0FieldHandle("speedDown");
    manualEv100 = material->ComputeCB0FieldHandle("manualEv100");
    autoExposure = material->ComputeCB0FieldHandle("autoExposure");
    resetHistory = material->ComputeCB0FieldHandle("resetHistory");
    startDistance = material->ComputeCB0FieldHandle("startDistance");
    exponentialUpM = material->ComputeCB0FieldHandle("exponentialUpM");
    exponentialDownM = material->ComputeCB0FieldHandle("exponentialDownM");
    blackBucketInfluence = material->ComputeCB0FieldHandle("blackBucketInfluence");
}

#if WITH_EDITOR
void SceneSelectionOutlineCBHandles::Populate(Material* material)
{
    *this = {};
    if (!material)
    {
        return;
    }

    screenSize = material->ComputeCB0FieldHandle("screenSize");
    selectedBit = material->ComputeCB0FieldHandle("selectedBit");
    outlineRadius = material->ComputeCB0FieldHandle("outlineRadius");
    outlineColor = material->ComputeCB0FieldHandle("outlineColor");
}
#endif

void SceneResourceBootstrapper::Initialize(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, UploadList* uploadKeepAlive)
{
    if (!renderer)
    {
        return;
    }

    EnsureMaterials(renderer);
    RefreshHandles();
}

void SceneResourceBootstrapper::Finalize(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
    ID3D12GraphicsCommandList* uploadCmdList,
    UploadList* uploadKeepAlive,
    Skybox* skybox)
{
    for (const auto& obj : objects)
    {
        if (obj)
        {
            obj->Init(renderer, uploadCmdList, uploadKeepAlive);
        }
    }

    RefreshHandles();
    RefreshObjectMaterials(renderer, objects, skybox);
}

void SceneResourceBootstrapper::RefreshMaterialHandles(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
    Skybox* skybox)
{
    EnsureMaterials(renderer);
    RefreshHandles();
    RefreshObjectMaterials(renderer, objects, skybox);
}

void SceneResourceBootstrapper::EnsureMaterials(Renderer* renderer)
{
    if (!renderer)
    {
        return;
    }

    auto* mm = renderer->GetMaterialManager();
    if (!matLighting_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/lighting_cs.hlsl";
        cd.csEntry = "CSMain";
        matLighting_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matPointLightCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/pointlight_cs.hlsl";
        cd.csEntry = "CSMain";
        matPointLightCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matSpotLightCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/spotlight_cs.hlsl";
        cd.csEntry = "CSMain";
        matSpotLightCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matComposeCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/compose_cs.hlsl";
        cd.csEntry = "CSMain";
        matComposeCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matTonemapCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/tonemap_cs.hlsl";
        cd.csEntry = "CSMain";
        matTonemapCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matFxaaCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/fxaa_cs.hlsl";
        cd.csEntry = "CSMain";
        matFxaaCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    // P2: two entry points out of one histogram shader, plus the solve. Materials are keyed by
    // (file, entry), so the clear and the build are separate Material objects over the same file.
    if (!matExposureClearCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/exposure_histogram_cs.hlsl";
        cd.csEntry = "CSClear";
        matExposureClearCS_ = mm->GetOrCreateCompute(renderer, cd);
    }
    if (!matExposureBuildCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/exposure_histogram_cs.hlsl";
        cd.csEntry = "CSBuild";
        matExposureBuildCS_ = mm->GetOrCreateCompute(renderer, cd);
    }
    if (!matExposureBaseLumCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/exposure_baselum_cs.hlsl";
        cd.csEntry = "CSMain";
        matExposureBaseLumCS_ = mm->GetOrCreateCompute(renderer, cd);
    }
    if (!matExposureSolveCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/exposure_solve_cs.hlsl";
        cd.csEntry = "CSMain";
        matExposureSolveCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matGtaoCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/gtao_cs.hlsl";
        cd.csEntry = "CSMain";
        matGtaoCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matGtaoFilterCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/gtao_filter_cs.hlsl";
        cd.csEntry = "CSMain";
        matGtaoFilterCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matGtaoTemporalCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/gtao_temporal_cs.hlsl";
        cd.csEntry = "CSMain";
        matGtaoTemporalCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matSsrTemporalCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/ssr_temporal_cs.hlsl";
        cd.csEntry = "CSMain";
        matSsrTemporalCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matGtaoUpsampleCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/gtao_upsample_cs.hlsl";
        cd.csEntry = "CSMain";
        matGtaoUpsampleCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matHzbCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/hzb_build_cs.hlsl";
        cd.csEntry = "CSMain";
        matHzbCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matBloomCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/bloom_cs.hlsl";
        cd.csEntry = "CSMain";
        matBloomCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matBloomFftCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/bloom_fft_cs.hlsl";
        cd.csEntry = "CSMain";
        matBloomFftCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matBloomConvCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/bloom_conv_cs.hlsl";
        cd.csEntry = "CSMain";
        matBloomConvCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matDebugPreviewCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/debug_preview_cs.hlsl";
        cd.csEntry = "CSMain";
        matDebugPreviewCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matSSR_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/ssr_cs.hlsl";
        cd.csEntry = "CSMain";
        // Independent opt-ins: optimized keeps the legacy fallback; Batch4 only reschedules coarse reads.
        cd.defines.emplace_back("SSR_LOGMARCH_OPTIMIZED", "1");
        cd.defines.emplace_back("SSR_LOGMARCH_BATCH4", "1");
        // The tracer's two budgets, stated here so the P14 ablation is reproducible without
        // editing a shader. Compile-time on purpose: a constant-buffer bound cost ~4%.
        cd.defines.emplace_back("SSR_LOGMARCH_COARSE_STEPS", "128");
        cd.defines.emplace_back("SSR_LOGMARCH_REFINE_STEPS", "8");
        matSSR_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matOceanReflection_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/ocean_reflection_cs.hlsl";
        cd.csEntry = "CSMain";
        cd.defines.emplace_back("SSR_LOGMARCH_OPTIMIZED", "1");
        cd.defines.emplace_back("SSR_LOGMARCH_BATCH4", "1");
        matOceanReflection_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matBlur_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/reflection_blur_cs.hlsl";
        cd.csEntry = "CSMain";
        matBlur_ = mm->GetOrCreateCompute(renderer, cd);
    }

#if WITH_EDITOR
    if (!matSelectionOutlineCS_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/selection_outline_cs.hlsl";
        cd.csEntry = "CSMain";
        matSelectionOutlineCS_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matSelectionStencil_)
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/selection_stencil.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = "PosNormTanUV";
        gd.numRT = 0;
        gd.dsvFormat = renderer->GetDsvFormat();
        // Selection uses an x-ray silhouette: stencil the selected mesh even
        // where scene geometry is closer, then outline that projected contour.
        gd.depth.DepthEnable = FALSE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        gd.depth.StencilEnable = TRUE;
        gd.depth.StencilReadMask = 0xff;
        gd.depth.StencilWriteMask = kEditorSelectionStencilBit;
        gd.depth.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        gd.depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        gd.depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        gd.depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        gd.depth.BackFace = gd.depth.FrontFace;
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        matSelectionStencil_ = mm->GetOrCreateGraphics(renderer, gd);
    }
#endif

    // S6 RT debug viz: a cs_6_5 RayQuery shader. Only built on RT-capable
    // hardware (else the cs_6_5 compile would fail / fall back); the debug pass
    // is gated on the same support, so it stays null otherwise.
    if (!matRtDebug_ && renderer->IsRaytracingSupported())
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/rt_debug_cs.hlsl";
        cd.csEntry = "CSMain";
        matRtDebug_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matRtReflect_ && renderer->IsRaytracingSupported())
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/rt_reflections_cs.hlsl";
        cd.csEntry = "CSMain";
        matRtReflect_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matRtDenoise_ && renderer->IsRaytracingSupported())
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/rt_reflection_denoise_cs.hlsl";
        cd.csEntry = "CSMain";
        matRtDenoise_ = mm->GetOrCreateCompute(renderer, cd);
    }

    // S15b: glass reflection G-buffer prepass PSO (front-face normal RTV + depth DSV). Built on
    // all HW — glass off-screen reflections work in SSR mode too (ssr_cs), not just RT.
    if (!matGlassReflPrepass_)
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/glass_refl_prepass.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = "PosNormTanUV";
        gd.numRT = 1;
        gd.rtvFormats[0] = renderer->GetGBuffer1Format();
        gd.dsvFormat = renderer->GetDsvFormat();
        gd.depth.DepthEnable = TRUE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // reverse-Z, front-most wins
        gd.raster.CullMode = D3D12_CULL_MODE_BACK;
        matGlassReflPrepass_ = mm->GetOrCreateGraphics(renderer, gd);
    }

    if (!matDebug_)
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/debug_texture.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = "";
        gd.numRT = 1;
        gd.rtvFormats[0] = renderer->GetBackbufferFormat();
        gd.dsvFormat = DXGI_FORMAT_UNKNOWN;
        gd.depth.DepthEnable = FALSE;
        matDebug_ = mm->GetOrCreateGraphics(renderer, gd);
    }
}

void SceneResourceBootstrapper::RefreshObjectMaterials(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
    Skybox* skybox)
{
    for (const auto& obj : objects)
    {
        if (obj)
        {
            obj->OnMaterialHotReload(renderer);
        }
    }

    if (skybox)
    {
        skybox->OnMaterialHotReload(renderer);
    }
}

void SceneResourceBootstrapper::RefreshHandles()
{
    lightingHandles_.Populate(matLighting_.get());
    pointHandles_.Populate(matPointLightCS_.get());
    spotHandles_.Populate(matSpotLightCS_.get());
    composeHandles_.Populate(matComposeCS_.get());
    fxaaHandles_.Populate(matFxaaCS_.get());
    tonemapHandles_.Populate(matTonemapCS_.get());
    exposureHistogramHandles_.Populate(matExposureBuildCS_.get());
    exposureSolveHandles_.Populate(matExposureSolveCS_.get());
    exposureBaseLumHandles_.Populate(matExposureBaseLumCS_.get());
    gtaoHandles_.Populate(matGtaoCS_.get());
    gtaoFilterHandles_.Populate(matGtaoFilterCS_.get());
    gtaoTemporalHandles_.Populate(matGtaoTemporalCS_.get());
    ssrTemporalHandles_.Populate(matSsrTemporalCS_.get());
    gtaoUpsampleHandles_.Populate(matGtaoUpsampleCS_.get());
    hzbHandles_.Populate(matHzbCS_.get());
    bloomHandles_.Populate(matBloomCS_.get());
    bloomFftHandles_.Populate(matBloomFftCS_.get());
    bloomConvHandles_.Populate(matBloomConvCS_.get());
    debugPreviewHandles_.Populate(matDebugPreviewCS_.get());
    ssrHandles_.Populate(matSSR_.get());
    blurHandles_.Populate(matBlur_.get());
#if WITH_EDITOR
    selectionOutlineHandles_.Populate(matSelectionOutlineCS_.get());
#endif
}

UINT SceneResourceBootstrapper::GetLightingCBSizeBytes() const
{
    return matLighting_ ? matLighting_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetPointLightCBSizeBytes() const
{
    return matPointLightCS_ ? matPointLightCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetSpotLightCBSizeBytes() const
{
    return matSpotLightCS_ ? matSpotLightCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetSsrCBSizeBytes() const
{
    return matSSR_ ? matSSR_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetOceanReflectionCBSizeBytes() const
{
    return matOceanReflection_ ? matOceanReflection_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetBlurCBSizeBytes() const
{
    return matBlur_ ? matBlur_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetComposeCBSizeBytes() const
{
    return matComposeCS_ ? matComposeCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetFxaaCBSizeBytes() const
{
    return matFxaaCS_ ? matFxaaCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetTonemapCBSizeBytes() const
{
    return matTonemapCS_ ? matTonemapCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetExposureHistogramCBSizeBytes() const
{
    return matExposureBuildCS_ ? matExposureBuildCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void GtaoHandles::Populate(Material* material)
{
    if (!material) { return; }
    view = material->ComputeCB0FieldHandle("view");
    invProj = material->ComputeCB0FieldHandle("invProj");
    aoSize = material->ComputeCB0FieldHandle("aoSize");
    invAoSize = material->ComputeCB0FieldHandle("invAoSize");
    depthA = material->ComputeCB0FieldHandle("depthA");
    depthB = material->ComputeCB0FieldHandle("depthB");
    worldRadius = material->ComputeCB0FieldHandle("worldRadius");
    thickness = material->ComputeCB0FieldHandle("thickness");
    intensity = material->ComputeCB0FieldHandle("intensity");
    fadeStart = material->ComputeCB0FieldHandle("fadeStart");
    fadeEnd = material->ComputeCB0FieldHandle("fadeEnd");
    invTanHalfFovY = material->ComputeCB0FieldHandle("invTanHalfFovY");
    numAngles = material->ComputeCB0FieldHandle("numAngles");
    numSteps = material->ComputeCB0FieldHandle("numSteps");
    frameIndex = material->ComputeCB0FieldHandle("frameIndex");
    useGBufferNormal = material->ComputeCB0FieldHandle("useGBufferNormal");
    useHzb = material->ComputeCB0FieldHandle("useHzb");
    hzbMipBias = material->ComputeCB0FieldHandle("hzbMipBias");
    hzbMipCount = material->ComputeCB0FieldHandle("hzbMipCount");
    skyRadius = material->ComputeCB0FieldHandle("skyRadius");
    skyMipBias = material->ComputeCB0FieldHandle("skyMipBias");
    skyIntensity = material->ComputeCB0FieldHandle("skyIntensity");
}

UINT SceneResourceBootstrapper::GetGtaoCBSizeBytes() const
{
    return matGtaoCS_ ? matGtaoCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void SceneResourceBootstrapper::WriteGtaoConstants(const GtaoPassConstants& d, uint8_t* dest) const
{
    if (!matGtaoCS_ || !dest) { return; }
    const auto& h = gtaoHandles_;
    matGtaoCS_->UpdateCBField(h.view, d.view, dest);
    matGtaoCS_->UpdateCBField(h.invProj, d.invProj, dest);
    matGtaoCS_->UpdateCBField(h.aoSize, d.aoSize, dest);
    matGtaoCS_->UpdateCBField(h.invAoSize, d.invAoSize, dest);
    matGtaoCS_->UpdateCBField(h.depthA, d.depthA, dest);
    matGtaoCS_->UpdateCBField(h.depthB, d.depthB, dest);
    matGtaoCS_->UpdateCBField(h.worldRadius, d.worldRadius, dest);
    matGtaoCS_->UpdateCBField(h.thickness, d.thickness, dest);
    matGtaoCS_->UpdateCBField(h.intensity, d.intensity, dest);
    matGtaoCS_->UpdateCBField(h.fadeStart, d.fadeStart, dest);
    matGtaoCS_->UpdateCBField(h.fadeEnd, d.fadeEnd, dest);
    matGtaoCS_->UpdateCBField(h.invTanHalfFovY, d.invTanHalfFovY, dest);
    matGtaoCS_->UpdateCBField(h.numAngles, d.numAngles, dest);
    matGtaoCS_->UpdateCBField(h.numSteps, d.numSteps, dest);
    matGtaoCS_->UpdateCBField(h.frameIndex, d.frameIndex, dest);
    matGtaoCS_->UpdateCBField(h.useGBufferNormal, d.useGBufferNormal, dest);
    matGtaoCS_->UpdateCBField(h.useHzb, d.useHzb, dest);
    matGtaoCS_->UpdateCBField(h.hzbMipBias, d.hzbMipBias, dest);
    matGtaoCS_->UpdateCBField(h.hzbMipCount, d.hzbMipCount, dest);
    matGtaoCS_->UpdateCBField(h.skyRadius, d.skyRadius, dest);
    matGtaoCS_->UpdateCBField(h.skyMipBias, d.skyMipBias, dest);
    matGtaoCS_->UpdateCBField(h.skyIntensity, d.skyIntensity, dest);
}

void GtaoFilterHandles::Populate(Material* material)
{
    if (!material) { return; }
    aoSize = material->ComputeCB0FieldHandle("aoSize");
    invAoSize = material->ComputeCB0FieldHandle("invAoSize");
    outSize = material->ComputeCB0FieldHandle("outSize");
    invOutSize = material->ComputeCB0FieldHandle("invOutSize");
    depthA = material->ComputeCB0FieldHandle("depthA");
    depthB = material->ComputeCB0FieldHandle("depthB");
    planeTolerance = material->ComputeCB0FieldHandle("planeTolerance");
    blendWeight = material->ComputeCB0FieldHandle("blendWeight");
    upsampleTolerance = material->ComputeCB0FieldHandle("upsampleTolerance");
    historyValid = material->ComputeCB0FieldHandle("historyValid");
    filterRadius = material->ComputeCB0FieldHandle("filterRadius");
    temporalClampRange = material->ComputeCB0FieldHandle("temporalClampRange");
}

namespace
{
// The three P6B filter kernels declare the SAME cbuffer, so one writer serves all of them; only
// the (material, handles) pair differs. Written out rather than repeated three times because the
// failure mode of a drifting copy is a field that silently keeps its default.
void WriteGtaoFilterCB(Material* material, const GtaoFilterHandles& h,
                       const GtaoFilterConstants& d, uint8_t* dest)
{
    if (!material || !dest) { return; }
    material->UpdateCBField(h.aoSize, d.aoSize, dest);
    material->UpdateCBField(h.invAoSize, d.invAoSize, dest);
    material->UpdateCBField(h.outSize, d.outSize, dest);
    material->UpdateCBField(h.invOutSize, d.invOutSize, dest);
    material->UpdateCBField(h.depthA, d.depthA, dest);
    material->UpdateCBField(h.depthB, d.depthB, dest);
    material->UpdateCBField(h.planeTolerance, d.planeTolerance, dest);
    material->UpdateCBField(h.blendWeight, d.blendWeight, dest);
    material->UpdateCBField(h.upsampleTolerance, d.upsampleTolerance, dest);
    material->UpdateCBField(h.historyValid, d.historyValid, dest);
    material->UpdateCBField(h.filterRadius, d.filterRadius, dest);
    material->UpdateCBField(h.temporalClampRange, d.temporalClampRange, dest);
}
} // namespace

void DebugPreviewHandles::Populate(Material* material)
{
    if (!material) { return; }
    previewSize = material->ComputeCB0FieldHandle("previewSize");
    gain = material->ComputeCB0FieldHandle("gain");
    stretch = material->ComputeCB0FieldHandle("stretch");
    showAlpha = material->ComputeCB0FieldHandle("showAlpha");
}

UINT SceneResourceBootstrapper::GetDebugPreviewCBSizeBytes() const
{
    return matDebugPreviewCS_
        ? matDebugPreviewCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void SceneResourceBootstrapper::WriteDebugPreviewConstants(const DebugPreviewConstants& d,
    uint8_t* dest) const
{
    if (!matDebugPreviewCS_ || !dest) { return; }
    matDebugPreviewCS_->UpdateCBField(debugPreviewHandles_.previewSize, d.previewSize, dest);
    matDebugPreviewCS_->UpdateCBField(debugPreviewHandles_.gain, d.gain, dest);
    matDebugPreviewCS_->UpdateCBField(debugPreviewHandles_.stretch, d.stretch, dest);
    matDebugPreviewCS_->UpdateCBField(debugPreviewHandles_.showAlpha, d.showAlpha, dest);
}

void HzbHandles::Populate(Material* material)
{
    if (!material) { return; }
    dstSize = material->ComputeCB0FieldHandle("dstSize");
    srcSize = material->ComputeCB0FieldHandle("srcSize");
    fromDepth = material->ComputeCB0FieldHandle("fromDepth");
    writeClosest = material->ComputeCB0FieldHandle("writeClosest");
}

UINT SceneResourceBootstrapper::GetHzbCBSizeBytes() const
{
    return matHzbCS_ ? matHzbCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void BloomHandles::Populate(Material* material)
{
    if (!material) { return; }
    stage = material->ComputeCB0FieldHandle("stage");
    exposureEnabled = material->ComputeCB0FieldHandle("exposureEnabled");
    dstSize = material->ComputeCB0FieldHandle("dstSize");
    srcSize = material->ComputeCB0FieldHandle("srcSize");
    threshold = material->ComputeCB0FieldHandle("threshold");
    softKnee = material->ComputeCB0FieldHandle("softKnee");
    radius = material->ComputeCB0FieldHandle("radius");
    fireflyClamp = material->ComputeCB0FieldHandle("fireflyClamp");
}

UINT SceneResourceBootstrapper::GetBloomCBSizeBytes() const
{
    return matBloomCS_ ? matBloomCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void BloomFftHandles::Populate(Material* material)
{
    if (!material) { return; }
    transformSize = material->ComputeCB0FieldHandle("transformSize");
    isVertical = material->ComputeCB0FieldHandle("isVertical");
    isInverse = material->ComputeCB0FieldHandle("isInverse");
    multiplyByKernel = material->ComputeCB0FieldHandle("multiplyByKernel");
}

UINT SceneResourceBootstrapper::GetBloomFftCBSizeBytes() const
{
    return matBloomFftCS_ ? matBloomFftCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void BloomConvHandles::Populate(Material* material)
{
    if (!material) { return; }
    convStage = material->ComputeCB0FieldHandle("convStage");
    exposureEnabled = material->ComputeCB0FieldHandle("exposureEnabled");
    transformSize = material->ComputeCB0FieldHandle("transformSize");
    imageSize = material->ComputeCB0FieldHandle("imageSize");
    sourceSize = material->ComputeCB0FieldHandle("sourceSize");
    threshold = material->ComputeCB0FieldHandle("threshold");
    softKnee = material->ComputeCB0FieldHandle("softKnee");
    kernelRadius = material->ComputeCB0FieldHandle("kernelRadius");
    blades = material->ComputeCB0FieldHandle("blades");
    bladeRotation = material->ComputeCB0FieldHandle("bladeRotation");
    spokeStrength = material->ComputeCB0FieldHandle("spokeStrength");
    spokeLength = material->ComputeCB0FieldHandle("spokeLength");
    spokeWidth = material->ComputeCB0FieldHandle("spokeWidth");
    anamorphic = material->ComputeCB0FieldHandle("anamorphic");
    anamorphicLength = material->ComputeCB0FieldHandle("anamorphicLength");
    chroma = material->ComputeCB0FieldHandle("chroma");
    ghostCount = material->ComputeCB0FieldHandle("ghostCount");
    ghostSpacing = material->ComputeCB0FieldHandle("ghostSpacing");
    ghostIntensity = material->ComputeCB0FieldHandle("ghostIntensity");
    ghostBokeh = material->ComputeCB0FieldHandle("ghostBokeh");
    sunUV = material->ComputeCB0FieldHandle("sunUV");
    sunOnScreen = material->ComputeCB0FieldHandle("sunOnScreen");
    apertureScale = material->ComputeCB0FieldHandle("apertureScale");
    psfLane = material->ComputeCB0FieldHandle("psfLane");
}

UINT SceneResourceBootstrapper::GetBloomConvCBSizeBytes() const
{
    return matBloomConvCS_ ? matBloomConvCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void SsrTemporalHandles::Populate(Material* material)
{
    if (!material) { return; }
    texSize = material->ComputeCB0FieldHandle("texSize");
    invTexSize = material->ComputeCB0FieldHandle("invTexSize");
    blendWeight = material->ComputeCB0FieldHandle("blendWeight");
    historyValid = material->ComputeCB0FieldHandle("historyValid");
    clampExpand = material->ComputeCB0FieldHandle("clampExpand");
}

UINT SceneResourceBootstrapper::GetSsrTemporalCBSizeBytes() const
{
    return matSsrTemporalCS_
        ? matSsrTemporalCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void SceneResourceBootstrapper::WriteSsrTemporalConstants(const SsrTemporalConstants& d,
                                                          uint8_t* dest) const
{
    if (!matSsrTemporalCS_ || !dest) { return; }
    matSsrTemporalCS_->UpdateCBField(ssrTemporalHandles_.texSize, d.texSize, dest);
    matSsrTemporalCS_->UpdateCBField(ssrTemporalHandles_.invTexSize, d.invTexSize, dest);
    matSsrTemporalCS_->UpdateCBField(ssrTemporalHandles_.blendWeight, d.blendWeight, dest);
    matSsrTemporalCS_->UpdateCBField(ssrTemporalHandles_.historyValid, d.historyValid, dest);
    matSsrTemporalCS_->UpdateCBField(ssrTemporalHandles_.clampExpand, d.clampExpand, dest);
}

void SceneResourceBootstrapper::WriteHzbConstants(const HzbPassConstants& d, uint8_t* dest) const
{
    if (!matHzbCS_ || !dest) { return; }
    matHzbCS_->UpdateCBField(hzbHandles_.dstSize, d.dstSize, dest);
    matHzbCS_->UpdateCBField(hzbHandles_.srcSize, d.srcSize, dest);
    matHzbCS_->UpdateCBField(hzbHandles_.fromDepth, d.fromDepth, dest);
    matHzbCS_->UpdateCBField(hzbHandles_.writeClosest, d.writeClosest, dest);
}

void SceneResourceBootstrapper::WriteBloomFftConstants(const BloomFftConstants& d, uint8_t* dest) const
{
    if (!matBloomFftCS_ || !dest) { return; }
    matBloomFftCS_->UpdateCBField(bloomFftHandles_.transformSize, d.transformSize, dest);
    matBloomFftCS_->UpdateCBField(bloomFftHandles_.isVertical, d.isVertical, dest);
    matBloomFftCS_->UpdateCBField(bloomFftHandles_.isInverse, d.isInverse, dest);
    matBloomFftCS_->UpdateCBField(bloomFftHandles_.multiplyByKernel, d.multiplyByKernel, dest);
}

void SceneResourceBootstrapper::WriteBloomConvConstants(const BloomConvConstants& d, uint8_t* dest) const
{
    if (!matBloomConvCS_ || !dest) { return; }
    const auto& h = bloomConvHandles_;
    matBloomConvCS_->UpdateCBField(h.convStage, d.convStage, dest);
    matBloomConvCS_->UpdateCBField(h.exposureEnabled, d.exposureEnabled, dest);
    matBloomConvCS_->UpdateCBField(h.transformSize, d.transformSize, dest);
    matBloomConvCS_->UpdateCBField(h.imageSize, d.imageSize, dest);
    matBloomConvCS_->UpdateCBField(h.sourceSize, d.sourceSize, dest);
    matBloomConvCS_->UpdateCBField(h.threshold, d.threshold, dest);
    matBloomConvCS_->UpdateCBField(h.softKnee, d.softKnee, dest);
    matBloomConvCS_->UpdateCBField(h.kernelRadius, d.kernelRadius, dest);
    matBloomConvCS_->UpdateCBField(h.blades, d.blades, dest);
    matBloomConvCS_->UpdateCBField(h.bladeRotation, d.bladeRotation, dest);
    matBloomConvCS_->UpdateCBField(h.spokeStrength, d.spokeStrength, dest);
    matBloomConvCS_->UpdateCBField(h.spokeLength, d.spokeLength, dest);
    matBloomConvCS_->UpdateCBField(h.spokeWidth, d.spokeWidth, dest);
    matBloomConvCS_->UpdateCBField(h.anamorphic, d.anamorphic, dest);
    matBloomConvCS_->UpdateCBField(h.anamorphicLength, d.anamorphicLength, dest);
    matBloomConvCS_->UpdateCBField(h.chroma, d.chroma, dest);
    matBloomConvCS_->UpdateCBField(h.ghostCount, d.ghostCount, dest);
    matBloomConvCS_->UpdateCBField(h.ghostSpacing, d.ghostSpacing, dest);
    matBloomConvCS_->UpdateCBField(h.ghostIntensity, d.ghostIntensity, dest);
    matBloomConvCS_->UpdateCBField(h.ghostBokeh, d.ghostBokeh, dest);
    matBloomConvCS_->UpdateCBField(h.sunUV, Math::float2(d.sunUV[0], d.sunUV[1]), dest);
    matBloomConvCS_->UpdateCBField(h.sunOnScreen, d.sunOnScreen, dest);
    matBloomConvCS_->UpdateCBField(h.apertureScale, d.apertureScale, dest);
    matBloomConvCS_->UpdateCBField(h.psfLane, d.psfLane, dest);
}

void SceneResourceBootstrapper::WriteBloomConstants(const BloomPassConstants& d, uint8_t* dest) const
{
    if (!matBloomCS_ || !dest) { return; }
    matBloomCS_->UpdateCBField(bloomHandles_.stage, d.stage, dest);
    matBloomCS_->UpdateCBField(bloomHandles_.exposureEnabled, d.exposureEnabled, dest);
    matBloomCS_->UpdateCBField(bloomHandles_.dstSize, d.dstSize, dest);
    matBloomCS_->UpdateCBField(bloomHandles_.srcSize, d.srcSize, dest);
    matBloomCS_->UpdateCBField(bloomHandles_.threshold, d.threshold, dest);
    matBloomCS_->UpdateCBField(bloomHandles_.softKnee, d.softKnee, dest);
    matBloomCS_->UpdateCBField(bloomHandles_.radius, d.radius, dest);
    matBloomCS_->UpdateCBField(bloomHandles_.fireflyClamp, d.fireflyClamp, dest);
}

UINT SceneResourceBootstrapper::GetGtaoFilterCBSizeBytes() const
{
    return matGtaoFilterCS_ ? matGtaoFilterCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetGtaoTemporalCBSizeBytes() const
{
    return matGtaoTemporalCS_ ? matGtaoTemporalCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

UINT SceneResourceBootstrapper::GetGtaoUpsampleCBSizeBytes() const
{
    return matGtaoUpsampleCS_ ? matGtaoUpsampleCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void SceneResourceBootstrapper::WriteGtaoFilterConstants(const GtaoFilterConstants& d, uint8_t* dest) const
{
    WriteGtaoFilterCB(matGtaoFilterCS_.get(), gtaoFilterHandles_, d, dest);
}

void SceneResourceBootstrapper::WriteGtaoTemporalConstants(const GtaoFilterConstants& d, uint8_t* dest) const
{
    WriteGtaoFilterCB(matGtaoTemporalCS_.get(), gtaoTemporalHandles_, d, dest);
}

void SceneResourceBootstrapper::WriteGtaoUpsampleConstants(const GtaoFilterConstants& d, uint8_t* dest) const
{
    WriteGtaoFilterCB(matGtaoUpsampleCS_.get(), gtaoUpsampleHandles_, d, dest);
}

UINT SceneResourceBootstrapper::GetExposureBaseLumCBSizeBytes() const
{
    return matExposureBaseLumCS_ ? matExposureBaseLumCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

void SceneResourceBootstrapper::WriteExposureBaseLumConstants(uint8_t* dest) const
{
    if (!matExposureBaseLumCS_ || !dest)
    {
        return;
    }
    matExposureBaseLumCS_->UpdateCBField(exposureBaseLumHandles_.baseWidth,
        ExposureMetering::kBaseLumWidth, dest);
    matExposureBaseLumCS_->UpdateCBField(exposureBaseLumHandles_.baseHeight,
        ExposureMetering::kBaseLumHeight, dest);
    matExposureBaseLumCS_->UpdateCBField(exposureBaseLumHandles_.invPreExposure,
        1.0f / std::max(render::g_preExposure, 1.0e-8f), dest);
}

UINT SceneResourceBootstrapper::GetExposureSolveCBSizeBytes() const
{
    return matExposureSolveCS_ ? matExposureSolveCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}

#if WITH_EDITOR
UINT SceneResourceBootstrapper::GetSelectionOutlineCBSizeBytes() const
{
    return matSelectionOutlineCS_ ? matSelectionOutlineCS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment) : 0u;
}
#endif

void SceneResourceBootstrapper::WriteLightingConstants(const LightingPassConstants& data, uint8_t* dest) const
{
    if (!matLighting_ || !dest)
    {
        return;
    }

    const auto& handles = lightingHandles_;
    matLighting_->UpdateCBField(handles.sunDir, data.sunDir, dest);
    matLighting_->UpdateCBField(handles.ambient, data.ambient, dest);
    matLighting_->UpdateCBField(handles.lightRgb, data.lightRgb, dest);
    matLighting_->UpdateCBField(handles.ambientRgb, data.ambientRgb, dest);
    matLighting_->UpdateCBField(handles.skyIrradianceEnabled, data.skyIrradianceEnabled, dest);
    matLighting_->UpdateCBField(handles.skyIrradianceScale, data.skyIrradianceScale, dest);
    matLighting_->UpdateCBField(handles.gtaoEnabled, data.gtaoEnabled, dest);
    matLighting_->UpdateCBField(handles.gtaoStrength, data.gtaoStrength, dest);
    matLighting_->UpdateCBField(handles.groundAlbedoRgb, data.groundAlbedoRgb, dest);
    matLighting_->UpdateCBField(handles.exposure, data.exposure, dest);
    matLighting_->UpdateCBField(handles.camPos, data.camPos, dest);
    matLighting_->UpdateCBField(handles.camDir, data.camDir, dest);
    matLighting_->UpdateCBField(handles.invView, data.invView, dest);
    matLighting_->UpdateCBField(handles.invProj, data.invProj, dest);

    for (size_t i = 0; i < data.lightViewProj.size(); ++i)
    {
        matLighting_->UpdateCBField(handles.lightViewProj, data.lightViewProj[i], dest, static_cast<uint32_t>(i));
    }
    for (size_t i = 0; i < data.cascadeScaleBias.size(); ++i)
    {
        matLighting_->UpdateCBField(handles.cascadeScaleBias, data.cascadeScaleBias[i], dest, static_cast<uint32_t>(i));
    }

    matLighting_->UpdateCBField(handles.cascadeSplits, data.cascadeSplits, dest);
    matLighting_->UpdateCBField(handles.shadowAtlasSize, data.shadowAtlasSize, dest);
    matLighting_->UpdateCBField(handles.shadowBiasNDC, data.shadowBiasNDC, dest);
    matLighting_->UpdateCBField(handles.normalBiasWS, data.normalBiasWS, dest);
    matLighting_->UpdateCBField(handles.screenSize, data.screenSize, dest);
    matLighting_->UpdateCBField(handles.invScreenSize, data.invScreenSize, dest);
    matLighting_->UpdateCBField(handles.sunMetalSpec, data.sunMetalSpec, dest);
    matLighting_->UpdateCBField(handles.sunAngularSize, data.sunAngularSize, dest);
    matLighting_->UpdateCBField(handles.useVsm, data.useVsm, dest);
    matLighting_->UpdateCBField(handles.vsmDepthBias, data.vsmDepthBias, dest);
    matLighting_->UpdateCBField(handles.clipmapBaseExtent, data.clipmapBaseExtent, dest);
    matLighting_->UpdateCBField(handles.clipmapNormalBias, data.clipmapNormalBias, dest);
    matLighting_->UpdateCBField(handles.clipmapDepthBiasDecay, data.clipmapDepthBiasDecay, dest);
    matLighting_->UpdateCBField(handles.clipmapDepthBiasFloorNdc, data.clipmapDepthBiasFloorNdc, dest);
    matLighting_->UpdateCBField(handles.clipmapBlendWidth, data.clipmapBlendWidth, dest);
    matLighting_->UpdateCBField(handles.causticsTint, data.causticsTint, dest);
    matLighting_->UpdateCBField(handles.causticsParams0, data.causticsParams0, dest);
    matLighting_->UpdateCBField(handles.causticsParams1, data.causticsParams1, dest);
    matLighting_->UpdateCBField(handles.causticsParams2, data.causticsParams2, dest);
    matLighting_->UpdateCBField(handles.csmDebugMode, data.csmDebugMode, dest);
    matLighting_->UpdateCBField(handles.enableSkySpecular, data.enableSkySpecular, dest);
    matLighting_->UpdateCBField(handles.skySpecMipCount, data.skySpecMipCount, dest);
    matLighting_->UpdateCBField(handles.skyboxIntensity, data.skyboxIntensity, dest);
    for (size_t i = 0; i < data.clipmapViewProj.size(); ++i)
    {
        matLighting_->UpdateCBField(handles.clipmapViewProj, data.clipmapViewProj[i], dest, static_cast<uint32_t>(i));
    }
    matLighting_->UpdateCBField(handles.clipmapUvNormal, data.clipmapUvNormal, dest); // P16.16
}

void SceneResourceBootstrapper::WritePointLightConstants(const PointLightPassConstants& data, uint8_t* dest) const
{
    if (!matPointLightCS_ || !dest)
    {
        return;
    }

    const auto& handles = pointHandles_;
    matPointLightCS_->UpdateCBField(handles.invView, data.invView, dest);
    matPointLightCS_->UpdateCBField(handles.invProj, data.invProj, dest);
    matPointLightCS_->UpdateCBField(handles.camPos, data.camPos, dest);
    matPointLightCS_->UpdateCBField(handles.lightCount, data.lightCount, dest);
    matPointLightCS_->UpdateCBField(handles.screenSize, data.screenSize, dest);
    matPointLightCS_->UpdateCBField(handles.invScreenSize, data.invScreenSize, dest);
    matPointLightCS_->UpdateCBField(handles.invPointShadowSize, data.invPointShadowSize, dest);
    matPointLightCS_->UpdateCBField(handles.useVsm, data.useVsm, dest);
    matPointLightCS_->UpdateCBField(handles.vsmRefDist, data.vsmRefDist, dest);
    matPointLightCS_->UpdateCBField(handles.localLateralTexels, data.localLateralTexels, dest);
    matPointLightCS_->UpdateCBField(handles.localDepthPushTexels, data.localDepthPushTexels, dest);
}

void SceneResourceBootstrapper::WriteSpotLightConstants(const SpotLightPassConstants& data, uint8_t* dest) const
{
    if (!matSpotLightCS_ || !dest)
    {
        return;
    }

    const auto& handles = spotHandles_;
    matSpotLightCS_->UpdateCBField(handles.invView, data.invView, dest);
    matSpotLightCS_->UpdateCBField(handles.invProj, data.invProj, dest);
    matSpotLightCS_->UpdateCBField(handles.camPos, data.camPos, dest);
    matSpotLightCS_->UpdateCBField(handles.lightCount, data.lightCount, dest);
    matSpotLightCS_->UpdateCBField(handles.screenSize, data.screenSize, dest);
    matSpotLightCS_->UpdateCBField(handles.invScreenSize, data.invScreenSize, dest);
    matSpotLightCS_->UpdateCBField(handles.invShadowSize, data.invShadowSize, dest);
    matSpotLightCS_->UpdateCBField(handles.useVsm, data.useVsm, dest);
    matSpotLightCS_->UpdateCBField(handles.vsmRefDist, data.vsmRefDist, dest);
    matSpotLightCS_->UpdateCBField(handles.localLateralTexels, data.localLateralTexels, dest);
    matSpotLightCS_->UpdateCBField(handles.localDepthPushTexels, data.localDepthPushTexels, dest);
}

void SceneResourceBootstrapper::WriteSsrConstants(const SsrPassConstants& data, uint8_t* dest) const
{
    if (!matSSR_ || !dest)
    {
        return;
    }

    const auto& handles = ssrHandles_;
    matSSR_->UpdateCBField(handles.view, data.view, dest);
    matSSR_->UpdateCBField(handles.proj, data.proj, dest);
    matSSR_->UpdateCBField(handles.invView, data.invView, dest);
    matSSR_->UpdateCBField(handles.invProj, data.invProj, dest);
    matSSR_->UpdateCBField(handles.clipToPrevClip, data.clipToPrevClip, dest);
    matSSR_->UpdateCBField(handles.depthA, data.depthA, dest);
    matSSR_->UpdateCBField(handles.depthB, data.depthB, dest);
    matSSR_->UpdateCBField(handles.zNear, data.zNear, dest);
    matSSR_->UpdateCBField(handles.zFar, data.zFar, dest);
    matSSR_->UpdateCBField(handles.screenSize, data.screenSize, dest);
    matSSR_->UpdateCBField(handles.invScreenSize, data.invScreenSize, dest);
    matSSR_->UpdateCBField(handles.technique, data.technique, dest);
    matSSR_->UpdateCBField(handles.useHzb, data.useHzb, dest);
    matSSR_->UpdateCBField(handles.hzbMipCount, data.hzbMipCount, dest);
    matSSR_->UpdateCBField(handles.frameIndexMod8, data.frameIndexMod8, dest);
    matSSR_->UpdateCBField(handles.hzbSize, data.hzbSize, dest);
    matSSR_->UpdateCBField(handles.hzbInvSize, data.hzbInvSize, dest);
    matSSR_->UpdateCBField(handles.sceneColorHistoryValid, data.sceneColorHistoryValid, dest);
    matSSR_->UpdateCBField(handles.ueNumSteps, data.ueNumSteps, dest);
    matSSR_->UpdateCBField(handles.ueNumRays, data.ueNumRays, dest);
    matSSR_->UpdateCBField(handles.ueGlossyRays, data.ueGlossyRays, dest);
    matSSR_->UpdateCBField(handles.ueStartMipLevel, data.ueStartMipLevel, dest);
    matSSR_->UpdateCBField(handles.ueSlopeCompareToleranceScale, data.ueSlopeCompareToleranceScale, dest);
    matSSR_->UpdateCBField(handles.ueConfirmRetries, data.ueConfirmRetries, dest);
    matSSR_->UpdateCBField(handles.ueRefineSteps, data.ueRefineSteps, dest);
    matSSR_->UpdateCBField(handles.ueUseRoughnessTexture, data.ueUseRoughnessTexture, dest);
    matSSR_->UpdateCBField(handles.ueRoughnessOverride, data.ueRoughnessOverride, dest);
    matSSR_->UpdateCBField(handles.invPrevPreExposure, data.invPrevPreExposure, dest);
    matSSR_->UpdateCBField(handles.preExposure, data.preExposure, dest);
}

void SceneResourceBootstrapper::WriteBlurConstants(const BlurPassConstants& data, uint8_t* dest) const
{
    if (!matBlur_ || !dest)
    {
        return;
    }

    const auto& handles = blurHandles_;
    matBlur_->UpdateCBField(handles.dir, data.direction, dest);
    matBlur_->UpdateCBField(handles.radius, data.radius, dest);
    matBlur_->UpdateCBField(handles.glossyScale, data.glossyScale, dest);
}

void SceneResourceBootstrapper::WriteTonemapConstants(bool exposureEnabled,
                                                      const render::ColorPipelineSettings& color,
                                                      const render::CameraExposureSettings& camera,
                                                      float bloomIntensity,
                                                      uint8_t* dest) const
{
    if (!matTonemapCS_ || !dest)
    {
        return;
    }
    const auto& h = tonemapHandles_;
    matTonemapCS_->UpdateCBField(h.exposureEnabled,
        static_cast<uint32_t>(exposureEnabled ? 1u : 0u), dest);
    // P16.1: the factor every writer of scene colour already applied, divided back out here.
    matTonemapCS_->UpdateCBField(h.preExposure, render::g_preExposure, dest);
    matTonemapCS_->UpdateCBField(h.preExposureActive,
        static_cast<uint32_t>(render::g_preExposureEnabled ? 1u : 0u), dest);
    matTonemapCS_->UpdateCBField(h.toneCurve,
        static_cast<uint32_t>(color.toneCurve), dest);
    matTonemapCS_->UpdateCBField(h.agxSlope, color.agxSlope, dest);
    matTonemapCS_->UpdateCBField(h.agxPower, color.agxPower, dest);
    matTonemapCS_->UpdateCBField(h.agxSaturation, color.agxSaturation, dest);
    matTonemapCS_->UpdateCBField(h.gradeSaturation, color.gradeSaturation, dest);
    matTonemapCS_->UpdateCBField(h.gradeContrast, color.gradeContrast, dest);
    matTonemapCS_->UpdateCBField(h.gradeGamma, color.gradeGamma, dest);
    matTonemapCS_->UpdateCBField(h.gradeGain, color.gradeGain, dest);
    matTonemapCS_->UpdateCBField(h.gradeOffset, color.gradeOffset, dest);
    matTonemapCS_->UpdateCBField(h.filmSlope, color.filmSlope, dest);
    matTonemapCS_->UpdateCBField(h.filmToe, color.filmToe, dest);
    matTonemapCS_->UpdateCBField(h.filmShoulder, color.filmShoulder, dest);
    matTonemapCS_->UpdateCBField(h.filmBlackClip, color.filmBlackClip, dest);
    matTonemapCS_->UpdateCBField(h.filmWhiteClip, color.filmWhiteClip, dest);
    matTonemapCS_->UpdateCBField(h.localHighlightContrast, camera.localHighlightContrast, dest);
    matTonemapCS_->UpdateCBField(h.localShadowContrast, camera.localShadowContrast, dest);
    matTonemapCS_->UpdateCBField(h.localDetailStrength, camera.localDetailStrength, dest);
    matTonemapCS_->UpdateCBField(h.localHighlightThreshold, camera.localHighlightThreshold, dest);
    matTonemapCS_->UpdateCBField(h.localShadowThreshold, camera.localShadowThreshold, dest);
    matTonemapCS_->UpdateCBField(h.bloomIntensity, bloomIntensity, dest);
}

void SceneResourceBootstrapper::WriteExposureHistogramConstants(const ExposureMeteringConstants& data,
                                                                uint8_t* dest) const
{
    if (!matExposureBuildCS_ || !dest)
    {
        return;
    }
    const auto& handles = exposureHistogramHandles_;
    const float range = ExposureMeteringConstants::kMaxLogLum - ExposureMeteringConstants::kMinLogLum;
    matExposureBuildCS_->UpdateCBField(handles.sampleGridX, ExposureMeteringConstants::kSampleGridX, dest);
    matExposureBuildCS_->UpdateCBField(handles.sampleGridY, ExposureMeteringConstants::kSampleGridY, dest);
    matExposureBuildCS_->UpdateCBField(handles.minLogLum, ExposureMeteringConstants::kMinLogLum, dest);
    matExposureBuildCS_->UpdateCBField(handles.invPreExposure,
                                       1.0f / std::max(render::g_preExposure, 1.0e-8f), dest);
    matExposureBuildCS_->UpdateCBField(handles.invLogLumRange, 1.0f / range, dest);
    matExposureBuildCS_->UpdateCBField(handles.maskStrength, data.maskStrength, dest);
    matExposureBuildCS_->UpdateCBField(handles.maskInnerRadius, data.maskInnerRadius, dest);
    matExposureBuildCS_->UpdateCBField(handles.maskOuterRadius, data.maskOuterRadius, dest);
    matExposureBuildCS_->UpdateCBField(handles.maskSkyBias, data.maskSkyBias, dest);
}

void SceneResourceBootstrapper::WriteExposureSolveConstants(const ExposureMeteringConstants& data, uint8_t* dest) const
{
    if (!matExposureSolveCS_ || !dest)
    {
        return;
    }
    const auto& handles = exposureSolveHandles_;
    const float range = ExposureMeteringConstants::kMaxLogLum - ExposureMeteringConstants::kMinLogLum;
    matExposureSolveCS_->UpdateCBField(handles.minLogLum, ExposureMeteringConstants::kMinLogLum, dest);
    matExposureSolveCS_->UpdateCBField(handles.logLumRange, range, dest);
    matExposureSolveCS_->UpdateCBField(handles.lowPercentile, data.lowPercentile, dest);
    matExposureSolveCS_->UpdateCBField(handles.highPercentile, data.highPercentile, dest);
    matExposureSolveCS_->UpdateCBField(handles.compensationEv, data.compensationEv, dest);
    matExposureSolveCS_->UpdateCBField(handles.manualCompensationEv, data.manualCompensationEv, dest);
    matExposureSolveCS_->UpdateCBField(handles.minEv100, data.minEv100, dest);
    matExposureSolveCS_->UpdateCBField(handles.maxEv100, data.maxEv100, dest);
    matExposureSolveCS_->UpdateCBField(handles.deltaTime, data.deltaTime, dest);
    matExposureSolveCS_->UpdateCBField(handles.speedUp, data.speedUp, dest);
    matExposureSolveCS_->UpdateCBField(handles.speedDown, data.speedDown, dest);
    matExposureSolveCS_->UpdateCBField(handles.manualEv100, data.manualEv100, dest);
    matExposureSolveCS_->UpdateCBField(handles.autoExposure, data.autoExposure, dest);
    matExposureSolveCS_->UpdateCBField(handles.resetHistory, data.resetHistory, dest);
    matExposureSolveCS_->UpdateCBField(handles.startDistance, data.startDistance, dest);
    matExposureSolveCS_->UpdateCBField(handles.exponentialUpM, data.exponentialUpM, dest);
    matExposureSolveCS_->UpdateCBField(handles.exponentialDownM, data.exponentialDownM, dest);
    matExposureSolveCS_->UpdateCBField(handles.blackBucketInfluence, data.blackBucketInfluence, dest);
}

void SceneResourceBootstrapper::WriteComposeConstants(const ComposePassConstants& data, uint8_t* dest) const
{
    if (!matComposeCS_ || !dest)
    {
        return;
    }

    const auto& handles = composeHandles_;
    matComposeCS_->UpdateCBField(handles.invView, data.invView, dest);
    matComposeCS_->UpdateCBField(handles.invProj, data.invProj, dest);
    matComposeCS_->UpdateCBField(handles.skyboxIntensity, data.skyboxIntensity, dest);
    matComposeCS_->UpdateCBField(handles.camPos, data.camPos, dest);
    matComposeCS_->UpdateCBField(handles.enableSkySpecular, data.enableSkySpecular, dest);
    matComposeCS_->UpdateCBField(handles.skySpecMipCount, data.skySpecMipCount, dest);
    matComposeCS_->UpdateCBField(handles.gtaoEnabled, data.gtaoEnabled, dest);
    matComposeCS_->UpdateCBField(handles.gtaoStrength, data.gtaoStrength, dest);
    matComposeCS_->UpdateCBField(handles.screenSize, data.screenSize, dest);
    matComposeCS_->UpdateCBField(handles.invScreenSize, data.invScreenSize, dest);
    matComposeCS_->UpdateCBField(handles.shoreWetnessWindow, data.shoreWetnessWindow, dest);
    matComposeCS_->UpdateCBField(
        handles.shoreWetnessAppearance, data.shoreWetnessAppearance, dest);
    matComposeCS_->UpdateCBField(
        handles.shoreWetnessFallback, data.shoreWetnessFallback, dest);
    matComposeCS_->UpdateCBField(
        handles.shoreWetnessBreakup, data.shoreWetnessBreakup, dest);
    matComposeCS_->UpdateCBField(handles.fogParams0, data.fogParams0, dest);
    matComposeCS_->UpdateCBField(handles.fogParams1, data.fogParams1, dest);
    matComposeCS_->UpdateCBField(handles.fogParams2, data.fogParams2, dest);
    matComposeCS_->UpdateCBField(handles.fogSunDir, data.fogSunDir, dest);
    matComposeCS_->UpdateCBField(handles.fogSunColor, data.fogSunColor, dest);
    matComposeCS_->UpdateCBField(handles.fogDebugView, data.fogDebugView, dest);
    matComposeCS_->UpdateCBField(handles.preExposure, data.preExposure, dest);
}

void SceneResourceBootstrapper::WriteFxaaConstants(const FxaaPassConstants& data, uint8_t* dest) const
{
    if (!matFxaaCS_ || !dest)
    {
        return;
    }

    const auto& handles = fxaaHandles_;
    matFxaaCS_->UpdateCBField(handles.invResolution, data.invResolution, dest);
    matFxaaCS_->UpdateCBField(handles.subpix, data.subpix, dest);
    matFxaaCS_->UpdateCBField(handles.edgeThreshold, data.edgeThreshold, dest);
    matFxaaCS_->UpdateCBField(handles.edgeThresholdMin, data.edgeThresholdMin, dest);
}

#if WITH_EDITOR
void SceneResourceBootstrapper::WriteSelectionOutlineConstants(const SelectionOutlinePassConstants& data, uint8_t* dest) const
{
    if (!matSelectionOutlineCS_ || !dest)
    {
        return;
    }

    const auto& handles = selectionOutlineHandles_;
    matSelectionOutlineCS_->UpdateCBField(handles.screenSize, data.screenSize, dest);
    matSelectionOutlineCS_->UpdateCBField(handles.selectedBit, data.selectedBit, dest);
    matSelectionOutlineCS_->UpdateCBField(handles.outlineRadius, data.outlineRadius, dest);
    matSelectionOutlineCS_->UpdateCBField(handles.outlineColor, data.outlineColor, dest);
}
#endif
