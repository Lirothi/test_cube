#include "DebugGrid.h"
#include "Renderer.h"
#include "SamplerManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static inline XMFLOAT4 RGBA(float r, float g, float b, float a) { return XMFLOAT4(r, g, b, a); }

DebugGrid::DebugGrid(Renderer* renderer,
    float halfSize, float step,
    float axisLen, float yPlane,
    float gridAlpha, float axisAlpha,
    float axisThicknessPx)
    : SceneObject(renderer, /*cbLayout*/"MVP_Axis", /*inputLayout*/"PosColor", /*shader*/L"lines.hlsl")
    , halfSize_(halfSize), step_(step), axisLen_(axisLen)
    , yPlane_(yPlane), gridAlpha_(gridAlpha), axisAlpha_(axisAlpha)
    , axisThicknessPx_(axisThicknessPx)
{
    // материал базового объекта — для сетки (LINELIST)
    auto& gd = GetGraphicsDesc();
    gd.numRT = 1;
    gd.rtvFormat = renderer->GetSceneColorFormat();
    gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    gd.raster.CullMode = D3D12_CULL_MODE_NONE;
    //gd.raster.DepthClipEnable = false;
    gd.raster.DepthBias = 0;
    gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    gd.blend.RenderTarget[0].BlendEnable = TRUE;
    gd.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    gd.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    gd.blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    gd.blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    gd.blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    gd.blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
}

void DebugGrid::BuildGridCPU(std::vector<LineVertex>& out)
{
    out.clear();
    if (step_ <= 0.0f) { step_ = 1.0f; }

    const float hs = (halfSize_ > 0.0f) ? halfSize_ : 10.0f;
    const int   n = int(std::floor(hs / step_));

    const XMFLOAT4 gridCol = RGBA(1, 1, 1, gridAlpha_);

    for (int i = -n; i <= n; ++i) {
        const float z = i * step_;
        {
            LineVertex a{ XMFLOAT3(-hs, yPlane_, z), gridCol };
            LineVertex b{ XMFLOAT3(+hs, yPlane_, z), gridCol };
            out.push_back(a); out.push_back(b);
        }
        const float x = i * step_;
        {
            LineVertex a{ XMFLOAT3(x, yPlane_, -hs), gridCol };
            LineVertex b{ XMFLOAT3(x, yPlane_, +hs), gridCol };
            out.push_back(a); out.push_back(b);
        }
    }
}

void DebugGrid::BuildAxesScreenCPU(std::vector<AxisVertex>& out)
{
    out.clear();
    const float L = axisLen_;
    const XMFLOAT4 xCol = RGBA(1, 0, 0, axisAlpha_);
    const XMFLOAT4 yCol = RGBA(0, 1, 0, axisAlpha_);
    const XMFLOAT4 zCol = RGBA(0, 0, 1, axisAlpha_);

    auto pushList = [&](const XMFLOAT3& A, const XMFLOAT3& B, const XMFLOAT4& C) {
        const float eps = 2.25f;
        //// Первый треугольник: A-left, B-left, B-right
        //out.push_back({ A, B, XMFLOAT2(-1,-1), C });
        //out.push_back({ A, B, XMFLOAT2(-1,+1), C });
        //out.push_back({ A, B, XMFLOAT2(+1,+1), C });
        //// Второй треугольник: A-left, B-right, A-right
        //out.push_back({ A, B, XMFLOAT2(-1,-1), C });
        //out.push_back({ A, B, XMFLOAT2(+1,+1), C });
        //out.push_back({ A, B, XMFLOAT2(+1,-1), C });

        out.push_back({ A, B, XMFLOAT3(-1,-1, +eps), C });  // AL
        out.push_back({ A, B, XMFLOAT3(-1,+1, +eps), C });  // BL (shared +)
        out.push_back({ A, B, XMFLOAT3(+1,+1, +eps), C });  // BR (shared +)

        // T2: AR, BR(-eps), BL(-eps)
        out.push_back({ A, B, XMFLOAT3(-1,-1, -eps), C });  // AR
        out.push_back({ A, B, XMFLOAT3(+1,+1, -eps), C });  // BR (shared -)
        out.push_back({ A, B, XMFLOAT3(+1,-1, -eps), C });  // BL (shared -)
        };

    // X, Z, Y
    pushList(XMFLOAT3(0.0, yPlane_, 0.0f), XMFLOAT3(L, yPlane_, 0.0f), xCol);
    pushList(XMFLOAT3(0.0f, yPlane_, 0.0), XMFLOAT3(0.0f, yPlane_, L), zCol);
    pushList(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, L, 0.0f), yCol);
}

void DebugGrid::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    // базовый материал (сеточный)
    SceneObject::Init(renderer, uploadCmdList, uploadKeepAlive);

    UploadManager um(renderer->GetDevice(), uploadCmdList);

    // --- VB сетки ---
    std::vector<LineVertex> gridCpu;
    BuildGridCPU(gridCpu);
    gridVertexCount_ = (UINT)gridCpu.size();
    {
        ComPtr<ID3D12Resource> vb = um.CreateBufferWithData(
            gridCpu.data(), (UINT)gridCpu.size() * sizeof(LineVertex),
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        vbGrid_ = vb;
        vbvGrid_.BufferLocation = vbGrid_->GetGPUVirtualAddress();
        vbvGrid_.StrideInBytes = sizeof(LineVertex);
        vbvGrid_.SizeInBytes = (UINT)gridCpu.size() * sizeof(LineVertex);
    }

    // --- VB осей (экраная толщина) ---
    std::vector<AxisVertex> axesCpu;
    BuildAxesScreenCPU(axesCpu);
    axesVertexCount_ = (UINT)axesCpu.size();
    {
        ComPtr<ID3D12Resource> vb = um.CreateBufferWithData(
            axesCpu.data(), (UINT)axesCpu.size() * sizeof(AxisVertex),
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        vbAxes_ = vb;
        vbvAxes_.BufferLocation = vbAxes_->GetGPUVirtualAddress();
        vbvAxes_.StrideInBytes = sizeof(AxisVertex);
        vbvAxes_.SizeInBytes = (UINT)axesCpu.size() * sizeof(AxisVertex);
    }

    um.StealKeepAlive(uploadKeepAlive);

    // --- Материал для осей (axes.hlsl, TRIANGLES, input=AxisLine) ---
    Material::GraphicsDesc agd = GetGraphicsDesc(); // копируем состояния (бленд, depth, raster)
    agd.shaderFile = L"axes.hlsl";
    agd.vsEntry = "VSMainAxis";
    agd.psEntry = "PSMainAxis";
    agd.inputLayoutKey = "AxisLine";
    agd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    agd.raster.DepthBias = -150;
    axisMat_ = renderer->GetMaterialManager().GetOrCreateGraphics(renderer, agd);
}

void DebugGrid::UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj)
{
    mat4 mvp = (GetModelMatrix() * view * proj);
    UpdateUniform("modelViewProj", mvp.xm());

    UINT w = renderer->GetWidth(), h = renderer->GetHeight();

    UpdateUniform("viewportThickness", DirectX::XMFLOAT4(float(w), float(h), axisThicknessPx_, 0.0f));
}

void DebugGrid::IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }

    // 1) Сетка
    {
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        cl->IASetVertexBuffers(0, 1, &vbvGrid_);
        if (gridVertexCount_ > 0u) { cl->DrawInstanced(gridVertexCount_, 1, 0, 0); }
    }
    // 2) Оси
    {
        if (axisMat_) { axisMat_->Bind(cl, graphicsCtx_); }
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 1, &vbvAxes_);
        cl->DrawInstanced(6, 1, 0, 0);
        cl->DrawInstanced(6, 1, 6, 0);
        cl->DrawInstanced(6, 1, 12, 0);
    }
}