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
    graphicsDesc_.dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
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

void RenderableObject::PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx)
{
	if (matData_)
	{
        matData_->StageGBufferBindings(renderer, ctx, 0, 0);
	}
}

void RenderableObject::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    // Установить графический материал
    graphicsMaterial_->Bind(cl, ctx, renderer->GetWireframeMode() && allowWireframe_);
}

void RenderableObject::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& view, const mat4& proj)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    CPU_SCOPE("RenderableObject::Render");

    UINT cbSizeBytes = 0;

	if (cbLayout_) {
        cbSizeBytes = cbLayout_->GetSize();
    }

    constexpr UINT kAlign = 256;
    if (cbSizeBytes == 0) {
        cbSizeBytes = graphicsMaterial_->GetCBSizeBytesAligned(0, kAlign);
    }
    
    // 2) выделить слайс в ринг-буфере кадра и прописать CBV
    auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSizeBytes, kAlign); // <- как просили
    uint8_t* cbData = static_cast<uint8_t*>(alloc.cpu);
    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();
    ctx.cbv[0] = alloc.gpu;

    RecordCompute(renderer, cl);
    UpdateUniforms(renderer, view, proj, cbData);
    PopulateContext(renderer, cl, ctx);
    RecordGraphics(renderer, cl, ctx);
    
    IssueDraw(renderer, cl);
}

namespace {
constexpr CBFieldID kBaseColorID   = CB_FIELD_ID("baseColor");
constexpr CBFieldID kMetalRoughID  = CB_FIELD_ID("metalRough");
constexpr CBFieldID kTexOffsScaleID = CB_FIELD_ID("texOffsScale");
constexpr CBFieldID kTexFlagsID    = CB_FIELD_ID("texFlags");
constexpr CBFieldID kWorldID       = CB_FIELD_ID("world");
constexpr CBFieldID kViewID        = CB_FIELD_ID("view");
constexpr CBFieldID kProjID        = CB_FIELD_ID("proj");
}

void RenderableObject::ApplyMaterialParamsToCB(uint8_t* cbData)
{
    const auto& p = matParams_;
    UpdateUniform(kBaseColorID, p.baseColor, cbData);
    UpdateUniform(kMetalRoughID, p.metalRough, cbData);
    UpdateUniform(kTexOffsScaleID, p.texOffsScale, cbData);
    UpdateUniform(kTexFlagsID, p.texFlags, cbData);
}

std::wstring RenderableObject::AppendSuffixBeforeExt(const std::wstring& file,
    const std::wstring& suffix)
{
    auto pos = file.find_last_of(L'.');
    if (pos == std::wstring::npos) return file + suffix;
    return file.substr(0, pos) + suffix + file.substr(pos);
}

void RenderableObject::RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const mat4& lightView, const mat4& lightProj, RenderContext& ctx, uint8_t* cbData)
{
    if (!renderer || !cl || !GetMesh() || !shadowMaterial_) { return; }

    // world/view/proj — хешированные идентификаторы
    shadowMaterial_->UpdateCB0Field(kWorldID, GetModelMatrix(), cbData);
    shadowMaterial_->UpdateCB0Field(kViewID, lightView, cbData);
    shadowMaterial_->UpdateCB0Field(kProjID, lightProj, cbData);

    shadowMaterial_->Bind(cl, ctx, false);
}

void RenderableObject::RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const mat4& lightView, const mat4& lightProj)
{
    CPU_SCOPE("RenderableObject::RenderShadow");
    if (!CastsShadow())
    {
        return;
    }

    UINT cbSize = shadowMaterial_->GetCBSizeBytesAligned(0, 256);
    auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSize, 256);
    uint8_t* cbData = static_cast<uint8_t*>(alloc.cpu);
    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();

    ctx.cbv[0] = alloc.gpu;

    RecordShadow(renderer, cl, lightView, lightProj, ctx, cbData);
    IssueDraw(renderer, cl);
}