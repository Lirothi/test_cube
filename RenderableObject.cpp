#include "RenderableObject.h"

#include <stdexcept>
#include <cstring>

#include "Renderer.h"
#include "Helpers.h"
#include "InputLayoutManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

RenderableObject::RenderableObject(
    const std::string& inputLayout,
    const std::wstring& graphicsShader)
{
    graphicsDesc_.shaderFile = graphicsShader;
    graphicsDesc_.inputLayoutKey = inputLayout;
    graphicsDesc_.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphicsDesc_.numRT = 0;
    graphicsDesc_.dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    graphicsDesc_.FillDefaultsTriangle();

    mesh_.reset(new Mesh());
    modelMatrix_ = mat4::Identity();
}

RenderableObject::~RenderableObject() = default;

void RenderableObject::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    (void)uploadCmdList;
    (void)uploadKeepAlive;

    if (!renderer)
    {
        return;
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
    else
    {
        shadowMaterial_.reset();
    }

    if (uniformBinder_)
    {
        uniformBinder_->RebuildHandles(*this);
    }
}

void RenderableObject::IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
    if (!GetMesh()) { return; }
    if (cl == nullptr) { return; }
    GetMesh()->Draw(cl);
}

void RenderableObject::PopulateContext(Renderer* /*renderer*/, ID3D12GraphicsCommandList* /*cl*/, RenderContext& /*ctx*/)
{
}

void RenderableObject::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    if (!graphicsMaterial_) { return; }
    graphicsMaterial_->Bind(cl, ctx, renderer->GetWireframeMode() && allowWireframe_);
}

void RenderableObject::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& view, const mat4& proj)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    if (!graphicsMaterial_) { return; }

    constexpr UINT kAlign = 256;
    const UINT cbSizeBytes = graphicsMaterial_->GetCBSizeBytesAligned(0, kAlign);

    auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSizeBytes, kAlign);
    uint8_t* cbData = static_cast<uint8_t*>(alloc.cpu);
    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();
    ctx.cbv[0] = alloc.gpu;

    RecordCompute(renderer, cl);
    if (uniformBinder_)
    {
        uniformBinder_->UpdateMainCB(*this, renderer, view, proj, cbData);
    }
    PopulateContext(renderer, cl, ctx);
    RecordGraphics(renderer, cl, ctx);

    IssueDraw(renderer, cl);
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
    const mat4& lightView, const mat4& lightProj, RenderContext& ctx)
{
    (void)lightView;
    (void)lightProj;

    if (!renderer || !cl || !GetMesh() || !shadowMaterial_) { return; }
    shadowMaterial_->Bind(cl, ctx, false);
}

void RenderableObject::OnMaterialHotReload(Renderer* /*renderer*/)
{
    if (uniformBinder_)
    {
        uniformBinder_->RebuildHandles(*this);
    }
}

void RenderableObject::RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const mat4& lightView, const mat4& lightProj)
{
    if (!renderer || !cl || !shadowMaterial_)
    {
        return;
    }

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

    if (uniformBinder_)
    {
        uniformBinder_->UpdateShadowCB(*this, renderer, lightView, lightProj, cbData);
    }

    RecordShadow(renderer, cl, lightView, lightProj, ctx);
    IssueDraw(renderer, cl);
}

void RenderableObject::SetGraphicsMaterial(Material* m)
{
    graphicsMaterial_.reset(m);
    if (uniformBinder_)
    {
        uniformBinder_->RebuildHandles(*this);
    }
}

void RenderableObject::SetUniformBinder(std::unique_ptr<UniformBinder> binder)
{
    uniformBinder_ = std::move(binder);
    if (uniformBinder_ && graphicsMaterial_)
    {
        uniformBinder_->RebuildHandles(*this);
    }
}
