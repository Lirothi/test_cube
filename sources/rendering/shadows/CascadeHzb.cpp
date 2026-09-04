#include "rendering/shadows/CascadeHzb.h"

#include <algorithm>
#include <cstring>

#include "core/logging/Log.h"
#include "materials/Material.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/TextureCreate.h"

namespace render
{
namespace
{
    // Mirrors HzbCB of hzb_build_cs.hlsl under HZB_LIGHT=1.
    struct HzbLightCB
    {
        std::uint32_t dstSize[2];
        std::uint32_t srcSize[2];
        std::uint32_t fromDepth;
        std::uint32_t writeClosest;
        std::uint32_t pad1, pad2;
        std::uint32_t srcOffset[2];
        std::uint32_t pad3[2];
    };
    static_assert(sizeof(HzbLightCB) == 48, "HzbCB (HZB_LIGHT) layout");

    // Row-vector z flip: clip.z' = w - z, i.e. 1 - z for an orthographic projection (w == 1).
    Math::mat4 FlipZ()
    {
        Math::mat4 f = Math::mat4::Identity();
        f.m.m[2][2] = -1.0f;
        f.m.m[3][2] = 1.0f;
        return f;
    }
}

void CascadeHzb::ReleasePyramids()
{
    for (auto& p : pyramid_) { p.Reset(); }
    heap_.Reset();
    srv_.fill({});
    for (auto& m : mipUav_) { m.fill({}); }
    width_ = height_ = mips_ = 0;
    contentRes_ = 0;
    builtFrame_.fill(0);
}

void CascadeHzb::Invalidate()
{
    builtFrame_.fill(0);
    haveViews_ = false;
    active_ = false;
}

bool CascadeHzb::EnsureResources(Renderer* renderer, UINT contentRes)
{
    if (failed_) { return false; }
    ID3D12Device* device = renderer ? renderer->GetDevice() : nullptr;
    if (!device || contentRes < 2) { return false; }
    if (Ready() && contentRes_ == contentRes) { return true; }

    const auto fail = [&](const char* what)
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "cascade hzb: {} failed; light-space occlusion off", what);
        ReleasePyramids();
        failed_ = true;
        return false;
    };

    if (!buildMat_)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/hzb_build_cs.hlsl";
        cd.csEntry = "CSMain";
        cd.defines.emplace_back("HZB_LIGHT", "1");
        buildMat_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
        if (!buildMat_ || !buildMat_->GetPipelineState()) { buildMat_.reset(); return fail("hzb_build_cs.hlsl HZB_LIGHT PSO"); }
    }

    // A resolution change: the old pyramids go, and with them the history (a pyramid of another
    // size is not last frame's tile).
    ReleasePyramids();

    // Mip 0 = a QUARTER of the content rect rounded up (one 4x4 minimum per texel, HZB_LIGHT's
    // first level), coarser mips floor-halved with the odd tail folded (hzb_build_cs.hlsl). The
    // cull is told the tile is HALF its size (FillParams), so to the library this is the usual
    // "mip 0 = half the view" pyramid and its texel clamp (delta 2) applies unchanged. Measured:
    // the half-res version cost 0.15 ms GPU for four 1020^2 chains against a 0.1-0.2 ms Pass_CSM.
    const UINT w = std::max(1u, (contentRes + 3u) / 4u);
    const UINT h = w;
    UINT mips = 1;
    while (((w >> mips) > 0 || (h >> mips) > 0) && mips < kMaxMips) { ++mips; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kCascades * (1 + kMaxMips);
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) { return fail("descriptor heap"); }
    const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = heap_->GetCPUDescriptorHandleForHeapStart();

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = static_cast<UINT16>(mips);
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    static const wchar_t* const kNames[kCascades] = { L"CascadeHzb0", L"CascadeHzb1", L"CascadeHzb2", L"CascadeHzb3" };
    for (unsigned c = 0; c < kCascades; ++c)
    {
        // Created and resting in NON_PIXEL (the cull reads it); UNORDERED_ACCESS only for the build.
        if (FAILED(render::CreateCommittedTexture(device, hp, D3D12_HEAP_FLAG_NONE, rd,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                  pyramid_[c].GetAddressOfForCreate())))
        {
            return fail("pyramid texture");
        }
        pyramid_[c].DeclareCreated(renderer->Declarations(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kNames[c]);

        const SIZE_T first = static_cast<SIZE_T>(c) * (1 + kMaxMips);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = mips;
        srv_[c] = D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + first * incr };
        device->CreateShaderResourceView(pyramid_[c].Get(), &sd, srv_[c]);
        for (UINT m = 0; m < kMaxMips; ++m)
        {
            // Slots past the real mip count point at the last mip: a VOLATILE table may not hold a
            // hole (RenderTargetManager does the same for the camera pyramid).
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_FLOAT;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            ud.Texture2D.MipSlice = (m < mips) ? m : (mips - 1);
            mipUav_[c][m] = D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + (first + 1 + m) * incr };
            device->CreateUnorderedAccessView(pyramid_[c].Get(), nullptr, &ud, mipUav_[c][m]);
        }
    }

    contentRes_ = contentRes;
    width_ = w;
    height_ = h;
    mips_ = mips;
    LOG_INFO(logging::LogCategory::RenderShadow, "cascade hzb: {} pyramids {}x{} x {} mips (quarter of a {} tile)",
             kCascades, w, h, mips, contentRes);
    return true;
}

void CascadeHzb::SetFrameViews(const Math::mat4* lightViewProj, std::uint64_t frameNumber, bool active)
{
    static const Math::mat4 kFlip = FlipZ();
    if (haveViews_) { prevViewProjRev_ = viewProjRev_; }
    for (unsigned c = 0; c < kCascades; ++c)
    {
        viewProjRev_[c] = lightViewProj ? (lightViewProj[c] * kFlip) : Math::mat4::Identity();
    }
    if (!haveViews_) { prevViewProjRev_ = viewProjRev_; }
    haveViews_ = lightViewProj != nullptr;
    frame_ = frameNumber;
    active_ = active && Ready() && haveViews_;
}

bool CascadeHzb::PrevValid(unsigned c) const
{
    // Built last frame, with the matrices now in `prev` -- both by the same frame counter.
    return c < kCascades && active_ && frame_ > 0 && builtFrame_[c] == frame_ - 1;
}

void CascadeHzb::FillParams(GpuParams& out, bool on) const
{
    std::memset(&out, 0, sizeof(out));
    for (unsigned c = 0; c < kCascades; ++c)
    {
        out.prevViewProj[c] = prevViewProjRev_[c];
        out.viewProj[c] = viewProjRev_[c];
        out.prevValid[c] = (on && PrevValid(c)) ? 1u : 0u;
    }
    // Half the content: the library derives mip-0 texels as view pixels >> 1, and mip 0 here is a
    // quarter of the tile (see EnsureResources).
    const std::int32_t view = static_cast<std::int32_t>((contentRes_ + 1u) / 2u);
    out.viewRect[0] = 0;
    out.viewRect[1] = 0;
    out.viewRect[2] = view;
    out.viewRect[3] = view;
    out.hzbSize[0] = width_;
    out.hzbSize[1] = height_;
    out.on = on ? 1u : 0u;
}

void CascadeHzb::MarkBuilt(std::uint64_t frameNumber)
{
    builtFrame_.fill(frameNumber);
}

void CascadeHzb::RecordBuild(Renderer* renderer, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE atlasSrv,
                             UINT tileRes, UINT border)
{
    if (!renderer || !cl || !Ready() || atlasSrv.ptr == 0) { return; }

    const auto samplerDescs = std::array{ *SamplerManager::PointClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

    for (unsigned c = 0; c < kCascades; ++c)
    {
        const UINT originX = (c % 2u) * tileRes + border;
        const UINT originY = (c / 2u) * tileRes + border;
        UINT srcW = contentRes_;
        UINT srcH = contentRes_;
        for (UINT mip = 0; mip < mips_; ++mip)
        {
            const UINT dstW = std::max(1u, width_ >> mip);
            const UINT dstH = std::max(1u, height_ >> mip);
            HzbLightCB k{};
            k.dstSize[0] = dstW;
            k.dstSize[1] = dstH;
            k.srcSize[0] = srcW;
            k.srcSize[1] = srcH;
            k.fromDepth = (mip == 0) ? 1u : 0u;
            k.writeClosest = 0u;
            k.srcOffset[0] = originX;
            k.srcOffset[1] = originY;
            // u0/u2 are the source mips (mip 0 reads the atlas instead; the table must stay
            // full, so they point at mip 0 as inert placeholders); u1 the destination; u3 the
            // closest chain's destination, never written (writeClosest = 0) -- bound to the
            // destination mip so the table holds a valid descriptor.
            const D3D12_CPU_DESCRIPTOR_HANDLE srcUav = (mip == 0) ? mipUav_[c][0] : mipUav_[c][mip - 1];
            RecordComputeDispatch(renderer, cl, buildMat_.get(), static_cast<UINT>(sizeof(HzbLightCB)),
                [&](std::uint8_t* dst) { std::memcpy(dst, &k, sizeof(k)); },
                { atlasSrv },
                { srcUav, mipUav_[c][mip], srcUav, mipUav_[c][mip] },
                samplerTable,
                dstW, dstH,
                pyramid_[c].Get()); // UAV barrier: the next level reads what this one wrote
            srcW = dstW;
            srcH = dstH;
        }
    }
}
} // namespace render
