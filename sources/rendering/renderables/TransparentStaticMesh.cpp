#include "rendering/renderables/TransparentStaticMesh.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "app/scene/Scene.h"
#include "app/camera/Camera.h"
#include "rendering/core/Renderer.h"
#include "rendering/descriptors/SamplerManager.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/meshes/MeshManager.h"

using namespace Math;

namespace
{
    constexpr size_t kCascadeCount = 4;

    uint32_t ToObjectId32(std::uint64_t id)
    {
        return id > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(id);
    }
}

class TransparentStaticMesh::TransparentUniformBinder final : public RenderableObject::UniformBinder
{
public:
    TransparentUniformBinder(Scene* scene, TransparentStaticMesh* owner)
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
            // Per-object only (b0). All per-frame/per-view fields now live in the
            // shared GlassView CB (b1), filled once per pass.
            cb_.world = material->ComputeCBFieldHandle(0, "world");
            cb_.prevWorld = material->ComputeCBFieldHandle(0, "prevWorld");
            cb_.absorptionThickness = material->ComputeCBFieldHandle(0, "absorptionThickness");
            cb_.tintRoughness = material->ComputeCBFieldHandle(0, "tintRoughness");
            cb_.matExtra = material->ComputeCBFieldHandle(0, "matExtra");
            cb_.objectId = material->ComputeCBFieldHandle(0, "objectId");
        }

        if (Material* shadowMaterial = obj.GetShadowMaterial())
        {
            shadowCb_.world = shadowMaterial->ComputeCBFieldHandle(0, "world");
            shadowCb_.view = shadowMaterial->ComputeCBFieldHandle(0, "view");
            shadowCb_.proj = shadowMaterial->ComputeCBFieldHandle(0, "proj");
        }
    }

    void UpdateMainCB(RenderableObject& obj, Renderer* /*renderer*/, const Camera& /*camera*/, uint8_t* cbData) override
    {
        if (!owner_)
        {
            return;
        }
        Material* material = obj.GetGraphicsMaterial();
        if (!material)
        {
            return;
        }

        // Per-object only. Per-frame/per-view fields are written once per pass into
        // the shared GlassView CB (b1) by SceneRenderer::Pass_Transparent.
        const float normalInfo = owner_->HasNormalMap() ? 1.0f : 0.0f;

        UpdateUniform(obj, cb_.world, material, obj.GetModelMatrix(), cbData);
        UpdateUniform(obj, cb_.prevWorld, material, obj.GetPreviousModelMatrix(), cbData);
        UpdateUniform(obj, cb_.absorptionThickness, material, float4(owner_->absorption_, owner_->thickness_), cbData);
        UpdateUniform(obj, cb_.tintRoughness, material, float4(owner_->tint_, owner_->roughness_), cbData);
        UpdateUniform(obj, cb_.matExtra, material,
            float4(owner_->ior_, owner_->reflectionStrength_, owner_->refractionDistortion_, normalInfo), cbData);
        UpdateUniform(obj, cb_.objectId, material, ToObjectId32(obj.GetEditorObjectId()), cbData);
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
        Material::CBFieldHandle prevWorld;
        Material::CBFieldHandle absorptionThickness;
        Material::CBFieldHandle tintRoughness;
        Material::CBFieldHandle matExtra;
        Material::CBFieldHandle objectId;
    } cb_{};

    struct ShadowHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle view;
        Material::CBFieldHandle proj;
    } shadowCb_{};

    Scene* scene_ = nullptr;
    TransparentStaticMesh* owner_ = nullptr;
};

TransparentStaticMesh::TransparentStaticMesh(Scene* scene,
    const std::string& modelName,
    const float3& position,
    const float3& scale,
    float rotationSpeedRad)
    : RenderableObject("PosNormTanUV", L"shaders/glass.hlsl")
    , scene_(scene)
    , modelName_(modelName)
    , rotationSpeed_(rotationSpeedRad)
{
    SetPosition(position);
    SetScale(scale);
    SetRotationEulerRad(float3(0.0f, 0.0f, 0.0f));
    SetRenderLayer(RenderLayer::Transparent);

    allowWireframe_ = false;

    SetUniformBinder(std::make_unique<TransparentUniformBinder>(scene_, this));
}

void TransparentStaticMesh::SetRecomputeNormalSlots(std::vector<uint32_t> slots)
{
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    recomputeNormalSlots_ = std::move(slots);
}

void TransparentStaticMesh::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);
    if (renderer && !modelName_.empty())
    {
        MeshLoadOptions options;
        options.generateTangentSpace = true;
        options.wantCW = false;
        options.recomputeNormalSlots = recomputeNormalSlots_;
        SetMesh(renderer->GetMeshManager()->Load(
            modelName_, renderer, uploadCmdList, uploadKeepAlive, options));
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
}

void TransparentStaticMesh::Tick(float deltaTime)
{
    float3 rotEuler = GetRotationEulerRad();
    rotEuler.y += rotationSpeed_ * deltaTime;
    if (rotEuler.y > XM_2PI)
    {
        rotEuler.y -= XM_2PI;
    }
    SetRotationEulerRad(rotEuler);
}

bool TransparentStaticMesh::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData)
{
    if (!renderer || !scene_)
    {
        return false;
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

    auto& lights = scene_->GetLightManager();
    const size_t pointCount = lights.PointLights().size();
    const size_t spotCount = lights.GetSpotLightCount();
    if (!lights.EnsurePointLightBuffer(renderer, std::max<size_t>(pointCount, size_t(1))) ||
        !lights.EnsureSpotLightBuffer(renderer, std::max<size_t>(spotCount, size_t(1))))
    {
        return false;
    }

    // Defensive: the deferred shadow SRVs staged below must be valid (see the
    // SceneRenderer Pass_SpotLights note); skip this draw for the frame if not.
    if (deferred.shadowSRV.ptr == 0 || deferred.spotShadowSRV.ptr == 0 ||
        deferred.pointShadowSRV.ptr == 0)
    {
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE sceneColorSrv =
        deferred.sceneOpaqueSRV.ptr != 0 ? deferred.sceneOpaqueSRV : deferred.sceneSRV;
    // t7 = the off-screen RT glass reflection (S15b). A normal texture on all HW — the glass
    // PS samples it only when rtEnabled (lightCounts.z in b1); fall back to scene color so the
    // slot is always a valid SRV (it's unread when RT is off / on non-RT HW).
    const D3D12_CPU_DESCRIPTOR_HANDLE glassReflSrv =
        deferred.glassReflectionSRV.ptr != 0 ? deferred.glassReflectionSRV : sceneColorSrv;

    // t9/t10 = VSM page table + pool (Step 21), set by Pass_Transparent (valid once a level is
    // loaded). Skip the draw if not set (its root sig binds them; a null staged SRV is invalid).
    if (renderer->GetVsmPageTableSrv().ptr == 0 || renderer->GetVsmPoolSrv().ptr == 0)
    {
        return false;
    }
    // P5 (t11): the GGX-prefiltered sky. Falls back to the display cube so the table is never
    // half-populated; the shader gates on camDirWS.w (the mip count), which is 0 without it.
    const D3D12_CPU_DESCRIPTOR_HANDLE skyDisplaySrv =
        sky ? sky->GetTex()->GetSRVCPU() : deferred.sceneSRV;
    const D3D12_CPU_DESCRIPTOR_HANDLE skySpecSrv =
        (sky && sky->HasIbl() && sky->GetSpecTex()->GetSRVCPU().ptr != 0)
            ? sky->GetSpecTex()->GetSRVCPU()
            : skyDisplaySrv;

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 12> srvs{
        sceneColorSrv,
        deferred.shadowSRV,
        deferred.spotShadowSRV,
        skyDisplaySrv,
        lights.GetPointLightSrv(renderer->GetCurrentFrameIndex()),
        lights.GetSpotLightSrv(renderer->GetCurrentFrameIndex()),
        normalSrv,
        glassReflSrv,
        deferred.pointShadowSRV,          // t8: omnidirectional point shadow cube (B3)
        renderer->GetVsmPageTableSrv(),   // t9: VSM page table
        renderer->GetVsmPoolSrv(),        // t10: VSM pool
        skySpecSrv                        // t11: P5 prefiltered sky
    };
    ctx.srvTable[0] = renderer->StageSrvUavTable(srvs).gpu;

    const auto samplerDescs = std::array{
        *SamplerManager::LinearClamp(),
        *SamplerManager::ComparisonLinearClamp(),
        *SamplerManager::LinearClamp()
    };
    ctx.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

    return RenderableObject::RecordGraphics(renderer, cl, ctx, camera, cbData);
}

void TransparentStaticMesh::SetNormalMap(const std::wstring& path, bool normalIsRG)
{
    normalMapPath_ = path;
    normalMapIsRG_ = normalIsRG;
    hasNormalMap_ = false;
}

void TransparentStaticMesh::ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const
{
    RenderableObject::ConfigureGraphicsPipeline(renderer, desc);

#if WITH_EDITOR
    desc.numRT = 3;
#else
    desc.numRT = 2;
#endif
    desc.rtvFormats[0] = renderer->GetSceneColorFormat();
    desc.rtvFormats[1] = renderer->GetGBufferVelocityFormat();
#if WITH_EDITOR
    desc.rtvFormats[2] = renderer->GetObjectIdFormat();
#endif
    desc.dsvFormat = renderer->GetDsvFormat();
    desc.depth.DepthEnable = TRUE;
    desc.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.depth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    desc.blend.IndependentBlendEnable = TRUE;
    desc.blend.RenderTarget[0].BlendEnable = TRUE;
    desc.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    desc.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.blend.RenderTarget[1].BlendEnable = FALSE;
    desc.blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
#if WITH_EDITOR
    desc.blend.RenderTarget[2].BlendEnable = FALSE;
    desc.blend.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
#endif
    desc.raster.CullMode = D3D12_CULL_MODE_BACK;

    auto& defs = desc.defines;
    defs.erase(std::remove_if(defs.begin(), defs.end(), [](const auto& def) { return def.first == "NORMALMAP_IS_RG"; }), defs.end());
    if (normalMapIsRG_)
    {
        defs.emplace_back("NORMALMAP_IS_RG", "1");
    }
#if WITH_EDITOR
    defs.emplace_back("EDITOR_OBJECT_ID", "1");
#endif
    // S15b: glass samples the precomputed GlassReflection texture (t7) — no RayQuery in this
    // shader, so the PSO is identical on RT and non-RT HW (one variant, 8-SRV table).
}
