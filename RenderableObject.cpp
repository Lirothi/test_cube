#include "RenderableObject.h"

#include <stdexcept>
#include <cstring>

#include "Renderer.h"
#include "Helpers.h"
#include "InputLayoutManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

RenderableObject::RenderableObject(
    const std::string& matPreset,
    const std::string& inputLayout,
    const std::wstring& graphicsShader):
    matPreset_(matPreset)
{
    // Дефолтный GraphicsDesc (треугольники, depth on, без бленда)
    graphicsDesc_.shaderFile = graphicsShader;
    graphicsDesc_.inputLayoutKey = inputLayout;
    graphicsDesc_.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphicsDesc_.numRT = 3;
    graphicsDesc_.rtvFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;      // GB0: Albedo+Metal
    graphicsDesc_.rtvFormats[1] = DXGI_FORMAT_R10G10B10A2_UNORM;   // GB1: NormalOcta+Rough
    graphicsDesc_.rtvFormats[2] = DXGI_FORMAT_R11G11B10_FLOAT;     // GB2: Emissive
    graphicsDesc_.dsvFormat = DXGI_FORMAT_D32_FLOAT;
    graphicsDesc_.FillDefaultsTriangle();

    mesh_.reset(new Mesh());
}

RenderableObject::~RenderableObject()
{
}

void RenderableObject::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!matData_)
    {
        matData_ = renderer->GetMaterialDataManager()->GetOrCreate(renderer, uploadCmdList, uploadKeepAlive, matPreset_);
        if (matData_)
        {
	        matData_->ConfigureDefinesForGBuffer(graphicsDesc_);
        }
    }

    graphicsMaterial_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, graphicsDesc_);
}

void RenderableObject::IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
    if (!GetMesh()) { return; }
    if (cl == nullptr) { return; }
    GetMesh()->Draw(cl);
}

void RenderableObject::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    // Установить графический материал
    graphicsMaterial_->Bind(cl, graphicsCtx_);
}

void RenderableObject::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& view, const mat4& proj)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }

    UINT cbSizeBytes = 0;

	if (cbLayout_) {
        cbSizeBytes = cbLayout_->GetSize();
    }

    if (cbSizeBytes == 0) {
        cbSizeBytes = graphicsMaterial_->GetCBSizeBytes(0);
    }
    // страховка: минимум 256 байт
    constexpr UINT kAlign = 256;
    if (cbSizeBytes == 0)
    {
	    cbSizeBytes = kAlign;
    }
    const UINT cbSizeAligned = (cbSizeBytes + (kAlign - 1)) & ~(kAlign - 1);

    // 2) выделить слайс в ринг-буфере кадра и прописать CBV
    auto alloc = renderer->GetFrameResource().AllocDynamic(cbSizeAligned, kAlign); // <- как просили
    cbvDataBegin_ = static_cast<uint8_t*>(alloc.cpu);
    graphicsCtx_.cbv[0] = alloc.gpu;

    RecordCompute(renderer, cl);
    UpdateUniforms(renderer, view, proj);
    PopulateContext(renderer, cl);
    RecordGraphics(renderer, cl);
    
    IssueDraw(renderer, cl);
}

void RenderableObject::ApplyMaterialParamsToCB()
{
    const auto& p = matParams_;
    UpdateUniform("baseColor", p.baseColor.xm());
    UpdateUniform("metalRough", p.metalRough.xm());
    UpdateUniform("texFlags", p.texFlags.xm());
}