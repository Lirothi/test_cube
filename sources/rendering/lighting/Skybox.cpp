#include "rendering/lighting/Skybox.h"
#include "rendering/core/Renderer.h"
#include "materials/UploadManager.h"
#include "rendering/core/FrameResource.h"
#include "app/camera/Camera.h"

#include <memory>

using Microsoft::WRL::ComPtr;

namespace
{
class SkyboxUniformBinder final : public RenderableObject::UniformBinder
{
public:
    explicit SkyboxUniformBinder(Skybox& owner) : owner_(owner) {}

    void RebuildHandles(RenderableObject& owner) override
    {
        viewHandle_ = {};
        projHandle_ = {};
        prevViewHandle_ = {};
        prevProjHandle_ = {};
        projNoJitterHandle_ = {};
        prevProjNoJitterHandle_ = {};
        exposureHandle_ = {};

        if (Material* material = owner.GetGraphicsMaterial())
        {
            viewHandle_ = material->ComputeCBFieldHandle(0, "view");
            projHandle_ = material->ComputeCBFieldHandle(0, "proj");
            prevViewHandle_ = material->ComputeCBFieldHandle(0, "prevView");
            prevProjHandle_ = material->ComputeCBFieldHandle(0, "prevProj");
            projNoJitterHandle_ = material->ComputeCBFieldHandle(0, "projNoJitter");
            prevProjNoJitterHandle_ = material->ComputeCBFieldHandle(0, "prevProjNoJitter");
            exposureHandle_ = material->ComputeCBFieldHandle(0, "exposure");
        }
    }

    void UpdateMainCB(RenderableObject& owner, Renderer* /*renderer*/, const Camera& camera, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material) { return; }

        UpdateUniform(owner, viewHandle_, material, camera.GetViewMatrix(), cbData);
        UpdateUniform(owner, projHandle_, material, camera.GetProjMatrix(), cbData);
        UpdateUniform(owner, prevViewHandle_, material, camera.GetPrevViewMatrix(), cbData);
        UpdateUniform(owner, prevProjHandle_, material, camera.GetPrevProjMatrix(), cbData);
        UpdateUniform(owner, projNoJitterHandle_, material, camera.GetProjMatrixNoJitter(), cbData);
        UpdateUniform(owner, prevProjNoJitterHandle_, material, camera.GetPrevProjMatrixNoJitter(), cbData);
        UpdateUniform(owner, exposureHandle_, material, owner_.GetExposure(), cbData);
    }

private:
    Skybox& owner_;
    Material::CBFieldHandle viewHandle_{};
    Material::CBFieldHandle projHandle_{};
    Material::CBFieldHandle prevViewHandle_{};
    Material::CBFieldHandle prevProjHandle_{};
    Material::CBFieldHandle projNoJitterHandle_{};
    Material::CBFieldHandle prevProjNoJitterHandle_{};
    Material::CBFieldHandle exposureHandle_{};
};
} // namespace

void Skybox::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!renderer) {
        return;
    }

    // If the texture has not been loaded via LoadDDS yet, we can optionally try it here
    if (!cube_.GetResource() && !path_.empty()) {
        (void)cube_.CreateFromDDS(renderer, uploadCmdList, path_, uploadKeepAlive);
    }

    // Build the cube geometry
    BuildCubeMesh_(renderer, uploadCmdList, uploadKeepAlive);

    if (!GetUniformBinder())
    {
        SetUniformBinder(std::make_unique<SkyboxUniformBinder>(*this));
    }

    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);
}

void Skybox::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData)
{
    ctx.table[0] = cube_.GetSRVForFrame(renderer);
    ctx.samplerTable[0] = renderer->GetSamplerManager()->Get(renderer, *SamplerManager::LinearClamp());

    RenderableObject::RecordGraphics(renderer, cl, ctx, camera, cbData);
}

void Skybox::BuildCubeMesh_(Renderer* r,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    std::vector<VertexPNTUV> cubeVerts;
    std::vector<uint32_t> cubeIndices;
    BuildCubeCW(cubeVerts, cubeIndices);

    GetMesh()->CreateGPU_PNTUV(r->GetDevice(), uploadCmdList, keepAlive, cubeVerts, cubeIndices.data(), (UINT)cubeIndices.size(), true);
}

void Skybox::ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const
{
    RenderableObject::ConfigureGraphicsPipeline(renderer, desc);

    desc.numRT = 2;
    if (renderer)
    {
        desc.rtvFormats[0] = renderer->GetLightTargetFormat();
        desc.rtvFormats[1] = Renderer::kGBufferVelocityFormat;
    }
    desc.depth.DepthEnable = TRUE;
    desc.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;      // do not write depth
    desc.depth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; // reverse-Z depth test for the sky
    desc.raster.CullMode = D3D12_CULL_MODE_NONE;             // render from the inside
    desc.blend.RenderTarget[0].BlendEnable = FALSE;
    desc.blend.RenderTarget[1].BlendEnable = FALSE;
    desc.blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
}
