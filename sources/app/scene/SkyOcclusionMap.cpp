#include "app/scene/SkyOcclusionMap.h"

#include <algorithm>
#include <cmath>

#include "core/logging/Log.h"
#include "materials/Material.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/Renderer.h"
#include "rendering/RenderLayers.h"

namespace
{
float SnapTo(float v, float step) { return std::floor(v / step + 0.5f) * step; }
} // namespace

SkyOcclusionMap::SkyOcclusionMap() = default;
SkyOcclusionMap::~SkyOcclusionMap() = default;

std::uint32_t SkyOcclusionMap::ClampResolution(std::uint32_t resolution)
{
    std::uint32_t best = kMinResolution;
    for (std::uint32_t r = kMinResolution; r <= kMaxResolution; r *= 2u)
    {
        // The nearest in log2: r beats best when resolution lies above their geometric mean.
        if (static_cast<double>(resolution) * resolution >= static_cast<double>(r) * best) { best = r; }
    }
    return best;
}

float SkyOcclusionMap::ClampTexel(float texel)
{
    return std::isfinite(texel) ? std::clamp(texel, kMinTexel, kMaxTexel) : 1.0f;
}

double SkyOcclusionMap::MegabytesAt(std::uint32_t resolution)
{
    const double depth = static_cast<double>(resolution) * kDepthPerTexel;
    const double nearSize = resolution;
    const double farSize = static_cast<double>(resolution) / kFarPerTexel;
    return (3.0 * depth * depth * 2.0 + (nearSize * nearSize + farSize * farSize) * 8.0) / (1024.0 * 1024.0);
}

void SkyOcclusionMap::Prepare(Renderer* renderer, std::uint32_t resolution)
{
    if (ready_ || failed_ || !renderer) { return; }
    ID3D12Device* device = renderer->GetDevice();
    if (!device) { return; }
    const auto fail = [this](const char* what) {
        LOG_ERROR(logging::LogCategory::Render, "sky occlusion map: {} failed -- without RT the sky stays open", what);
        failed_ = true;
    };

    if (!pipelines_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC dh{};
        dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dh.NumDescriptors = 3;
        if (FAILED(gpu::CreateDescriptorHeap(device, &dh, MemTag::Ao, L"SkyOcc.DsvHeap", IID_PPV_ARGS(&dsvHeap_)))) { fail("DSV heap"); return; }
        D3D12_DESCRIPTOR_HEAP_DESC sh{};
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.NumDescriptors = 7;
        if (FAILED(gpu::CreateDescriptorHeap(device, &sh, MemTag::Ao, L"SkyOcc.SrvHeap", IID_PPV_ARGS(&srvHeap_)))) { fail("SRV heap"); return; }
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE base = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
            const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            for (UINT i = 0; i < 3; ++i) { dsv_[i] = { base.ptr + SIZE_T(i) * step }; }
        }
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE base = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for (UINT i = 0; i < 7; ++i) { srv_[i] = { base.ptr + SIZE_T(i) * step }; }
        }

        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/sky_occlusion_cs.hlsl";
        cd.csEntry = "CSReduce";
        cd.defines = { { "SKYOCC_PASS", "0" } };
        reduceMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
        cd.csEntry = "CSReduceFar";
        cd.defines = { { "SKYOCC_PASS", "1" } };
        reduceFarMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
        cd.csEntry = "CSTrace";
        cd.defines = { { "SKYOCC_PASS", "2" } };
        traceMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
        if (!reduceMaterial_ || !reduceMaterial_->GetPipelineState() || !reduceFarMaterial_ ||
            !reduceFarMaterial_->GetPipelineState() || !traceMaterial_ || !traceMaterial_->GetPipelineState())
        {
            fail("PSO");
            return;
        }
        pipelines_ = true;
    }

    if (!CreateTextures(renderer, ClampResolution(resolution)))
    {
        ReleaseTextures();
        fail("texture");
        return;
    }
    ready_ = true;
}

bool SkyOcclusionMap::CreateTextures(Renderer* renderer, std::uint32_t resolution)
{
    ID3D12Device* device = renderer->GetDevice();
    nearSize_ = resolution;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    // The three depth renders: rasterized as depth, then read by the reduce, so they rest shader-readable (the shore
    // map's rule). D16, the CPU shadow path's depth format (RenderableObject::ConfigureShadowPipeline).
    const auto makeDepth = [&](GpuResource& out, const wchar_t* name, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                               D3D12_CPU_DESCRIPTOR_HANDLE srv) -> bool {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = DepthSize();
        desc.Height = DepthSize();
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D16_UNORM;
        clear.DepthStencil.Depth = 1.0f;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kDepthRest, &clear, MemTag::Ao,
                                               name, &resource)))
        {
            return false;
        }
        out.Attach(renderer->Declarations(), resource, kDepthRest, name);
        D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D16_UNORM;
        dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(resource.Get(), &dd, dsv);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R16_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource.Get(), &sd, srv);
        return true;
    };
    // A level: (coverage, coverage x top, coverage x bottom, ground), written by the reduce, read by the trace.
    const auto makeLevel = [&](GpuResource& out, UINT size, const wchar_t* name, D3D12_CPU_DESCRIPTOR_HANDLE srv,
                               D3D12_CPU_DESCRIPTOR_HANDLE uav) -> bool {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = size;
        desc.Height = size;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        if (FAILED(gpu::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, desc, kLevelRest, nullptr, MemTag::Ao,
                                               name, &resource)))
        {
            return false;
        }
        out.Attach(renderer->Declarations(), resource, kLevelRest, name);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = desc.Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource.Get(), &sd, srv);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = desc.Format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource.Get(), nullptr, &ud, uav);
        return true;
    };
    if (!makeDepth(top_, L"SkyOcc.TopDepth", dsv_[0], srv_[0]) ||
        !makeDepth(bottom_, L"SkyOcc.BottomDepth", dsv_[1], srv_[1]) ||
        !makeDepth(terrain_, L"SkyOcc.TerrainDepth", dsv_[2], srv_[2]) ||
        !makeLevel(near_, NearSize(), L"SkyOcc.Near", srv_[3], srv_[4]) ||
        !makeLevel(far_, FarSize(), L"SkyOcc.Far", srv_[5], srv_[6]))
    {
        return false;
    }
    LOG_INFO(logging::LogCategory::Render, "sky occlusion map: made at {}^2 (depth {}^2 x 3, far {}^2): {:.1f} MB",
             NearSize(), DepthSize(), FarSize(), MegabytesAt(NearSize()));
    return true;
}

void SkyOcclusionMap::ReleaseTextures()
{
    top_.Reset();
    bottom_.Reset();
    terrain_.Reset();
    near_.Reset();
    far_.Reset();
}

void SkyOcclusionMap::Resize(Renderer* renderer, std::uint32_t resolution)
{
    if (!ready_) { return; }
    const std::uint32_t was = nearSize_;
    ReleaseTextures();
    ready_ = false;
    nearSize_ = 0;
    Invalidate();
    if (resolution == 0u)
    {
        LOG_INFO(logging::LogCategory::Render, "sky occlusion map: freed ({}^2, {:.1f} MB) -- not wanted", was,
                 MegabytesAt(was));
        return;
    }
    LOG_INFO(logging::LogCategory::Render, "sky occlusion map: resolution {} -> {}", was, ClampResolution(resolution));
    Prepare(renderer, resolution);
}

void SkyOcclusionMap::Invalidate()
{
    draw_ = false;
    drawRegistered_ = false;
    drawn_ = false;
}

SkyOcclusionMap::Placement SkyOcclusionMap::PlaceAt(const Math::float3& centre, float texel) const
{
    Placement p;
    p.centre = centre;
    p.texel = texel;
    p.halfExtent = 0.5f * static_cast<float>(nearSize_) * texel;
    const Math::float3 topEye(centre.x, centre.y + kHalfHeight, centre.z);
    const Math::float3 bottomEye(centre.x, centre.y - kHalfHeight, centre.z);
    p.topEyeY = topEye.y;
    p.bottomEyeY = bottomEye.y;
    // View x = world x and view y = world z from above (LookAtLH with +Z up); from below view x = -world x -- the
    // reduce projects each through its own matrix, so neither has to be mirrored (a mirrored matrix would flip the
    // winding, and the bottom render has to see the faces that look DOWN).
    p.topView = Math::mat4::LookAtLH(topEye, topEye + Math::float3(0.0f, -1.0f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f));
    p.bottomView = Math::mat4::LookAtLH(bottomEye, bottomEye + Math::float3(0.0f, 1.0f, 0.0f), Math::float3(0.0f, 0.0f, 1.0f));
    p.proj = Math::mat4::OrthoOffCenterLH(-p.halfExtent, p.halfExtent, -p.halfExtent, p.halfExtent, 0.0f, 2.0f * kHalfHeight);
    return p;
}

void SkyOcclusionMap::PlaceViews(const Placement& p)
{
    const auto place = [&p](SceneView& v, std::uint32_t mask) {
        v.type = SceneView::Type::SkyOcclusion;
        v.renderLayerMask = mask;
        v.position = Math::float3(p.centre.x, p.topEyeY, p.centre.z);
        v.view = p.topView;
        v.proj = p.proj;
        v.invView = Math::mat4::Inverse(v.view);
        v.invProj = Math::mat4::Inverse(v.proj);
        // The box both cameras see: the bottom one looks up through the same volume, so one cull serves both renders.
        v.frustum = Frustum::FromOrthoBounds(v.invView, p.halfExtent, p.halfExtent, kHalfHeight, p.centre);
        v.zNear = 0.0f;
        v.zFar = 2.0f * kHalfHeight;
        v.hfov = 0.0f;
        v.requiresDepthCheck = false;
    };
    place(objectsView_, RenderLayerMask(RenderLayer::Default));
    place(terrainView_, RenderLayerMask(RenderLayer::Terrain));
}

void SkyOcclusionMap::Update(const Math::float3& cameraPos, std::uint32_t casterSetVersion, bool casterMoved, bool wanted,
                             float texel)
{
    draw_ = false;
    drawRegistered_ = false;
    framesSinceDraw_ = std::min(framesSinceDraw_ + 1u, 1u << 30);
    moveOutstanding_ = moveOutstanding_ || casterMoved;
    if (!wanted || !Ready()) { return; }
    texel = ClampTexel(texel);
    const float snap = kSnapTexels * texel;
    // Up and down in 16 m steps whatever the texel: the depths are continuous, the 100 m either side the only bound.
    const Math::float3 centre(SnapTo(cameraPos.x, snap), SnapTo(cameraPos.y, 16.0f), SnapTo(cameraPos.z, snap));
    const bool placed = !drawn_ || centre.x != drawnPlacement_.centre.x || centre.y != drawnPlacement_.centre.y ||
                        centre.z != drawnPlacement_.centre.z || texel != drawnPlacement_.texel ||
                        0.5f * static_cast<float>(nearSize_) * texel != drawnPlacement_.halfExtent;
    const bool moved = moveOutstanding_ && framesSinceDraw_ >= kMovedRedrawFrames;
    if (!placed && !moved && casterSetVersion == drawnCasterSet_) { return; }
    current_ = PlaceAt(centre, texel);
    PlaceViews(current_);
    pendingCasterSet_ = casterSetVersion;
    moveOutstanding_ = false;
    framesSinceDraw_ = 0;
    draw_ = true;
}

void SkyOcclusionMap::MarkReduced()
{
    drawnPlacement_ = current_;
    drawnCasterSet_ = pendingCasterSet_;
    drawn_ = true;
}

Math::float4 SkyOcclusionMap::MapParams(const Placement& p)
{
    const float width = 2.0f * p.halfExtent;
    return Math::float4(p.centre.x - p.halfExtent, p.centre.z - p.halfExtent, 1.0f / width, p.texel);
}

Math::float4 SkyOcclusionMap::HeightParams(const Placement& p)
{
    return Math::float4(p.topEyeY, p.bottomEyeY, 0.0f, 2.0f * kHalfHeight);
}

float SkyOcclusionMap::FullRadius(const Placement& p, float reach)
{
    // The camera stands within half a snap step of the centre on each axis, so a circle of this radius plus the reach
    // around it lies inside the map wherever it stands: every step of every direction of a point inside is on the map.
    // A map too small for the reach keeps half its inner radius (the directions that leave it see the open sky).
    const float inner = std::max(p.halfExtent - 0.5f * p.Snap(), 0.0f);
    return std::max(inner - std::max(reach, 0.0f), 0.5f * inner);
}

Math::float4 SkyOcclusionMap::FadeParams(const Placement& p, const Math::float3& camera, float reach, float fade)
{
    const float end = FullRadius(p, reach);
    const float width = std::clamp(fade, 0.0f, 1.0f) * end;
    // A fade of 0 = the hard edge: the gradient's width a millimetre.
    return Math::float4(camera.x, camera.z, end - width, 1.0f / std::max(width, 1.0e-3f));
}
