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

    RebuildHandleCaches();
}

void RenderableObject::IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
    if (!GetMesh()) { return; }
    if (cl == nullptr) { return; }
    GetMesh()->Draw(cl);
}

void RenderableObject::UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj, uint8_t* cbData)
{
    if (!cbData) { return; }
    //CPU_SCOPE(L"RenderableObject::UpdateUniforms");
    UpdateUniform(cb0Handles_.world, graphicsMaterial_.get(), GetModelMatrix(), cbData);
    UpdateUniform(cb0Handles_.view, graphicsMaterial_.get(), view, cbData);
    UpdateUniform(cb0Handles_.proj, graphicsMaterial_.get(), proj, cbData);

    ApplyMaterialParamsToCB(cbData);
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
    //CPU_SCOPE(L"RenderableObject::Render");

    constexpr UINT kAlign = 256;
    const UINT cbSizeBytes = graphicsMaterial_->GetCBSizeBytesAligned(0, kAlign);
    
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

void RenderableObject::ApplyMaterialParamsToCB(uint8_t* cbData)
{
    const auto& p = matParams_;
    UpdateUniform(cb0Handles_.baseColor, graphicsMaterial_.get(), p.baseColor, cbData);
    UpdateUniform(cb0Handles_.metalRough, graphicsMaterial_.get(), p.metalRough, cbData);
    UpdateUniform(cb0Handles_.texOffsScale, graphicsMaterial_.get(), p.texOffsScale, cbData);
    UpdateUniform(cb0Handles_.texFlags, graphicsMaterial_.get(), p.texFlags, cbData);
}

std::wstring RenderableObject::AppendSuffixBeforeExt(const std::wstring& file,
    const std::wstring& suffix)
{
    auto pos = file.find_last_of(L'.');
    if (pos == std::wstring::npos) {
        return file + suffix;
    }
    return file.substr(0, pos) + suffix + file.substr(pos);
}

void RenderableObject::RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const mat4& lightView, const mat4& lightProj, RenderContext& ctx, uint8_t* cbData)
{
    if (!renderer || !cl || !GetMesh() || !shadowMaterial_) { return; }
    UpdateUniform(shadowHandles_.world, shadowMaterial_.get(), GetModelMatrix(), cbData);
    UpdateUniform(shadowHandles_.view, shadowMaterial_.get(), lightView, cbData);
    UpdateUniform(shadowHandles_.proj, shadowMaterial_.get(), lightProj, cbData);

    shadowMaterial_->Bind(cl, ctx, false);
}

void RenderableObject::OnMaterialHotReload(Renderer* /*renderer*/)
{
    RebuildHandleCaches();
}

void RenderableObject::RebuildHandleCaches()
{
    cb0Handles_ = {};
    shadowHandles_ = {};

    if (graphicsMaterial_)
    {
        cb0Handles_.world = graphicsMaterial_->ComputeCBFieldHandle(0, "world");
        cb0Handles_.view = graphicsMaterial_->ComputeCBFieldHandle(0, "view");
        cb0Handles_.proj = graphicsMaterial_->ComputeCBFieldHandle(0, "proj");
        cb0Handles_.baseColor = graphicsMaterial_->ComputeCBFieldHandle(0, "baseColor");
        cb0Handles_.metalRough = graphicsMaterial_->ComputeCBFieldHandle(0, "metalRough");
        cb0Handles_.texOffsScale = graphicsMaterial_->ComputeCBFieldHandle(0, "texOffsScale");
        cb0Handles_.texFlags = graphicsMaterial_->ComputeCBFieldHandle(0, "texFlags");
    }

    if (shadowMaterial_)
    {
        shadowHandles_.world = shadowMaterial_->ComputeCBFieldHandle(0, "world");
        shadowHandles_.view = shadowMaterial_->ComputeCBFieldHandle(0, "view");
        shadowHandles_.proj = shadowMaterial_->ComputeCBFieldHandle(0, "proj");
    }
}

void RenderableObject::RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const mat4& lightView, const mat4& lightProj)
{
    //CPU_SCOPE(L"RenderableObject::RenderShadow");
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