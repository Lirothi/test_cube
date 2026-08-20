#include "rendering/lighting/PointLight.h"
#include "rendering/core/PhotographicSettings.h" // P16.5 CandelaFromLumens
#include "rendering/core/Renderer.h"
#include <algorithm>
#include <array>
#include <cmath>

using namespace Math;

static D3D12_DEPTH_STENCIL_DESC MakeZFail_DS()
{
    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
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

namespace
{
    constexpr float kTwoPi = 6.28318530718f;

    float SeedPhase(std::uint32_t seed, std::uint32_t salt)
    {
        // A compact integer hash gives each light stable but distinct phases.
        std::uint32_t v = seed ^ salt;
        v ^= v >> 16;
        v *= 0x7feb352du;
        v ^= v >> 15;
        v *= 0x846ca68bu;
        v ^= v >> 16;
        return static_cast<float>(v & 0x00ffffffu) * (kTwoPi / 16777215.0f);
    }
}

void PointLight::SetDesc(const PointLightDesc& d)
{
    baseDesc_ = d;
    ApplyFlicker();
    ++transformVersion_;
}

void PointLight::Tick(float deltaTime)
{
    const PointLightFlickerDesc& flicker = baseDesc_.flicker;
    if (deltaTime <= 0.0f || flicker.amplitude <= 0.0f || flicker.frequencyHz <= 0.0f)
    {
        return;
    }

    flickerTime_ += deltaTime;
    ApplyFlicker();
}

void PointLight::ApplyFlicker()
{
    desc_ = baseDesc_;
    const PointLightFlickerDesc& flicker = baseDesc_.flicker;
    const float amplitude = std::clamp(flicker.amplitude, 0.0f, 1.0f);
    const float frequencyHz = std::max(flicker.frequencyHz, 0.0f);
    if (amplitude <= 0.0f || frequencyHz <= 0.0f)
    {
        return;
    }

    // The weights sum to one, keeping the authored amplitude an upper bound while
    // the incommensurate waves avoid the mechanical regularity of one pure sine.
    const float cycle = kTwoPi * frequencyHz * flickerTime_;
    const float signal =
        0.58f * std::sin(cycle + SeedPhase(flicker.seed, 0x68bc21ebu)) +
        0.28f * std::sin(cycle * 1.73f + SeedPhase(flicker.seed, 0x02e5be93u)) +
        0.14f * std::sin(cycle * 0.37f + SeedPhase(flicker.seed, 0x9e3779b9u));
    desc_.luminousFluxLm = std::max(0.0f, baseDesc_.luminousFluxLm * (1.0f + amplitude * signal));
}

mat4 PointLight::BuildModel() const
{
    const float s = std::max(0.001f, desc_.radius) * 2;
    return mat4::Scaling(s, s, s) * mat4::Translation(desc_.position);
}

void PointLight::Init(Renderer* r, ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    sphere_ = r->GetMeshManager()->Load("models/sphere/sphere.mesh.bin", r, uploadCmdList, uploadKeepAlive, { true, false, 0 });

    // --- Z-FAIL ---
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/pointlight_zfail.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = "PosNormTanUV";        // sphere vertex positions
        gd.numRT = 1;
        gd.rtvFormats[0] = r->GetLightTargetFormat(); // color is not written (mask=0), but we can bind the shared RTV
        gd.dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        gd.depth = MakeZFail_DS();
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        // write mask = 0 (skip writing color)
        for (int i = 0; i < 8; ++i) { gd.blend.RenderTarget[i].RenderTargetWriteMask = 0; }
        matZFail_ = r->GetMaterialManager()->GetOrCreateGraphics(r, gd);
    }

    // --- COLOR (fullscreen, additive, stencil!=0) ---
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/pointlight_color_fs.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = ""; // fullscreen triangle
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
        cbHandles_.zFail.viewProj = matZFail_->ComputeCBFieldHandle(0, "viewProj");
    }

    if (matColorFS_)
    {
        cbHandles_.color.frame.invView = matColorFS_->ComputeCBFieldHandle(0, "invView");
        cbHandles_.color.frame.invProj = matColorFS_->ComputeCBFieldHandle(0, "invProj");
        cbHandles_.color.frame.camPos = matColorFS_->ComputeCBFieldHandle(0, "camPosWS");

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

    // CB b0: world/viewProj
    auto cb = r->GetFrameResource()->AllocDynamic(
        matZFail_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment),
        render::kConstantBufferAlignment);
    const mat4 viewProj = view * proj;
    matZFail_->UpdateCBField(cbHandles_.zFail.world, BuildModel(), (uint8_t*)cb.cpu);
    matZFail_->UpdateCBField(cbHandles_.zFail.viewProj, viewProj, (uint8_t*)cb.cpu);

    // Reset the stencil ref to 0 (color pass tests for != 0)
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
    (void)view;
    (void)proj;

    // CB b0: per-frame
    auto cb0 = r->GetFrameResource()->AllocDynamic(
        matColorFS_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment),
        render::kConstantBufferAlignment);
    matColorFS_->UpdateCBField(cbHandles_.color.frame.invView, invView, (uint8_t*)cb0.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.frame.invProj, invProj, (uint8_t*)cb0.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.frame.camPos, camPos, (uint8_t*)cb0.cpu);

    // CB b1: per-light
    auto cb1 = r->GetFrameResource()->AllocDynamic(
        matColorFS_->GetCBSizeBytesAligned(1, render::kConstantBufferAlignment),
        render::kConstantBufferAlignment);
    matColorFS_->UpdateCBField(cbHandles_.color.light.position, desc_.position, (uint8_t*)cb1.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.light.radius, desc_.radius, (uint8_t*)cb1.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.light.color, desc_.color, (uint8_t*)cb1.cpu);
    matColorFS_->UpdateCBField(cbHandles_.color.light.intensity,
                               render::CandelaFromLumens(desc_.luminousFluxLm), (uint8_t*)cb1.cpu);

    // GBuffer SRV table: t0..t4 = GB0, GB1, GB2, GBVelocity, Depth
    auto tbl = r->StageGBufferSrvTable();

    auto h = r->GetRenderContextPool()->Acquire();
    auto& rc = h.ref();
    rc.cbv[0] = cb0.gpu;
    rc.cbv[1] = cb1.gpu;
    rc.srvTable[0] = tbl;
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
    rc.samplerTable[0] = r->GetSamplerManager()->GetTable(r, samplerDescs);

    cl->OMSetStencilRef(0);

    matColorFS_->Bind(cl, rc);
    // Fullscreen triangle
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
}

void PointLight::OnMaterialHotReload()
{
    RebuildHandleCache();
}
