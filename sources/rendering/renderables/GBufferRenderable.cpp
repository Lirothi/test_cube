#include "rendering/renderables/GBufferRenderable.h"

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

    void UpdateMainCB(RenderableObject& owner, Renderer* /*renderer*/, const mat4& view, const mat4& proj, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material) { return; }

        UpdateUniform(owner, cbHandles_.world, material, owner.GetModelMatrix(), cbData);
        UpdateUniform(owner, cbHandles_.view, material, view, cbData);
        UpdateUniform(owner, cbHandles_.proj, material, proj, cbData);

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
    auto& gd = GetGraphicsDesc();
    gd.numRT = 3;
    gd.rtvFormats[0] = Renderer::kGBuffer0Format;
    gd.rtvFormats[1] = Renderer::kGBuffer1Format;
    gd.rtvFormats[2] = Renderer::kGBuffer2Format;
    gd.dsvFormat = Renderer::kDeferredDepthFormat;

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
        if (matData_)
        {
            matData_->ConfigureDefinesForGBuffer(GetGraphicsDesc());
        }
    }

    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);
}

void GBufferRenderable::PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* /*cl*/, RenderContext& ctx)
{
    if (!renderer)
    {
        return;
    }

    if (matData_)
    {
        matData_->StageGBufferBindings(renderer, ctx, 0, 0);
    }
}
