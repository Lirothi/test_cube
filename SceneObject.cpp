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

    // Создать upload CB под размер layout
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = cbLayout_->GetSize();
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&constantBuffer_)));

    D3D12_RANGE range = { 0, 0 };
    ThrowIfFailed(constantBuffer_->Map(0, &range, reinterpret_cast<void**>(&cbvDataBegin_)));

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
    if (constantBuffer_) {
        constantBuffer_->Unmap(0, nullptr);
    }
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
    graphicsCtx_.cbv[0] = constantBuffer_->GetGPUVirtualAddress();
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