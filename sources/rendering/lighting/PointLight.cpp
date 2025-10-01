#include "rendering/lighting/PointLight.h"
#include "rendering/core/Renderer.h"
#include <algorithm>
#include <array>

using namespace Math;

static D3D12_DEPTH_STENCIL_DESC MakeZFail_DS()
{
    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    ds.StencilEnable = TRUE;
    ds.StencilReadMask = 0xFF;
    ds.StencilWriteMask = 0xFF;
    
    ds.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    ds.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_INCR_SAT; //increase
    ds.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;

    ds.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_DECR_SAT; //decrease
    ds.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;

    return ds;
}

static D3D12_DEPTH_STENCIL_DESC MakeColor_DS()
{
    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.StencilEnable = TRUE;
    ds.StencilReadMask = 0xFF;
    ds.StencilWriteMask = 0x00;
    ds.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
    ds.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    ds.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    ds.BackFace = ds.FrontFace;
    return ds;
}

mat4 PointLight::BuildModel() const
{
    const float s = std::max(0.001f, desc_.radius) * 2;
    return mat4::Scaling(s, s, s) * mat4::Translation(desc_.position);
}

void PointLight::Init(Renderer* r, ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    sphere_ = r->GetMeshManager()->Load("models/sphere.obj", r, uploadCmdList, uploadKeepAlive, { true, false, 0 });

    // --- Z-FAIL ---
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/pointlight_zfail.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = "PosNormTanUV";        // позиции сферы
        gd.numRT = 1;
        gd.rtvFormats[0] = r->GetLightTargetFormat(); // цвет не пишем (mask=0), но RTV можно привязать общий
        gd.dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        gd.depth = MakeZFail_DS();
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        // write mask = 0 (не пишем цвет)
        for (int i = 0; i < 8; ++i) { gd.blend.RenderTarget[i].RenderTargetWriteMask = 0; }
        matZFail_ = r->GetMaterialManager()->GetOrCreateGraphics(r, gd);
    }

    // --- COLOR (fullscreen, аддитив, stencil!=0) ---
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/pointlight_color_fs.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = ""; // fullscreen треугольник
        gd.numRT = 1;
        gd.rtvFormats[0] = r->GetLightTargetFormat();
        gd.dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        gd.depth = MakeColor_DS();
        auto& rt = gd.blend.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        matColorFS_ = r->GetMaterialManager()->GetOrCreateGraphics(r, gd);
    }

    RebuildHandleCache();
}

void PointLight::RebuildHandleCache()
{
    cbHandles_ = {};

    if (matZFail_)
    {
        cbHandles_.zFail.world = matZFail_->ComputeCBFieldHandle(0, "world");
        cbHandles_.zFail.view = matZFail_->ComputeCBFieldHandle(0, "view");
        cbHandles_.zFail.proj = matZFail_->ComputeCBFieldHandle(0, "proj");
    }

    if (matColorFS_)
    {
        cbHandles_.color.frame.view = matColorFS_->ComputeCBFieldHandle(0, "view");
        cbHandles_.color.frame.proj = matColorFS_->ComputeCBFieldHandle(0, "proj");
        cbHandles_.color.frame.invView = matColorFS_->ComputeCBFieldHandle(0, "invView");
        cbHandles_.color.frame.invProj = matColorFS_->ComputeCBFieldHandle(0, "invProj");
        cbHandles_.color.frame.camPos = matColorFS_->ComputeCBFieldHandle(0, "camPosWS");
        cbHandles_.color.frame.screenSize = matColorFS_->ComputeCBFieldHandle(0, "screenSize");

        cbHandles_.color.light.position = matColorFS_->ComputeCBFieldHandle(1, "lightPosWS");
        cbHandles_.color.light.radius = matColorFS_->ComputeCBFieldHandle(1, "lightRadius");
        cbHandles_.color.light.color = matColorFS_->ComputeCBFieldHandle(1, "lightColor");
        cbHandles_.color.light.intensity = matColorFS_->ComputeCBFieldHandle(1, "lightIntensity");
    }
}

void PointLight::RenderZFail(Renderer* r, ID3D12GraphicsCommandList* cl,
                             const mat4& view, const mat4& proj)
{
    if (sphere_ == nullptr) { return; }

    // CB b0: world/view/proj
    auto cb = r->GetFrameResource()->AllocDynamic(matZFail_->GetCBSizeBytesAligned(0, 256), 256);
    matZFail_->UpdateCBField(cbHandles_.zFail.world, BuildModel(), (uint8_t*)cb.cpu);
    matZFail_->UpdateCBField(cbHandles_.zFail.view, view, (uint8_t*)cb.cpu);
    matZFail_->UpdateCBField(cbHandles_.zFail.proj, proj, (uint8_t*)cb.cpu);

    // Сброс ref → 0 (мы будем тестировать !=0 в цвете)
    cl->OMSetStencilRef(0);

	auto h = r->GetRenderContextPool()->Acquire();
    auto& rc = h.ref();
    rc.cbv[0] = cb.gpu;

    matZFail_->Bind(cl, rc);
    sphere_->Draw(cl);
}

void PointLight::RenderColor(Renderer* r, ID3D12GraphicsCommandList* cl,
                             const mat4& view, const mat4& proj,
                             const mat4& invView, const mat4& invProj,
                             const float3& camPos)
{
    // CB b0: per-frame
    auto cb0 = r->GetFrameResource()->AllocDynamic(matColorFS_->GetCBSizeBytesAligned(0, 256), 256);
    matColorFS_->UpdateCBField(cbHandles_.color.frame.view, view, (uint8_t*)cb0.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.frame.proj, proj, (uint8_t*)cb0.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.frame.invView, invView, (uint8_t*)cb0.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.frame.invProj, invProj, (uint8_t*)cb0.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.frame.camPos, camPos, (uint8_t*)cb0.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.frame.screenSize, float2((float)r->GetWidth(), (float)r->GetHeight()), (uint8_t*)cb0.cpu);

    // CB b1: per-light
    auto cb1 = r->GetFrameResource()->AllocDynamic(matColorFS_->GetCBSizeBytesAligned(1, 256), 256);
    matColorFS_->UpdateCBField(cbHandles_.color.light.position, desc_.position, (uint8_t*)cb1.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.light.radius, desc_.radius, (uint8_t*)cb1.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.light.color, desc_.color, (uint8_t*)cb1.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.light.intensity, desc_.intensity, (uint8_t*)cb1.cpu);

    // таблица GBuffer SRV: t0..t3 = GB0,GB1,GB2,Depth
    auto tbl = r->StageGBufferSrvTable();

    auto h = r->GetRenderContextPool()->Acquire();
    auto& rc = h.ref();
    rc.cbv[0] = cb0.gpu;
    rc.cbv[1] = cb1.gpu;
    rc.table[0] = tbl;
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
    rc.samplerTable[0] = r->GetSamplerManager()->GetTable(r, samplerDescs);

    cl->OMSetStencilRef(0);

    matColorFS_->Bind(cl, rc);
    // fullscreen треугольник
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
}

void PointLight::OnMaterialHotReload()
{
    RebuildHandleCache();
}
