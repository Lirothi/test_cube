#include "SceneObject.h"

#include <stdexcept>
#include <cstring>

#include "Renderer.h"
#include "Helpers.h"
#include "InputLayoutManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

SceneObject::SceneObject(Renderer* renderer,
    const std::string& matPreset,
    const std::string& cbLayout,
    const std::string& inputLayout,
    const std::wstring& graphicsShader):
    matPreset_(matPreset)
{
    if (!renderer) { throw std::runtime_error("SceneObject: renderer is null"); }

    cbLayout_ = renderer->GetCBManager().GetLayout(cbLayout);
    if (!cbLayout_) { throw std::runtime_error("SceneObject: ConstantBufferLayout not found"); }

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

SceneObject::~SceneObject()
{
}

void SceneObject::Init(Renderer* renderer,
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

    graphicsMaterial_ = renderer->GetMaterialManager().GetOrCreateGraphics(renderer, graphicsDesc_);
}

void SceneObject::IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
    if (!GetMesh()) { return; }
    if (cl == nullptr) { return; }
    GetMesh()->Draw(cl);
}

void SceneObject::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    // Установить графический материал
    graphicsMaterial_->Bind(cl, graphicsCtx_);
}

void SceneObject::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& view, const mat4& proj)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }

    constexpr UINT align = 256;
    const UINT rawSize = cbLayout_->GetSize();
    const UINT cbSize = (rawSize + (align - 1)) & ~(align - 1);

    auto alloc = renderer->GetFrameResource().AllocDynamic(cbSize, align);
    cbvDataBegin_ = static_cast<uint8_t*>(alloc.cpu);

    // 2) проставить b0 в RenderContext
    graphicsCtx_.cbv[0] = alloc.gpu;

    RecordCompute(renderer, cl);
    UpdateUniforms(renderer, view, proj);
    PopulateContext(renderer, cl);
    RecordGraphics(renderer, cl);
    
    IssueDraw(renderer, cl);
}

void SceneObject::ApplyMaterialParamsToCB()
{
    const auto& p = matParams_;
    UpdateUniform("baseColor", p.baseColor.xm());
    UpdateUniform("metalRough", p.metalRough.xm());
    UpdateUniform("texFlags", p.texFlags.xm());
}