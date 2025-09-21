#include "Skybox.h"
#include "Renderer.h"
#include "UploadManager.h"
#include "FrameResource.h"

using Microsoft::WRL::ComPtr;

void Skybox::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!renderer) {
        return;
    }

    // Если текстура ещё не загружена через LoadDDS — можно попытаться здесь (необязательно)
    if (!cube_.GetResource() && !path_.empty()) {
        (void)cube_.CreateFromDDS(renderer, uploadCmdList, path_, uploadKeepAlive);
    }

    graphicsDesc_.numRT = 1;
    graphicsDesc_.rtvFormats[0] = renderer->GetSceneColorFormat();
    graphicsDesc_.depth.DepthEnable = TRUE;
    graphicsDesc_.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;      // не пишем глубину
    graphicsDesc_.depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // трюк для неба
    graphicsDesc_.raster.CullMode = D3D12_CULL_MODE_NONE;             // рисуем "изнутри"
    graphicsDesc_.blend.RenderTarget[0].BlendEnable = FALSE;

    // Сборка геометрии (куб)
    BuildCubeMesh_(renderer, uploadCmdList, uploadKeepAlive);

    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);

    if (auto* material = GetGraphicsMaterial())
    {
        viewHandle_ = material->ComputeCBFieldHandle(0, "view");
        projHandle_ = material->ComputeCBFieldHandle(0, "proj");
        exposureHandle_ = material->ComputeCBFieldHandle(0, "exposure");
    }
}

void Skybox::OnMaterialHotReload(Renderer* renderer)
{
    RenderableObject::OnMaterialHotReload(renderer);
    if (auto* material = GetGraphicsMaterial())
    {
        viewHandle_ = material->ComputeCBFieldHandle(0, "view");
        projHandle_ = material->ComputeCBFieldHandle(0, "proj");
        exposureHandle_ = material->ComputeCBFieldHandle(0, "exposure");
    }
}

void Skybox::UpdateUniforms(Renderer* /*renderer*/, const mat4& view, const mat4& proj, uint8_t* cbData)
{
    // CB0: ожидаем идентификаторы "view" и "proj" в cbuffer'е (см. skybox.hlsl)
    UpdateGraphicsUniform(viewHandle_, view, cbData);
    UpdateGraphicsUniform(projHandle_, proj, cbData);
    UpdateGraphicsUniform(exposureHandle_, exposure_, cbData);
}

void Skybox::PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx)
{
    ctx.table[0] = cube_.GetSRVForFrame(renderer);
    ctx.samplerTable[0] = renderer->GetSamplerManager()->Get(renderer, SamplerManager::LinearClamp());
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