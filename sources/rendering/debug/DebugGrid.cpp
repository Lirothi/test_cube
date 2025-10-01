#include "rendering/debug/DebugGrid.h"

#include <DirectXMath.h>
#include "rendering/renderables/RenderableObject.h"
#include "rendering/core/Renderer.h"
#include "materials/UploadManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
class GridMVPBinder final : public RenderableObject::UniformBinder
{
public:
    void RebuildHandles(RenderableObject& owner) override
    {
        if (Material* material = owner.GetGraphicsMaterial())
        {
            mvpHandle_ = material->ComputeCBFieldHandle(0, "modelViewProj");
        }
        else
        {
            mvpHandle_ = {};
        }
    }

    void UpdateMainCB(RenderableObject& owner, Renderer* /*renderer*/, const mat4& view, const mat4& proj, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material) { return; }

        mat4 mvp = owner.GetModelMatrix() * view * proj;
        UpdateUniform(owner, mvpHandle_, material, mvp, cbData);
    }

private:
    Material::CBFieldHandle mvpHandle_{};
};

class AxesUniformBinder final : public RenderableObject::UniformBinder
{
public:
    explicit AxesUniformBinder(const float* thicknessPx) : thicknessPx_(thicknessPx) {}

    void RebuildHandles(RenderableObject& owner) override
    {
        if (Material* material = owner.GetGraphicsMaterial())
        {
            mvpHandle_ = material->ComputeCBFieldHandle(0, "modelViewProj");
            viewportThicknessHandle_ = material->ComputeCBFieldHandle(0, "viewportThickness");
        }
        else
        {
            mvpHandle_ = {};
            viewportThicknessHandle_ = {};
        }
    }

    void UpdateMainCB(RenderableObject& owner, Renderer* renderer, const mat4& view, const mat4& proj, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material || !renderer) { return; }

        mat4 mvp = owner.GetModelMatrix() * view * proj;
        UpdateUniform(owner, mvpHandle_, material, mvp, cbData);

        const UINT w = renderer->GetWidth();
        const UINT h = renderer->GetHeight();
        const float thicknessPx = thicknessPx_ ? *thicknessPx_ : 0.0f;
        UpdateUniform(owner, viewportThicknessHandle_, material, float4(float(w), float(h), thicknessPx, 0.0f), cbData);
    }

private:
    const float* thicknessPx_ = nullptr;
    Material::CBFieldHandle mvpHandle_{};
    Material::CBFieldHandle viewportThicknessHandle_{};
};
} // namespace

// ──────────────────────────────────────────────────────────────
// VERTEX TYPES (local to avoid extra includes)
// ──────────────────────────────────────────────────────────────
struct LineVertex {
    float3 pos;
    float4 col;
};

struct AxisVertex {
    float3 a;           // segment start in world space
    float3 b;           // segment end in world space
    float3 cornerBias;  // xy = (-1/+1), z = edgeBiasPx
    float4 col;         // line color
};

// ──────────────────────────────────────────────────────────────
// GridRO — grid (lines)
// ──────────────────────────────────────────────────────────────
class DebugGrid::GridRO final : public RenderableObject {
public:
    GridRO(float halfSize, float step, float yPlane, float alpha)
        : RenderableObject(/*inputLayout*/"PosColor", /*shader*/L"shaders/lines.hlsl")
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

        if (!GetUniformBinder())
        {
            SetUniformBinder(std::make_unique<GridMVPBinder>());
        }

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
    bool CastsShadow() const override { return false; }

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
// AxesRO — axes (thick lines in screen pixels, triangles)
// ──────────────────────────────────────────────────────────────
class DebugGrid::AxesRO final : public RenderableObject {
public:
    AxesRO(float axisLen, float yPlane, float alpha, float thicknessPx)
        : RenderableObject(/*inputLayout*/"AxisLine", /*shader*/L"shaders/axes.hlsl")
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

        if (!GetUniformBinder())
        {
            SetUniformBinder(std::make_unique<AxesUniformBinder>(&thicknessPx_));
        }

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

    float GetThicknessPx() const { return thicknessPx_; }

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
    bool CastsShadow() const override { return false; }

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
            // Two triangles (= six vertices) per "thick" line in screen space
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

        // X and Z lie in the yPlane_, Y points upward
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
// DebugGrid — container composed of two renderables
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
    (void)dt; // nothing to animate yet, but keep the hook
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