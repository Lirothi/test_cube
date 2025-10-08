#include "rendering/renderables/GlassCube.h"

#include <algorithm>
#include <array>

#include "app/Scene.h"
#include "rendering/core/Renderer.h"
#include "rendering/descriptors/SamplerManager.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/meshes/MeshManager.h"

using namespace Math;

namespace
{
    constexpr size_t kCascadeCount = 4;
}

class GlassCube::GlassUniformBinder final : public RenderableObject::UniformBinder
{
public:
    GlassUniformBinder(Scene* scene, GlassCube* owner)
        : scene_(scene)
        , owner_(owner)
    {
    }

    void RebuildHandles(RenderableObject& obj) override
    {
        cb_ = {};
        shadowCb_ = {};
        if (Material* material = obj.GetGraphicsMaterial())
        {
            cb_.world = material->ComputeCBFieldHandle(0, "world");
            cb_.view = material->ComputeCBFieldHandle(0, "view");
            cb_.proj = material->ComputeCBFieldHandle(0, "proj");
            cb_.invView = material->ComputeCBFieldHandle(0, "invView");
            cb_.invProj = material->ComputeCBFieldHandle(0, "invProj");
            cb_.cameraPosIor = material->ComputeCBFieldHandle(0, "cameraPosIor");
            cb_.absorptionThickness = material->ComputeCBFieldHandle(0, "absorptionThickness");
            cb_.tintRoughness = material->ComputeCBFieldHandle(0, "tintRoughness");
            cb_.reflectionRefraction = material->ComputeCBFieldHandle(0, "reflectionRefraction");
            cb_.sunDirAmbient = material->ComputeCBFieldHandle(0, "sunDirAmbient");
            cb_.sunColorExposure = material->ComputeCBFieldHandle(0, "sunColorExposure");
            cb_.camDirWS = material->ComputeCBFieldHandle(0, "camDirWS");
            cb_.screenSizeInv = material->ComputeCBFieldHandle(0, "screenSizeInv");
            cb_.shadowAtlasSizeInv = material->ComputeCBFieldHandle(0, "shadowAtlasSizeInv");
            cb_.shadowBiasNDC = material->ComputeCBFieldHandle(0, "shadowBiasNDC");
            cb_.normalBiasWS = material->ComputeCBFieldHandle(0, "normalBiasWS");
            cb_.cascadeSplitsVS = material->ComputeCBFieldHandle(0, "cascadeSplitsVS");
            cb_.cascadeScaleBias = material->ComputeCBFieldHandle(0, "cascadeScaleBias");
            cb_.spotShadowInfo = material->ComputeCBFieldHandle(0, "spotShadowInfo");
            cb_.lightCounts = material->ComputeCBFieldHandle(0, "lightCounts");
            cb_.lightViewProj = material->ComputeCBFieldHandle(0, "lightViewProj");
        }

        if (Material* shadowMaterial = obj.GetShadowMaterial())
        {
            shadowCb_.world = shadowMaterial->ComputeCBFieldHandle(0, "world");
            shadowCb_.view = shadowMaterial->ComputeCBFieldHandle(0, "view");
            shadowCb_.proj = shadowMaterial->ComputeCBFieldHandle(0, "proj");
        }
    }

    void UpdateMainCB(RenderableObject& obj, Renderer* renderer, const mat4& view, const mat4& proj, uint8_t* cbData) override
    {
        if (!renderer || !scene_ || !owner_)
        {
            return;
        }
        Material* material = obj.GetGraphicsMaterial();
        if (!material)
        {
            return;
        }

        const mat4 invView = mat4::Inverse(view);
        const mat4 invProj = mat4::Inverse(proj);
        const float3 camPos = scene_->CameraRef().GetPosition();
        float3 camDir = invView.TransformDirection(float3(0.0f, 0.0f, 1.0f));
        if (camDir.Length() > Math::EPS)
        {
            camDir = camDir.Normalized();
        }
        else
        {
            camDir = float3(0.0f, 0.0f, 1.0f);
        }

        UpdateUniform(obj, cb_.world, material, obj.GetModelMatrix(), cbData);
        UpdateUniform(obj, cb_.view, material, view, cbData);
        UpdateUniform(obj, cb_.proj, material, proj, cbData);
        UpdateUniform(obj, cb_.invView, material, invView, cbData);
        UpdateUniform(obj, cb_.invProj, material, invProj, cbData);

        UpdateUniform(obj, cb_.cameraPosIor, material, float4(camPos, owner_->ior_), cbData);
        UpdateUniform(obj, cb_.absorptionThickness, material, float4(owner_->absorption_, owner_->thickness_), cbData);
        UpdateUniform(obj, cb_.tintRoughness, material, float4(owner_->tint_, owner_->roughness_), cbData);

        float skyIntensity = 1.0f;
        if (auto* sky = scene_->GetSkybox())
        {
            skyIntensity = sky->GetExposure();
        }
        float normalInfo = owner_->HasNormalMap() ? 1.0f : 0.0f;
        UpdateUniform(obj, cb_.reflectionRefraction, material, float4(owner_->reflectionStrength_, owner_->refractionDistortion_, skyIntensity, normalInfo), cbData);

        const auto& dirLight = scene_->GetDirectionalLight();
        UpdateUniform(obj, cb_.sunDirAmbient, material, float4(dirLight.dir, dirLight.ambient), cbData);
        UpdateUniform(obj, cb_.sunColorExposure, material, float4(dirLight.color, dirLight.exposure), cbData);
        UpdateUniform(obj, cb_.camDirWS, material, float4(camDir, 0.0f), cbData);

        const float width = static_cast<float>(std::max(renderer->GetWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetHeight(), 1u));
        const float invWidth = width > 0.0f ? 1.0f / width : 0.0f;
        const float invHeight = height > 0.0f ? 1.0f / height : 0.0f;
        UpdateUniform(obj, cb_.screenSizeInv, material, float4(width, height, invWidth, invHeight), cbData);

        const auto& deferred = renderer->GetDeferredForFrame();
        const float shadowRes = static_cast<float>(std::max(deferred.shadowRes, 1u));
        const float invShadow = shadowRes > 0.0f ? 1.0f / shadowRes : 0.0f;
        UpdateUniform(obj, cb_.shadowAtlasSizeInv, material, float4(shadowRes, shadowRes, invShadow, invShadow), cbData);

        UpdateUniform(obj, cb_.shadowBiasNDC, material, float4(
            scene_->GetCascadeDepthBias(0),
            scene_->GetCascadeDepthBias(1),
            scene_->GetCascadeDepthBias(2),
            scene_->GetCascadeDepthBias(3)), cbData);

        UpdateUniform(obj, cb_.normalBiasWS, material, float4(
            scene_->GetCascadeNormalBias(0),
            scene_->GetCascadeNormalBias(1),
            scene_->GetCascadeNormalBias(2),
            scene_->GetCascadeNormalBias(3)), cbData);

        if (const float* splits = scene_->GetCascadeSplitsVS())
        {
            UpdateUniform(obj, cb_.cascadeSplitsVS, material, float4(splits[0], splits[1], splits[2], splits[3]), cbData);
        }

        for (size_t i = 0; i < kCascadeCount; ++i)
        {
            const mat4 viewProj = scene_->GetCascadeView(i) * scene_->GetCascadeProj(i);
            UpdateUniform(obj, cb_.lightViewProj, material, viewProj, cbData, static_cast<uint32_t>(i));
            const float2 scale = scene_->GetCascadeScale(i);
            const float2 bias = scene_->GetCascadeBias(i);
            UpdateUniform(obj, cb_.cascadeScaleBias, material, float4(scale.x, scale.y, bias.x, bias.y), cbData, static_cast<uint32_t>(i));
        }

        const float spotRes = static_cast<float>(std::max(deferred.spotShadowRes, 1u));
        const float invSpot = spotRes > 0.0f ? 1.0f / spotRes : 0.0f;
        UpdateUniform(obj, cb_.spotShadowInfo, material, float4(spotRes, spotRes, invSpot, invSpot), cbData);

        auto& lightManager = scene_->GetLightManager();
        const float pointCount = static_cast<float>(lightManager.PointLights().size());
        const float spotCount = static_cast<float>(lightManager.GetSpotLightCount());
        UpdateUniform(obj, cb_.lightCounts, material, float4(pointCount, spotCount, 0.0f, 0.0f), cbData);
    }

    void UpdateShadowCB(RenderableObject& obj, Renderer* /*renderer*/, const mat4& lightView, const mat4& lightProj, uint8_t* cbData) override
    {
        Material* shadowMaterial = obj.GetShadowMaterial();
        if (!shadowMaterial)
        {
            return;
        }

        UpdateUniform(obj, shadowCb_.world, shadowMaterial, obj.GetModelMatrix(), cbData);
        UpdateUniform(obj, shadowCb_.view, shadowMaterial, lightView, cbData);
        UpdateUniform(obj, shadowCb_.proj, shadowMaterial, lightProj, cbData);
    }

private:
    struct MainHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle view;
        Material::CBFieldHandle proj;
        Material::CBFieldHandle invView;
        Material::CBFieldHandle invProj;
        Material::CBFieldHandle cameraPosIor;
        Material::CBFieldHandle absorptionThickness;
        Material::CBFieldHandle tintRoughness;
        Material::CBFieldHandle reflectionRefraction;
        Material::CBFieldHandle sunDirAmbient;
        Material::CBFieldHandle sunColorExposure;
        Material::CBFieldHandle camDirWS;
        Material::CBFieldHandle screenSizeInv;
        Material::CBFieldHandle shadowAtlasSizeInv;
        Material::CBFieldHandle shadowBiasNDC;
        Material::CBFieldHandle normalBiasWS;
        Material::CBFieldHandle cascadeSplitsVS;
        Material::CBFieldHandle cascadeScaleBias;
        Material::CBFieldHandle spotShadowInfo;
        Material::CBFieldHandle lightCounts;
        Material::CBFieldHandle lightViewProj;
    } cb_{};

    struct ShadowHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle view;
        Material::CBFieldHandle proj;
    } shadowCb_{};

    Scene* scene_ = nullptr;
    GlassCube* owner_ = nullptr;
};

GlassCube::GlassCube(Scene* scene,
    const std::string& modelName,
    float3 position,
    float3 scale,
    float rotationSpeedRad)
    : RenderableObject("PosNormTanUV", L"shaders/glass.hlsl")
    , scene_(scene)
    , modelName_(modelName)
    , rotationSpeed_(rotationSpeedRad)
{
    pos_ = position;
    scale_ = scale;
    rotEuler_ = float3(0.0f, 0.0f, 0.0f);
    transformDirty_ = true;

    auto& gd = GetGraphicsDesc();
    gd.numRT = 1;
    gd.rtvFormats[0] = Renderer::kSceneColorFormat;
    gd.dsvFormat = Renderer::kDeferredDepthFormat;
    gd.depth.DepthEnable = TRUE;
    gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    gd.blend.RenderTarget[0].BlendEnable = TRUE;
    gd.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    gd.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    gd.blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    gd.blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    gd.blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    gd.blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    gd.raster.CullMode = D3D12_CULL_MODE_BACK;

    allowWireframe_ = false;

    SetUniformBinder(std::make_unique<GlassUniformBinder>(scene_, this));
}

void GlassCube::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);
    if (renderer && !modelName_.empty())
    {
        mesh_ = renderer->GetMeshManager()->Load(modelName_, renderer, uploadCmdList, uploadKeepAlive, { true, false, 0 });
    }
    hasNormalMap_ = false;
    if (renderer && uploadCmdList && !normalMapPath_.empty())
    {
        Texture2D::CreateDesc desc{};
        desc.path = normalMapPath_;
        desc.usage = Texture2D::Usage::NormalMap;
        desc.normalIsRG = normalMapIsRG_;
        hasNormalMap_ = normalMap_.CreateFromFile(renderer, uploadCmdList, desc, uploadKeepAlive);
    }
    transformDirty_ = true;
}

void GlassCube::Tick(float deltaTime)
{
    rotEuler_.y += rotationSpeed_ * deltaTime;
    if (rotEuler_.y > XM_2PI)
    {
        rotEuler_.y -= XM_2PI;
    }
    MarkTransformDirty();
}

void GlassCube::PostTick(float /*deltaTime*/)
{
    if (!transformDirty_)
    {
        return;
    }
    RebuildModel();
    transformDirty_ = false;
}

void GlassCube::PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* /*cl*/, RenderContext& ctx)
{
    if (!renderer || !scene_)
    {
        return;
    }

    const auto& deferred = renderer->GetDeferredForFrame();
    auto* sky = scene_->GetSkybox();

    D3D12_CPU_DESCRIPTOR_HANDLE normalSrv{};
    if (hasNormalMap_)
    {
        normalSrv = normalMap_.GetSRVCPU();
    }
    if (normalSrv.ptr == 0)
    {
        normalSrv = deferred.sceneSRV;
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> srvs{
        deferred.sceneOpaqueSRV.ptr != 0 ? deferred.sceneOpaqueSRV : deferred.sceneSRV,
        deferred.shadowSRV,
        deferred.spotShadowSRV,
        sky ? sky->GetTex()->GetSRVCPU() : deferred.sceneSRV,
        normalSrv
    };
    ctx.table[0] = renderer->StageSrvUavTable(srvs).gpu;

    auto& lights = scene_->GetLightManager();
    const size_t pointCount = lights.PointLights().size();
    const size_t spotCount = lights.GetSpotLightCount();
    lights.EnsurePointLightBuffer(renderer, std::max<size_t>(pointCount, size_t(1)));
    lights.EnsureSpotLightBuffer(renderer, std::max<size_t>(spotCount, size_t(1)));

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> lightSrvs{
        lights.GetPointLightSrv(),
        lights.GetSpotLightSrv()
    };
    ctx.table[1] = renderer->StageSrvUavTable(lightSrvs).gpu;

    const auto samplerDescs = std::array{
        *SamplerManager::LinearClamp(),
        *SamplerManager::ComparisonLinearClamp(),
        *SamplerManager::LinearClamp()
    };
    ctx.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

}

void GlassCube::MarkTransformDirty()
{
    transformDirty_ = true;
}

void GlassCube::RebuildModel()
{
    mat4 T = mat4::Translation(pos_);
    mat4 S = mat4::Scaling(scale_);
    mat4 R = mat4::RotationFromEulerXYZRad(rotEuler_);
    SetModelMatrix(S * R * T);
}

void GlassCube::SetNormalMap(const std::wstring& path, bool normalIsRG)
{
    normalMapPath_ = path;
    normalMapIsRG_ = normalIsRG;
    hasNormalMap_ = false;

    auto& defs = GetGraphicsDesc().defines;
    defs.erase(std::remove_if(defs.begin(), defs.end(), [](const auto& def) { return def.first == "NORMALMAP_IS_RG"; }), defs.end());
    if (normalMapIsRG_)
    {
        defs.emplace_back("NORMALMAP_IS_RG", "1");
    }
}
