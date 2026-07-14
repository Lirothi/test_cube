#include "rendering/renderables/GBufferRenderable.h"

#include <cstdint>

#include "app/camera/Camera.h"
#include "rendering/core/Renderer.h"
#include "materials/MaterialDataManager.h"

namespace
{
uint32_t ToObjectId32(std::uint64_t id)
{
    return id > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(id);
}

class GBufferUniformBinder final : public RenderableObject::UniformBinder
{
public:
    explicit GBufferUniformBinder(MaterialParams& params) : params_(params) {}

    void RebuildHandles(RenderableObject& owner) override
    {
        cbHandles_ = {};
        shadowHandles_ = {};

        if (Material* material = owner.GetGraphicsMaterial())
        {
            // Per-object only (b0). The view matrices now live in the shared
            // per-view CB (b1), filled once per pass by the renderer.
            cbHandles_.world = material->ComputeCBFieldHandle(0, "world");
            cbHandles_.prevWorld = material->ComputeCBFieldHandle(0, "prevWorld");
            cbHandles_.baseColor = material->ComputeCBFieldHandle(0, "baseColor");
            cbHandles_.metalRough = material->ComputeCBFieldHandle(0, "metalRough");
            cbHandles_.texOffsScale = material->ComputeCBFieldHandle(0, "texOffsScale");
            cbHandles_.texFlags = material->ComputeCBFieldHandle(0, "texFlags");
            cbHandles_.objectId = material->ComputeCBFieldHandle(0, "objectId");
        }

        if (Material* shadowMaterial = owner.GetShadowMaterial())
        {
            // viewProj (light) now comes from the shared per-view CB (b1).
            shadowHandles_.world = shadowMaterial->ComputeCBFieldHandle(0, "world");
        }
    }

    void UpdateMainCB(RenderableObject& owner, Renderer* /*renderer*/, const Camera& /*camera*/, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material) { return; }

        UpdateUniform(owner, cbHandles_.world, material, owner.GetModelMatrix(), cbData);
        UpdateUniform(owner, cbHandles_.prevWorld, material, owner.GetPreviousModelMatrix(), cbData);

        const auto& p = params_;
        UpdateUniform(owner, cbHandles_.baseColor, material, p.baseColor, cbData);
        UpdateUniform(owner, cbHandles_.metalRough, material, p.metalRough, cbData);
        UpdateUniform(owner, cbHandles_.texOffsScale, material, p.texOffsScale, cbData);
        UpdateUniform(owner, cbHandles_.texFlags, material, p.texFlags, cbData);
        UpdateUniform(owner, cbHandles_.objectId, material, ToObjectId32(owner.GetEditorObjectId()), cbData);
    }

    void UpdateShadowCB(RenderableObject& owner, Renderer* /*renderer*/, const mat4& /*lightView*/, const mat4& /*lightProj*/, uint8_t* cbData) override
    {
        Material* material = owner.GetShadowMaterial();
        if (!material) { return; }

        // viewProj (light) is written once per cascade into the shared per-view CB (b1).
        UpdateUniform(owner, shadowHandles_.world, material, owner.GetModelMatrix(), cbData);
    }

private:
    struct CBHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle prevWorld;
        Material::CBFieldHandle baseColor;
        Material::CBFieldHandle metalRough;
        Material::CBFieldHandle texOffsScale;
        Material::CBFieldHandle texFlags;
        Material::CBFieldHandle objectId;
    } cbHandles_{};

    struct ShadowCBHandles
    {
        Material::CBFieldHandle world;
    } shadowHandles_{};

    MaterialParams& params_;
};
} // namespace

GBufferRenderable::GBufferRenderable(const std::string& matPreset,
    const std::string& inputLayout,
    const std::wstring& graphicsShader)
    : RenderableObject(inputLayout, graphicsShader)
    , matPreset_(matPreset)
{
    SetUniformBinder(std::make_unique<GBufferUniformBinder>(matParams_));
}

void GBufferRenderable::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!renderer)
    {
        return;
    }

    if (!matData_)
    {
        const std::string gltfSrc = GetGltfMaterialSourcePath();
        if (!gltfSrc.empty())
        {
            matData_ = renderer->GetMaterialDataManager()->GetOrCreateFromGltf(renderer, uploadCmdList, uploadKeepAlive, gltfSrc);
            // Seed per-object params from the glTF material defaults. NOTE: this runs after the
            // factory's ApplyStaticMeshJsonProperties, so explicit JSON param overrides on a
            // glTF-"auto" object are currently clobbered (acceptable for A3; the editor path in
            // B4 will layer overrides on top).
            if (matData_ && matData_->fromGltf)
            {
                matParams_ = matData_->gltfDefaultParams;
            }
        }
        if (!matData_)
        {
            matData_ = renderer->GetMaterialDataManager()->GetOrCreate(renderer, uploadCmdList, uploadKeepAlive, matPreset_);
        }
    }

    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);

    BuildInstancedMaterials(renderer);
}

void GBufferRenderable::BuildInstancedMaterials(Renderer* renderer)
{
    // Step 4: only the default gbuffer shader has cbuffer-array instanced counterparts
    // (gbuffer_instcb.hlsl + gbuffer_instcb_csm.hlsl). Build them with the SAME pipeline
    // config + material defines as the per-object materials so instanced draws match
    // pixel-for-pixel. MaterialManager caches by desc, so all objects of one material
    // share a single instanced PSO. Both gbuffer + shadow variants are required; if either
    // fails to compile we disable instancing for this object (no half-instanced state).
    if (!renderer) { return; }
    if (GetGraphicsShaderPath() != L"shaders/gbuffer.hlsl") { return; }

    Material::GraphicsDesc gd = BuildGraphicsDesc(renderer);
    gd.shaderFile = L"shaders/gbuffer_instcb.hlsl";
    auto gfx = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    if (!gfx || !gfx->GetPipelineState()) { return; }

    std::shared_ptr<Material> shadow;
    if (CastsShadow())
    {
        Material::GraphicsDesc sd = BuildShadowDesc(renderer, gd); // -> gbuffer_instcb_csm.hlsl
        shadow = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, sd);
        if (!shadow || !shadow->GetPipelineState()) { return; }
    }

    instancedGraphicsMaterial_ = std::move(gfx);
    instancedShadowMaterial_ = std::move(shadow);
}

void GBufferRenderable::FillInstanceData(render::InstancePerObject& out) const
{
    out.world = GetModelMatrix().m;
    out.prevWorld = GetPreviousModelMatrix().m;

    const MaterialParams& p = matParams_;
    out.baseColor = DirectX::XMFLOAT4(p.baseColor.x, p.baseColor.y, p.baseColor.z, p.baseColor.w);
    out.metalRough = DirectX::XMFLOAT2(p.metalRough.x, p.metalRough.y);
    out._pad0[0] = 0.0f;
    out._pad0[1] = 0.0f;
    out.texOffsScale = DirectX::XMFLOAT4(p.texOffsScale.x, p.texOffsScale.y, p.texOffsScale.z, p.texOffsScale.w);
    out.texFlags = DirectX::XMFLOAT4(p.texFlags.x, p.texFlags.y, p.texFlags.z, p.texFlags.w);
    out.objectId = ToObjectId32(GetEditorObjectId());
    out._pad1[0] = 0;
    out._pad1[1] = 0;
    out._pad1[2] = 0;
}

void GBufferRenderable::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData)
{
    if (!renderer)
    {
        return;
    }

    if (matData_)
    {
        matData_->StageGBufferBindings(renderer, ctx, 0, 0);
    }

    RenderableObject::RecordGraphics(renderer, cl, ctx, camera, cbData);
}

void GBufferRenderable::ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const
{
    RenderableObject::ConfigureGraphicsPipeline(renderer, desc);

    desc.numRT = 5;
    if (renderer)
    {
        desc.rtvFormats[0] = renderer->GetGBuffer0Format();
        desc.rtvFormats[1] = renderer->GetGBuffer1Format();
        desc.rtvFormats[2] = renderer->GetGBuffer2Format();
        desc.rtvFormats[3] = renderer->GetGBufferVelocityFormat();
        desc.rtvFormats[4] = renderer->GetObjectIdFormat();
        desc.dsvFormat = renderer->GetDeferredDepthFormat();
    }

    if (matData_)
    {
        matData_->ConfigureDefinesForGBuffer(desc);
    }

    desc.depth.StencilEnable = TRUE;
    desc.depth.StencilReadMask = 0x80;
    desc.depth.StencilWriteMask = 0x80;
    desc.depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.depth.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    desc.depth.BackFace = desc.depth.FrontFace;
}
