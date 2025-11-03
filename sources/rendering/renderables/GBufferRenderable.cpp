#include "rendering/renderables/GBufferRenderable.h"

#include "app/Camera.h"
#include "rendering/core/Renderer.h"
#include "materials/MaterialDataManager.h"

namespace
{
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
            cbHandles_.world = material->ComputeCBFieldHandle(0, "world");
            cbHandles_.view = material->ComputeCBFieldHandle(0, "view");
            cbHandles_.proj = material->ComputeCBFieldHandle(0, "proj");
            cbHandles_.baseColor = material->ComputeCBFieldHandle(0, "baseColor");
            cbHandles_.metalRough = material->ComputeCBFieldHandle(0, "metalRough");
            cbHandles_.texOffsScale = material->ComputeCBFieldHandle(0, "texOffsScale");
            cbHandles_.texFlags = material->ComputeCBFieldHandle(0, "texFlags");
        }

        if (Material* shadowMaterial = owner.GetShadowMaterial())
        {
            shadowHandles_.world = shadowMaterial->ComputeCBFieldHandle(0, "world");
            shadowHandles_.view = shadowMaterial->ComputeCBFieldHandle(0, "view");
            shadowHandles_.proj = shadowMaterial->ComputeCBFieldHandle(0, "proj");
        }
    }

    void UpdateMainCB(RenderableObject& owner, Renderer* /*renderer*/, const Camera& camera, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material) { return; }

        UpdateUniform(owner, cbHandles_.world, material, owner.GetModelMatrix(), cbData);
        UpdateUniform(owner, cbHandles_.view, material, camera.GetViewMatrix(), cbData);
        UpdateUniform(owner, cbHandles_.proj, material, camera.GetProjMatrix(), cbData);

        const auto& p = params_;
        UpdateUniform(owner, cbHandles_.baseColor, material, p.baseColor, cbData);
        UpdateUniform(owner, cbHandles_.metalRough, material, p.metalRough, cbData);
        UpdateUniform(owner, cbHandles_.texOffsScale, material, p.texOffsScale, cbData);
        UpdateUniform(owner, cbHandles_.texFlags, material, p.texFlags, cbData);
    }

    void UpdateShadowCB(RenderableObject& owner, Renderer* /*renderer*/, const mat4& lightView, const mat4& lightProj, uint8_t* cbData) override
    {
        Material* material = owner.GetShadowMaterial();
        if (!material) { return; }

        UpdateUniform(owner, shadowHandles_.world, material, owner.GetModelMatrix(), cbData);
        UpdateUniform(owner, shadowHandles_.view, material, lightView, cbData);
        UpdateUniform(owner, shadowHandles_.proj, material, lightProj, cbData);
    }

private:
    struct CBHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle view;
        Material::CBFieldHandle proj;
        Material::CBFieldHandle baseColor;
        Material::CBFieldHandle metalRough;
        Material::CBFieldHandle texOffsScale;
        Material::CBFieldHandle texFlags;
    } cbHandles_{};

    struct ShadowCBHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle view;
        Material::CBFieldHandle proj;
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
        matData_ = renderer->GetMaterialDataManager()->GetOrCreate(renderer, uploadCmdList, uploadKeepAlive, matPreset_);
    }

    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);
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

    desc.numRT = 3;
    desc.rtvFormats[0] = Renderer::kGBuffer0Format;
    desc.rtvFormats[1] = Renderer::kGBuffer1Format;
    desc.rtvFormats[2] = Renderer::kGBuffer2Format;
    desc.dsvFormat = Renderer::kDeferredDepthFormat;

    if (matData_)
    {
        matData_->ConfigureDefinesForGBuffer(desc);
    }
}
