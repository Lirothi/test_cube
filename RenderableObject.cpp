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

    if (CastsShadow())
    {
        shadowDesc_ = graphicsDesc_;
        shadowDesc_.shaderFile = AppendSuffixBeforeExt(graphicsDesc_.shaderFile, L"_csm");
        shadowDesc_.inputLayoutKey = graphicsDesc_.inputLayoutKey;
        shadowDesc_.numRT = 0;
        shadowDesc_.dsvFormat = DXGI_FORMAT_D16_UNORM;
        shadowDesc_.depth.DepthEnable = TRUE;
        shadowDesc_.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        shadowDesc_.raster.CullMode = D3D12_CULL_MODE_BACK;
    }

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
    if (CastsShadow())
    {
        shadowMaterial_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, shadowDesc_);
    }
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
    graphicsMaterial_->Bind(cl, graphicsCtx_, renderer->GetWireframeMode() && allowWireframe_);
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
    auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSizeAligned, kAlign); // <- как просили
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
    UpdateUniform("texOffsScale", p.texOffsScale.xm());
    UpdateUniform("texFlags", p.texFlags.xm());
}

std::wstring RenderableObject::AppendSuffixBeforeExt(const std::wstring& file,
    const std::wstring& suffix)
{
    auto pos = file.find_last_of(L'.');
    if (pos == std::wstring::npos) return file + suffix;
    return file.substr(0, pos) + suffix + file.substr(pos);
}

void RenderableObject::RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const mat4& lightView, const mat4& lightProj)
{
    if (!renderer || !cl || !GetMesh() || !shadowMaterial_) { return; }

    // per-object CB для shadowMaterial (b0)
    UINT cbSize = shadowMaterial_->GetCBSizeBytesAligned(0, 256);

    auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSize, 256);
    uint8_t* cb = static_cast<uint8_t*>(alloc.cpu);

    // world/view/proj — именами, как ожидает _csm шейдер
    shadowMaterial_->UpdateCB0Field("world", GetModelMatrix(), cb);
    shadowMaterial_->UpdateCB0Field("view", lightView, cb);
    shadowMaterial_->UpdateCB0Field("proj", lightProj, cb);

    shadowCtx_.cbv[0] = alloc.gpu;
    shadowMaterial_->Bind(cl, shadowCtx_, false);
}

void RenderableObject::RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const mat4& lightView, const mat4& lightProj)
{
    if (!CastsShadow())
    {
        return;
    }

    RecordShadow(renderer, cl, lightView, lightProj);
    IssueDraw(renderer, cl);
}