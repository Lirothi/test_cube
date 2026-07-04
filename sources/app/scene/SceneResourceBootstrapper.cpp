#include "app/scene/SceneResourceBootstrapper.h"

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
    depthA = material->ComputeCB0FieldHandle("depthA");
    depthB = material->ComputeCB0FieldHandle("depthB");
    zNear = material->ComputeCB0FieldHandle("zNear");
    zFar = material->ComputeCB0FieldHandle("zFar");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
    technique = material->ComputeCB0FieldHandle("tech");
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
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
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

    if (!matSSR_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/ssr_cs.hlsl";
        cd.csEntry = "CSMain";
        matSSR_ = mm->GetOrCreateCompute(renderer, cd);
    }

    if (!matOceanReflection_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/ocean_reflection_cs.hlsl";
        cd.csEntry = "CSMain";
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
        gd.depth.DepthEnable = TRUE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
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
    matSSR_->UpdateCBField(handles.depthA, data.depthA, dest);
    matSSR_->UpdateCBField(handles.depthB, data.depthB, dest);
    matSSR_->UpdateCBField(handles.zNear, data.zNear, dest);
    matSSR_->UpdateCBField(handles.zFar, data.zFar, dest);
    matSSR_->UpdateCBField(handles.screenSize, data.screenSize, dest);
    matSSR_->UpdateCBField(handles.invScreenSize, data.invScreenSize, dest);
    matSSR_->UpdateCBField(handles.technique, data.technique, dest);
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
    matComposeCS_->UpdateCBField(handles.screenSize, data.screenSize, dest);
    matComposeCS_->UpdateCBField(handles.invScreenSize, data.invScreenSize, dest);
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
