#include "DebugGrid.h"

#include <DirectXMath.h>
#include "RenderableObject.h"
#include "Renderer.h"
#include "UploadManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ──────────────────────────────────────────────────────────────
// ВЕРШИННЫЕ ТИПЫ (локально, чтобы не плодить инклуды)
// ──────────────────────────────────────────────────────────────
struct LineVertex {
    float3 pos;
    float4 col;
};

struct AxisVertex {
    float3 a;           // начало отрезка в мире
    float3 b;           // конец   отрезка в мире
    float3 cornerBias;  // xy = (-1/+1), z = edgeBiasPx
    float4 col;         // цвет линии
};

// ──────────────────────────────────────────────────────────────
// GridRO — сетка (линии)
// ──────────────────────────────────────────────────────────────
class DebugGrid::GridRO final : public RenderableObject {
public:
    GridRO(float halfSize, float step, float yPlane, float alpha)
        : RenderableObject(/*matPreset*/"", /*inputLayout*/"PosColor", /*shader*/L"shaders/lines.hlsl")
        , halfSize_(halfSize), step_(step), yPlane_(yPlane), alpha_(alpha)
    {
    }

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive) override
    {
        auto& gd = GetGraphicsDesc();
        gd.numRT = 1;
        gd.rtvFormats[0] = renderer->GetSceneColorFormat();
        gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        gd.blend.RenderTarget[0].BlendEnable = TRUE;
        gd.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        gd.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        gd.blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        gd.blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        gd.blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        gd.blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

        RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);

        std::vector<LineVertex> verts;
        BuildGridCPU(verts);

        UploadManager um(renderer->GetDevice(), uploadCmdList);
        {
            ComPtr<ID3D12Resource> vb = um.CreateBufferWithData(
                verts.data(), static_cast<UINT>(verts.size() * sizeof(LineVertex)),
                D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            vb_ = vb;
        }
        vbv_.BufferLocation = vb_->GetGPUVirtualAddress();
        vbv_.StrideInBytes = sizeof(LineVertex);
        vbv_.SizeInBytes = static_cast<UINT>(verts.size() * sizeof(LineVertex));
        vertexCount_ = static_cast<UINT>(verts.size());

        um.StealKeepAlive(uploadKeepAlive);
    }

    void UpdateUniforms(Renderer* /*renderer*/, const mat4& view, const mat4& proj) override
    {
        mat4 mvp = (GetModelMatrix() * view * proj);
        UpdateUniform("modelViewProj", mvp.xm());
    }

    void IssueDraw(Renderer* /*renderer*/, ID3D12GraphicsCommandList* cl) override
    {
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        cl->IASetVertexBuffers(0, 1, &vbv_);
        if (vertexCount_ > 0u) {
            cl->DrawInstanced(vertexCount_, 1, 0, 0);
        }
    }

    bool IsTransparent() const override { return true; }
    bool IsSimpleRender() const override { return true; }

private:
    void BuildGridCPU(std::vector<LineVertex>& out)
    {
        out.clear();
        if (step_ <= 0.0f) {
            step_ = 1.0f;
        }

        const float hs = (halfSize_ > 0.0f) ? halfSize_ : 10.0f;
        const int   n = static_cast<int>(std::floor(hs / step_));

        const float4 c(1, 1, 1, alpha_);

        for (int i = -n; i <= n; ++i) {
            const float z = i * step_;
            {
                out.push_back({ float3(-hs, yPlane_, z), c });
                out.push_back({ float3(+hs, yPlane_, z), c });
            }
            const float x = i * step_;
            {
                out.push_back({ float3(x, yPlane_, -hs), c });
                out.push_back({ float3(x, yPlane_, +hs), c });
            }
        }
    }

private:
    float halfSize_;
    float step_;
    float yPlane_;
    float alpha_;

    ComPtr<ID3D12Resource> vb_;
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    UINT vertexCount_ = 0;
};

// ──────────────────────────────────────────────────────────────
// AxesRO — оси (толстые линии в экранных пикселях, треугольники)
// ──────────────────────────────────────────────────────────────
class DebugGrid::AxesRO final : public RenderableObject {
public:
    AxesRO(float axisLen, float yPlane, float alpha, float thicknessPx)
        : RenderableObject(/*matPreset*/"", /*inputLayout*/"AxisLine", /*shader*/L"shaders/axes.hlsl")
        , axisLen_(axisLen), yPlane_(yPlane), alpha_(alpha), thicknessPx_(thicknessPx)
    {
    }

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive) override
    {
        auto& gd = GetGraphicsDesc();
        gd.numRT = 1;
        gd.rtvFormats[0] = renderer->GetSceneColorFormat();
        gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        gd.raster.DepthBias = -150;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        gd.blend.RenderTarget[0].BlendEnable = TRUE;
        gd.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        gd.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        gd.blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        gd.blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        gd.blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        gd.blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

        RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);

        std::vector<AxisVertex> verts;
        BuildAxesCPU(verts);

        UploadManager um(renderer->GetDevice(), uploadCmdList);
        {
            ComPtr<ID3D12Resource> vb = um.CreateBufferWithData(
                verts.data(), static_cast<UINT>(verts.size() * sizeof(AxisVertex)),
                D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            vb_ = vb;
        }
        vbv_.BufferLocation = vb_->GetGPUVirtualAddress();
        vbv_.StrideInBytes = sizeof(AxisVertex);
        vbv_.SizeInBytes = static_cast<UINT>(verts.size() * sizeof(AxisVertex));
        vertexCount_ = static_cast<UINT>(verts.size());

        um.StealKeepAlive(uploadKeepAlive);
    }

    void UpdateUniforms(Renderer* r, const mat4& view, const mat4& proj) override
    {
        mat4 mvp = (GetModelMatrix() * view * proj);
        UpdateUniform("modelViewProj", mvp.xm());

        const UINT w = r->GetWidth();
        const UINT h = r->GetHeight();
        UpdateUniform("viewportThickness", XMFLOAT4(float(w), float(h), thicknessPx_, 0.0f));
    }

    void IssueDraw(Renderer* /*renderer*/, ID3D12GraphicsCommandList* cl) override
    {
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 1, &vbv_);
        if (vertexCount_ > 0u) {
            cl->DrawInstanced(vertexCount_, 1, 0, 0);
        }
    }

    bool IsTransparent() const override { return true; }
    bool IsSimpleRender() const override { return true; }

private:
    void BuildAxesCPU(std::vector<AxisVertex>& out)
    {
        out.clear();
        const float L = axisLen_;
        const float4 xC(1, 0, 0, alpha_);
        const float4 yC(0, 1, 0, alpha_);
        const float4 zC(0, 0, 1, alpha_);
        const float eps = 2.25f;

        auto push = [&](float3 A, float3 B, const float4& C) {
            // два треугольника (= шесть вершин) на «толстую» линию в экранных координатах
            {
                out.push_back({ A, B, float3(-1,-1, +eps), C });
                out.push_back({ A, B, float3(-1,+1, +eps), C });
                out.push_back({ A, B, float3(+1,+1, +eps), C });
            }
            {
                out.push_back({ A, B, float3(-1,-1, -eps), C });
                out.push_back({ A, B, float3(+1,+1, -eps), C });
                out.push_back({ A, B, float3(+1,-1, -eps), C });
            }
            };

        // X, Z в плоскости yPlane_, и Y вверх
        push(float3(0.0f, yPlane_, 0.0f), float3(L, yPlane_, 0.0f), xC);
        push(float3(0.0f, yPlane_, 0.0f), float3(0.0f, yPlane_, L), zC);
        push(float3(0.0f, 0.0f, 0.0f), float3(0.0f, L, 0.0f), yC);
    }

private:
    float axisLen_;
    float yPlane_;
    float alpha_;
    float thicknessPx_;

    ComPtr<ID3D12Resource> vb_;
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    UINT vertexCount_ = 0;
};

// ──────────────────────────────────────────────────────────────
// DebugGrid — контейнер из двух рендераблов
// ──────────────────────────────────────────────────────────────
DebugGrid::DebugGrid(float halfSize, float step, float axisLen, float yPlane,
    float gridAlpha, float axisAlpha, float axisThicknessPx)
    : halfSize_(halfSize)
    , step_(step)
    , axisLen_(axisLen)
    , yPlane_(yPlane)
    , gridAlpha_(gridAlpha)
    , axisAlpha_(axisAlpha)
    , axisThicknessPx_(axisThicknessPx)
{
}

DebugGrid::~DebugGrid()
{
	
}

void DebugGrid::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    grid_ = std::make_unique<GridRO>(halfSize_, step_, yPlane_, gridAlpha_);
    axes_ = std::make_unique<AxesRO>(axisLen_, yPlane_, axisAlpha_, axisThicknessPx_);

    grid_->Init(renderer, uploadCmdList, uploadKeepAlive);
    axes_->Init(renderer, uploadCmdList, uploadKeepAlive);
}

void DebugGrid::Tick(float dt)
{
    (void)dt; // сейчас нечего анимировать, но оставим хук
}

void DebugGrid::Render(Renderer* renderer,
    ID3D12GraphicsCommandList* cl,
    const mat4& view,
    const mat4& proj)
{
    if (grid_) {
        grid_->Render(renderer, cl, view, proj);
    }
    if (axes_) {
        axes_->Render(renderer, cl, view, proj);
    }
}