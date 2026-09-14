#include "rendering/shadows/SdsmShadows.h"

#include <algorithm>
#include <cstring>

#include "core/logging/Log.h"
#include "materials/Material.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/RenderContext.h"
#include "rendering/core/RenderContextPool.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/TextureCreate.h" // barrier plan step 11: the ONE place a texture is made
#include "rendering/renderables/InstanceTypes.h" // render::kShadowViewPlanes

namespace
{
using render::sdsm::kMaxPartitions;

constexpr UINT kPlanesPerView = render::kShadowViewPlanes;
// ONE MORE SLOT THAN kMaxShadowViews. ShadowGpuData's own ring carries the CAMERA frustum past
// the shadow views (occlusion plan S4 addresses it as slot kMaxShadowViews), so its
// ViewFrustumCount() is 47, not 46 -- and CarryFrustums copies every slot that count names.
// Sizing this to 46 made the copy write one view past the end, which GBV reports as an
// out-of-bounds UAV store and which a Release run would simply corrupt whatever follows.
constexpr UINT kFrustumViews = render::kMaxShadowViews + 1u;
constexpr UINT kFrustumFloat4s = kFrustumViews * kPlanesPerView;

// Heap slots, in the order EnsureDescriptors writes them. Named because a descriptor TABLE is
// positional (a skipped slot shifts everything after it) and these are staged as two tables.
enum HeapSlot : UINT
{
    kSlotZBoundsUav = 0,
    kSlotBoundsUav,
    kSlotPartitionUav,
    kSlotFrustumUav,
    kSlotViewCbUav,
    kSlotCullCbUav,
    kSlotPartitionSrv,
    kSlotPrevPartitionSrv,
    kSlotFrustumSrv,
    kSlotCount
};

bool CreateBuffer(Renderer* renderer, GpuResource& out, UINT64 bytes,
                  D3D12_RESOURCE_STATES canonical, const wchar_t* name)
{
    if (!renderer || !renderer->GetDevice()) { return false; }
    if (out) { return true; }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<UINT64>(bytes, 16ull);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    if (FAILED(renderer->GetDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(buffer.GetAddressOf()))) || !buffer)
    {
        return false;
    }
    out.Attach(renderer->Declarations(), buffer, D3D12_RESOURCE_STATE_COMMON, canonical, name);
    return true;
}
} // namespace

SdsmShadows::SdsmShadows() = default;
SdsmShadows::~SdsmShadows() = default;

// ---------------------------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------------------------

bool SdsmShadows::EnsureResources(Renderer* renderer)
{
    if (ready_) { return true; }
    if (!renderer || !renderer->GetDevice()) { return false; }
    EnsurePipelines(renderer);
    if (!clearMat_ || !reduceZMat_ || !logMat_ || !reduceBoundsMat_ || !finalizeMat_ || !carryMat_)
    {
        return false;
    }
    if (!EnsureBuffers(renderer) || !EnsureDescriptors(renderer)) { return false; }
    ready_ = true;
    LOG_INFO(logging::LogCategory::RenderShadow,
             "SDSM resources ready ({} partitions, {} cull-view slots)",
             kMaxPartitions, render::kMaxShadowViews);
    return true;
}

bool SdsmShadows::EnsureBuffers(Renderer* renderer)
{
    // CANONICAL STATES. The frame LEAVES each of these where its last consumer needs it, and the
    // graph's compiled barriers start from that (barrier plan step 6b):
    //   partitions / frustums -- read by the cull (NON_PIXEL), the depth VS (NON_PIXEL) and
    //     lighting (NON_PIXEL) but ALSO by glass, which is a PIXEL shader -> the combined SRV
    //     state, the same one the CSM atlas itself carries;
    //   zbounds / bounds -- pure scratch, never read outside the analyze pass, so UAV.
    constexpr D3D12_RESOURCE_STATES kSrvAll =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    bool ok = true;
    ok &= CreateBuffer(renderer, partitions_, sizeof(render::sdsm::Partition) * kMaxPartitions,
                       kSrvAll, L"SDSM.Partitions");
    ok &= CreateBuffer(renderer, zbounds_, sizeof(render::sdsm::ZBoundsScratch),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"SDSM.ZBounds");
    ok &= CreateBuffer(renderer, bounds_, sizeof(render::sdsm::BoundsUint) * kMaxPartitions,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"SDSM.Bounds");
    ok &= CreateBuffer(renderer, frustums_, sizeof(float) * 4ull * kFrustumFloat4s,
                       kSrvAll, L"SDSM.Frustums");
    ok &= CreateBuffer(renderer, prevPartitions_, sizeof(render::sdsm::Partition) * kMaxPartitions,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"SDSM.PrevPartitions");
    // The two GPU-WRITTEN CONSTANT BUFFERS. Their canonical state is the one a root CBV needs --
    // VERTEX_AND_CONSTANT_BUFFER covers a CBV bound from any stage, graphics or compute.
    ok &= CreateBuffer(renderer, viewCBs_, static_cast<UINT64>(render::sdsm::kViewCbStride) * kMaxPartitions,
                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"SDSM.ViewCBs");
    ok &= CreateBuffer(renderer, cullCB_, render::sdsm::kCullCbBytes,
                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"SDSM.CullCB");
    if (!ok) { return false; }

    // The readout ring. One partition set per frame slot; the copy rides the frame that wrote it
    // and is mapped kFrameCount frames later, when the natural per-frame fence has passed.
    if (!readback_)
    {
        D3D12_HEAP_PROPERTIES rb{};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(render::sdsm::Partition) * kMaxPartitions * render::kFrameCount;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(renderer->GetDevice()->CreateCommittedResource(
                &rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(readback_.GetAddressOf()))))
        {
            readback_.Reset(); // non-fatal: the readout is a display path, the render does not need it
        }
        readbackFrame_.fill(0ull);
    }
    return true;
}

bool SdsmShadows::EnsureDescriptors(Renderer* renderer)
{
    if (heap_) { return true; }
    ID3D12Device* dev = renderer->GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kSlotCount;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heap_.GetAddressOf()))) || !heap_)
    {
        heap_.Reset();
        return false;
    }
    const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = heap_->GetCPUDescriptorHandleForHeapStart();
    const auto slot = [&](UINT s) { return D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + static_cast<SIZE_T>(s) * incr }; };

    const auto structuredUav = [&](ID3D12Resource* res, UINT stride, UINT count, UINT s)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = count;
        ud.Buffer.StructureByteStride = stride;
        dev->CreateUnorderedAccessView(res, nullptr, &ud, slot(s));
    };
    const auto structuredSrv = [&](ID3D12Resource* res, UINT stride, UINT count, UINT s)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.FirstElement = 0;
        sd.Buffer.NumElements = count;
        sd.Buffer.StructureByteStride = stride;
        dev->CreateShaderResourceView(res, &sd, slot(s));
    };

    structuredUav(zbounds_.Get(), sizeof(render::sdsm::ZBoundsScratch), 1u, kSlotZBoundsUav);
    structuredUav(bounds_.Get(), sizeof(render::sdsm::BoundsUint), kMaxPartitions, kSlotBoundsUav);
    structuredUav(partitions_.Get(), sizeof(render::sdsm::Partition), kMaxPartitions, kSlotPartitionUav);
    structuredUav(frustums_.Get(), sizeof(float) * 4u, kFrustumFloat4s, kSlotFrustumUav);
    // Both constant blocks are WRITTEN as float4 arrays and READ as cbuffers; only the write side
    // needs a descriptor at all (the read side is a raw address).
    structuredUav(viewCBs_.Get(), sizeof(float) * 4u,
                  (render::sdsm::kViewCbStride / 16u) * kMaxPartitions, kSlotViewCbUav);
    structuredUav(cullCB_.Get(), sizeof(float) * 4u, render::sdsm::kCullCbBytes / 16u, kSlotCullCbUav);
    structuredSrv(partitions_.Get(), sizeof(render::sdsm::Partition), kMaxPartitions, kSlotPartitionSrv);
    structuredSrv(prevPartitions_.Get(), sizeof(render::sdsm::Partition), kMaxPartitions, kSlotPrevPartitionSrv);
    // The cull binds this at t1 as `StructuredBuffer<ViewFrustum>` (16 planes per element), so the
    // SRV has to describe it that way -- a float4 view here would make the shader read every
    // sixteenth plane. The UAV above is the float4 view, because the writer indexes planes.
    structuredSrv(frustums_.Get(), sizeof(float) * 4u * kPlanesPerView,
                  kFrustumViews, kSlotFrustumSrv);

    zboundsUav_ = slot(kSlotZBoundsUav);
    boundsUav_ = slot(kSlotBoundsUav);
    partitionUav_ = slot(kSlotPartitionUav);
    frustumUav_ = slot(kSlotFrustumUav);
    viewCbUav_ = slot(kSlotViewCbUav);
    cullCbUav_ = slot(kSlotCullCbUav);
    partitionSrv_ = slot(kSlotPartitionSrv);
    prevPartitionSrv_ = slot(kSlotPrevPartitionSrv);
    frustumSrv_ = slot(kSlotFrustumSrv);
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS SdsmShadows::ViewCbAddress(std::uint32_t partition) const
{
    if (!viewCBs_ || partition >= kMaxPartitions) { return 0; }
    return viewCBs_.Get()->GetGPUVirtualAddress() +
           static_cast<UINT64>(partition) * render::sdsm::kViewCbStride;
}

D3D12_GPU_VIRTUAL_ADDRESS SdsmShadows::CullCbAddress() const
{
    return cullCB_ ? cullCB_.Get()->GetGPUVirtualAddress() : 0;
}

void SdsmShadows::EnsurePipelines(Renderer* renderer)
{
    if (pipelinesTried_) { return; }
    pipelinesTried_ = true;
    auto* mm = renderer ? renderer->GetMaterialManager() : nullptr;
    if (!mm) { return; }

    const auto make = [&](const char* entry) -> std::shared_ptr<Material>
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/sdsm_analyze_cs.hlsl";
        cd.csEntry = entry;
        std::shared_ptr<Material> m = mm->GetOrCreateCompute(renderer, cd);
        if (!m || !m->GetPipelineState())
        {
            // A PSO that did not build is the whole mode: report it once, by NAME, and leave the
            // handle null so EnsureResources returns false and the graph never registers the
            // SDSM passes. Compiling is not loading -- a shader dxc accepted can still fail here.
            LOG_ERROR(logging::LogCategory::RenderShadow,
                      "sdsm_analyze_cs.hlsl '{}' did not build a PSO; SDSM mode unavailable", entry);
            return {};
        }
        return m;
    };
    {
        // S16: the moments converter. Optional -- a failure leaves MomentsReady() false and the
        // mode keeps its PCF arm, which is also the A/B.
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/sdsm_evsm_cs.hlsl";
        cd.csEntry = "CSMain";
        evsmMat_ = mm->GetOrCreateCompute(renderer, cd);
        if (!evsmMat_ || !evsmMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "sdsm_evsm_cs.hlsl did not build a PSO; EVSM unavailable");
            evsmMat_.reset();
        }
        cd.shaderFile = L"shaders/sdsm_evsm_blur_cs.hlsl";
        evsmBlurMat_ = mm->GetOrCreateCompute(renderer, cd);
        if (!evsmBlurMat_ || !evsmBlurMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "sdsm_evsm_blur_cs.hlsl did not build a PSO; EVSM blur off");
            evsmBlurMat_.reset();
        }
    }
    clearMat_ = make("ClearBounds");
    reduceZMat_ = make("ReduceZBounds");
    logMat_ = make("LogPartitions");
    reduceBoundsMat_ = make("ReduceBounds");
    finalizeMat_ = make("Finalize");
    carryMat_ = make("CarryFrustums");
}

void SdsmShadows::ReleaseResources(Renderer* renderer)
{
    (void)renderer;
    ready_ = false;
    readoutValid_ = false;
    heap_.Reset();
    zboundsUav_ = {};
    boundsUav_ = {};
    partitionUav_ = {};
    frustumUav_ = {};
    viewCbUav_ = {};
    cullCbUav_ = {};
    partitionSrv_ = {};
    prevPartitionSrv_ = {};
    frustumSrv_ = {};
    ReleaseMoments(renderer);
    partitions_.Reset();
    prevPartitions_.Reset();
    zbounds_.Reset();
    bounds_.Reset();
    frustums_.Reset();
    viewCBs_.Reset();
    cullCB_.Reset();
    readback_.Reset();
    readbackFrame_.fill(0ull);
}

// ---------------------------------------------------------------------------------------------
// S16 -- the EVSM moments atlas
// ---------------------------------------------------------------------------------------------

bool SdsmShadows::EnsureMoments(Renderer* renderer, UINT atlasRes)
{
    if (!renderer || !renderer->GetDevice() || atlasRes == 0) { return false; }
    if (moments_ && momentsRes_ == atlasRes) { return true; }
    ReleaseMoments(renderer);

    ID3D12Device* dev = renderer->GetDevice();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = atlasRes;
    desc.Height = atlasRes;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; // the mip chain is a later sub-step (see CsmSampleEvsm's note)
    // R32G32B32A32_FLOAT, and it is not negotiable: the exponents clamp at 42 precisely because
    // exp(42)^2 ~ 2.9e36 already sits just under fp32's ceiling. In fp16 the SECOND moment is Inf
    // for any occluder at all, and every shadow test returns garbage.
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    constexpr D3D12_RESOURCE_STATES kSrvAll =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // CREATED DIRECTLY IN ITS CANONICAL STATE, not in COMMON. Under enhanced barriers a TEXTURE
    // has a LAYOUT, and the graph compiles its first barrier FROM the declared canonical -- so a
    // texture that actually starts in COMMON gets a barrier whose before-layout is a lie
    // (GBV id=1334, caught 2026-09-14). Buffers have no layout and are unaffected, which is why
    // the buffers above may keep COMMON; RenderTargetManager states the same rule for every
    // texture it makes -- created directly in its resting state, so creation == canonical.
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    if (FAILED(render::CreateCommittedTexture(dev, heap, D3D12_HEAP_FLAG_NONE, desc,
                                              kSrvAll, nullptr, tex.GetAddressOf())) || !tex)
    {
        LOG_ERROR(logging::LogCategory::RenderShadow,
                  "SDSM moments atlas {}x{} RGBA32F allocation FAILED; EVSM unavailable", atlasRes, atlasRes);
        return false;
    }
    moments_.Attach(renderer->Declarations(), tex, kSrvAll, kSrvAll, L"SDSM.Moments");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 2;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(momentsHeap_.GetAddressOf()))) || !momentsHeap_)
    {
        ReleaseMoments(renderer);
        return false;
    }
    const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = momentsHeap_->GetCPUDescriptorHandleForHeapStart();
    momentsSrv_ = base;
    momentsUav_ = D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + incr };

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = desc.Format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(moments_.Get(), &sd, momentsSrv_);

    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = desc.Format;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(moments_.Get(), nullptr, &ud, momentsUav_);

    // The blur's intermediate: one TILE. Failure is not fatal -- the blur simply does not run.
    {
        const UINT tile = std::max<UINT>(1u, atlasRes / 2u);
        D3D12_RESOURCE_DESC bd = desc;
        bd.Width = tile;
        bd.Height = tile;
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
        if (SUCCEEDED(render::CreateCommittedTexture(dev, heap, D3D12_HEAP_FLAG_NONE, bd,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                     scratch.GetAddressOf())) && scratch)
        {
            blurScratch_.Attach(renderer->Declarations(), scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"SDSM.MomentsBlurScratch");
            D3D12_DESCRIPTOR_HEAP_DESC sh{};
            sh.NumDescriptors = 2;
            sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sheap;
            if (SUCCEEDED(dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(sheap.GetAddressOf()))) && sheap)
            {
                blurScratchHeap_ = sheap;
                const D3D12_CPU_DESCRIPTOR_HANDLE sbase = sheap->GetCPUDescriptorHandleForHeapStart();
                blurScratchSrv_ = sbase;
                blurScratchUav_ = D3D12_CPU_DESCRIPTOR_HANDLE{ sbase.ptr + incr };
                dev->CreateShaderResourceView(blurScratch_.Get(), &sd, blurScratchSrv_);
                dev->CreateUnorderedAccessView(blurScratch_.Get(), nullptr, &ud, blurScratchUav_);
            }
            else
            {
                blurScratch_.Reset();
            }
        }
    }

    momentsRes_ = atlasRes;
    LOG_INFO(logging::LogCategory::RenderShadow,
             "SDSM EVSM moments atlas {}x{} RGBA32F ({:.1f} MB) + {:.1f} MB blur scratch",
             atlasRes, atlasRes, (static_cast<double>(atlasRes) * atlasRes * 16.0) / (1024.0 * 1024.0),
             blurScratch_ ? (static_cast<double>(atlasRes / 2u) * (atlasRes / 2u) * 16.0) / (1024.0 * 1024.0) : 0.0);
    return true;
}

void SdsmShadows::ReleaseMoments(Renderer* renderer)
{
    (void)renderer;
    // Both sides of the residency say so, not just the allocating one: "the atlas appeared" with no
    // matching "it went away" is indistinguishable from a leak in the session log, and this pair is
    // 67 MB at 2048.
    if (moments_)
    {
        LOG_INFO(logging::LogCategory::RenderShadow, "SDSM EVSM moments atlas released ({}x{})",
                 momentsRes_, momentsRes_);
    }
    blurScratch_.Reset();
    blurScratchHeap_.Reset();
    blurScratchSrv_ = {};
    blurScratchUav_ = {};
    moments_.Reset();
    momentsHeap_.Reset();
    momentsSrv_ = {};
    momentsUav_ = {};
    momentsRes_ = 0;
}

SdsmShadows::MomentsDecisions SdsmShadows::PrepareMomentsPass(RenderGraphPassContext& ctx)
{
    MomentsDecisions dec{};
    if (!ready_ || !MomentsReady() || !evsmMat_ || !ctx.renderer) { return dec; }
    const auto& D = ctx.renderer->GetDeferredForFrame();
    if (D.shadow == nullptr || D.shadowSRV.ptr == 0) { return dec; }
    dec.active = true;

    dec.write = ctx.usePoint ? *ctx.usePoint : 0u;
    // The depth atlas the S15 depth pass just wrote, read by a COMPUTE shader: NON_PIXEL only.
    ctx.Use(D.shadow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(moments_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(partitions_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // The blur ping-pongs the scratch between UAV (the horizontal pass writes it) and
    // NON_PIXEL (the vertical pass reads it). Both states live inside THIS point; the UAV
    // barriers between the dispatches are emitted by the record.
    if (blurScratch_) { ctx.Use(blurScratch_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }

    ctx.NextPoint();
    dec.consume = ctx.usePoint ? *ctx.usePoint : 0u;
    constexpr D3D12_RESOURCE_STATES kSrvAll =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // PIXEL as well as non-pixel: glass samples the moments from a pixel shader, like the atlas.
    ctx.Use(moments_.Get(), kSrvAll);
    return dec;
}

void SdsmShadows::RecordMoments(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                const MomentsDecisions& dec)
{
    if (!renderer || !cl || !dec.active) { return; }
    // Points first, work second -- the rule the barrier comparator enforces.
    renderer->EmitPoint(cl, dec.write);

    const auto& D = renderer->GetDeferredForFrame();
    if (evsmMat_ && D.shadowSRV.ptr != 0 && momentsUav_.ptr != 0 && momentsRes_ > 0)
    {
        struct EvsmCB { std::uint32_t atlasRes, tileRes, partitions, pad; };
        const EvsmCB c{ momentsRes_, momentsRes_ / 2u,
                        std::min<std::uint32_t>(cb_.partitions, render::sdsm::kMaxPartitions), 0u };
        RecordComputeDispatch(renderer, cl, evsmMat_.get(), static_cast<UINT>(sizeof(EvsmCB)),
            [&](std::uint8_t* dst) { std::memcpy(dst, &c, sizeof(c)); },
            { D.shadowSRV, partitionSrv_ },
            { momentsUav_ },
            D3D12_GPU_DESCRIPTOR_HANDLE{},
            momentsRes_, momentsRes_,
            moments_.Get());

        // The separable box, per partition, in light space. Two dispatches each: the tile out into
        // the scratch (horizontal), the scratch back into the tile (vertical). Tile at a time
        // because a tap must never leave its own partition, and the whole atlas holds four.
        const float softening = render::sdsm::g_evsmBlur;
        const UINT tile = momentsRes_ / 2u;
        if (evsmBlurMat_ && blurScratch_ && softening > 0.0f && tile > 0u)
        {
            // Width in TEXELS: a fraction of the partition, and the partition's box maps exactly
            // onto the tile's content, so the fraction times the tile IS the texel count. Capped,
            // and floored at 1 -- below one texel the box is the identity and the passes are waste.
            const float widthTexels = std::min(softening * static_cast<float>(tile),
                                               render::sdsm::g_evsmBlurMaxTexels);
            if (widthTexels > 1.0f)
            {
                struct BlurCB
                {
                    std::int32_t srcOrigin[2], dstOrigin[2], tileSize[2];
                    std::uint32_t dimension;
                    float filterTexels;
                };
                const std::uint32_t parts = std::min<std::uint32_t>(cb_.partitions, render::sdsm::kMaxPartitions);
                for (std::uint32_t p = 0; p < parts; ++p)
                {
                    const std::int32_t ox = static_cast<std::int32_t>((p % 2u) * tile);
                    const std::int32_t oy = static_cast<std::int32_t>((p / 2u) * tile);
                    const std::int32_t t = static_cast<std::int32_t>(tile);

                    BlurCB h{ { ox, oy }, { 0, 0 }, { t, t }, 0u, widthTexels };
                    RecordComputeDispatch(renderer, cl, evsmBlurMat_.get(), static_cast<UINT>(sizeof(BlurCB)),
                        [&](std::uint8_t* dst) { std::memcpy(dst, &h, sizeof(h)); },
                        {}, { momentsUav_, blurScratchUav_ }, D3D12_GPU_DESCRIPTOR_HANDLE{},
                        tile, tile, blurScratch_.Get());

                    BlurCB v{ { 0, 0 }, { ox, oy }, { t, t }, 1u, widthTexels };
                    RecordComputeDispatch(renderer, cl, evsmBlurMat_.get(), static_cast<UINT>(sizeof(BlurCB)),
                        [&](std::uint8_t* dst) { std::memcpy(dst, &v, sizeof(v)); },
                        {}, { blurScratchUav_, momentsUav_ }, D3D12_GPU_DESCRIPTOR_HANDLE{},
                        tile, tile, moments_.Get());
                }
            }
        }
    }

    renderer->EmitPoint(cl, dec.consume);
}

void SdsmShadows::SetFrameParams(const render::sdsm::AnalyzeConstants& cb)
{
    cb_ = cb;
}

// ---------------------------------------------------------------------------------------------
// The analyze pass
// ---------------------------------------------------------------------------------------------

SdsmShadows::AnalyzeDecisions SdsmShadows::PrepareAnalyzePass(RenderGraphPassContext& ctx)
{
    AnalyzeDecisions dec{};
    if (!ready_ || !ctx.renderer) { return dec; }
    if (cb_.depthWidth == 0u || cb_.depthHeight == 0u) { return dec; }
    dec.active = true;

    // FIRST, before anything writes them: last frame's partitions are copied aside, because the
    // occlusion test has to project a box with the matrix the pyramid was RENDERED with.
    dec.prevCopy = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(partitions_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.Use(prevPartitions_.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    ctx.NextPoint();

    // The camera depth, read by a COMPUTE shader: NON_PIXEL only. The G-buffer leaves it in
    // DEPTH_WRITE, so this pass is the one that hands it over -- the same shape Main_VsmPageRequest
    // has, and for the same reason (D7: the PIXEL bit would be illegal for a compute-queue reader).
    ctx.Use(ctx.renderer->GetDeferredForFrame().depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    dec.write = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(prevPartitions_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(zbounds_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(bounds_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(partitions_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(frustums_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(viewCBs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(cullCB_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The readout copy, declared INSIDE the pass so the partition buffer's state chain is
    // UAV -> COPY_SOURCE -> SRV rather than UAV -> SRV with a copy squeezed in unannounced.
    dec.readback = readback_ != nullptr;
    if (dec.readback)
    {
        ctx.NextPoint();
        dec.readCopy = ctx.usePoint ? *ctx.usePoint : 0u;
        ctx.Use(partitions_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    ctx.NextPoint();
    dec.consume = ctx.usePoint ? *ctx.usePoint : 0u;
    constexpr D3D12_RESOURCE_STATES kSrvAll =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ctx.Use(partitions_.Get(), kSrvAll);
    ctx.Use(frustums_.Get(), kSrvAll);
    // The two GPU-written constant buffers go to the state a root CBV needs.
    ctx.Use(viewCBs_.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    ctx.Use(cullCB_.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // Cross-frame state, committed in the (serial) builder like every other decision of this
    // shape: which frame's partitions the readout slot about to be written holds.
    if (dec.readback)
    {
        const UINT f = ctx.renderer->GetCurrentFrameIndex();
        if (f < render::kFrameCount) { readbackFrame_[f] = ctx.renderer->GetTotalFrameNumber(); }
    }
    return dec;
}

void SdsmShadows::RecordAnalyze(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                const AnalyzeDecisions& dec)
{
    if (!dec.active || !renderer || !cl || !ready_) { return; }

    const auto& D = renderer->GetDeferredForFrame();
    const D3D12_CPU_DESCRIPTOR_HANDLE depthSrv = D.depthSRV;

    // EVERY DECLARED POINT IS EMITTED, WHATEVER THE DATA SAYS. A body that early-returns past a
    // point it declared leaves that point's compiled barriers unrecorded, and the compile's model
    // then runs one transition ahead of the GPU -- invisible until something else reads the
    // resource in the wrong state. (The comparator caught exactly this shape in Main_SdsmCull.)
    // So a missing depth SRV skips the DISPATCHES below, not the markers.
    const bool haveDepth = depthSrv.ptr != 0;

    // Last frame's partitions aside, before ClearBounds touches anything.
    renderer->EmitPoint(cl, dec.prevCopy);
    cl->CopyBufferRegion(prevPartitions_.Get(), 0, partitions_.Get(), 0,
                         sizeof(render::sdsm::Partition) * kMaxPartitions);

    renderer->EmitPoint(cl, dec.write);

    const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};
    const UINT cbSize = static_cast<UINT>(sizeof(render::sdsm::AnalyzeConstants));
    const auto writeCB = [this](std::uint8_t* dst) { std::memcpy(dst, &cb_, sizeof(cb_)); };

    // Every dispatch binds the SAME two tables: the root signature declares both for all six entry
    // points, and Material::Bind reports an SRV table left unbound whether the shader reads it or
    // not. One staging call per dispatch is the price of not tracking which entry uses what.
    // t1 is the CPU ring's region for this frame. When the caller has not set it (a frame with no
    // ShadowGpuData) the depth SRV stands in: the table must have no hole, and CarryFrustums is
    // the only reader -- it is dispatched with zero groups in that case.
    const D3D12_CPU_DESCRIPTOR_HANDLE srcFrustums = (srcFrustumSrv_.ptr != 0) ? srcFrustumSrv_ : depthSrv;
    const D3D12_CPU_DESCRIPTOR_HANDLE srvs[3] = { depthSrv, srcFrustums, prevPartitionSrv_ };
    const D3D12_CPU_DESCRIPTOR_HANDLE uavs[6] = { zboundsUav_, boundsUav_, partitionUav_, frustumUav_,
                                                  viewCbUav_, cullCbUav_ };

    // A dispatch of exactly `groups` thread groups. RecordComputeDispatch divides by 8, which is
    // right for its own 8x8 shaders and wrong for all five of these (16x16, 16x8, 4x1) -- so the
    // group count is stated, not derived.
    const auto dispatch = [&](Material* mat, UINT gx, UINT gy)
    {
        if (!haveDepth || !mat || gx == 0u || gy == 0u) { return; }
        auto cb = renderer->GetFrameResource()->AllocDynamic(cbSize, render::kConstantBufferAlignment);
        if (!cb.cpu) { return; }
        writeCB(static_cast<std::uint8_t*>(cb.cpu));
        auto h = renderer->GetRenderContextPool()->Acquire();
        RenderContext& rc = h.ref();
        rc.cbv[0] = cb.gpu;
        rc.srvTable[0] = renderer->StageSrvUavTable({ srvs[0], srvs[1], srvs[2] }).gpu;
        rc.uavTable[0] = renderer->StageSrvUavTable({ uavs[0], uavs[1], uavs[2], uavs[3],
                                                      uavs[4], uavs[5] }).gpu;
        rc.samplerTable[0] = noSampler;
        mat->Bind(cl, rc);
        cl->Dispatch(gx, gy, 1);
    };

    const UINT tile = std::max<UINT>(1u, cb_.reduceTileDim);
    const UINT tilesX = (cb_.depthWidth + tile - 1u) / tile;
    const UINT tilesY = (cb_.depthHeight + tile - 1u) / tile;

    // 1. clear the two accumulators.
    dispatch(clearMat_.get(), 1u, 1u);
    renderer->UAVBarrier(cl, zbounds_.Get());
    renderer->UAVBarrier(cl, bounds_.Get());

    // 2. reduce the depth buffer to one [minZ, maxZ].
    dispatch(reduceZMat_.get(), tilesX, tilesY);
    renderer->UAVBarrier(cl, zbounds_.Get());

    // 3. split that range logarithmically into the partitions' intervals.
    dispatch(logMat_.get(), 1u, 1u);
    renderer->UAVBarrier(cl, partitions_.Get());

    // 4. reduce the light-space bounds of each partition's own samples.
    dispatch(reduceBoundsMat_.get(), tilesX, tilesY);
    renderer->UAVBarrier(cl, bounds_.Get());

    // 5. bounds -> box, matrix, atlas rect, bias, cull planes.
    dispatch(finalizeMat_.get(), 1u, 1u);
    renderer->UAVBarrier(cl, partitions_.Get());
    renderer->UAVBarrier(cl, frustums_.Get());
    renderer->UAVBarrier(cl, viewCBs_.Get());
    renderer->UAVBarrier(cl, cullCB_.Get());

    // 6. carry the CPU-owned view slots (spot, point faces, clipmap, camera) into the same buffer.
    // Clamped to what the buffer actually holds: the count comes from ShadowGpuData, and a future
    // slot added there must not silently run off the end of this one.
    {
        const UINT views = std::min<UINT>(cb_.viewFrustumCount, kFrustumViews);
        if (srcFrustumSrv_.ptr != 0 && views > kMaxPartitions)
        {
            const UINT planes = (views - kMaxPartitions) * kPlanesPerView;
            dispatch(carryMat_.get(), (planes + 63u) / 64u, 1u);
            renderer->UAVBarrier(cl, frustums_.Get());
        }
    }

    if (dec.readback && readback_)
    {
        renderer->EmitPoint(cl, dec.readCopy);
        const UINT f = renderer->GetCurrentFrameIndex();
        const UINT64 bytes = sizeof(render::sdsm::Partition) * kMaxPartitions;
        cl->CopyBufferRegion(readback_.Get(), static_cast<UINT64>(f) * bytes,
                             partitions_.Get(), 0, bytes);
    }

    renderer->EmitPoint(cl, dec.consume);
}

// ---------------------------------------------------------------------------------------------
// Readout
// ---------------------------------------------------------------------------------------------

void SdsmShadows::PollReadout(Renderer* renderer)
{
    if (!readback_ || !renderer) { return; }
    const std::uint64_t now = renderer->GetTotalFrameNumber();
    if (now < render::kFrameCount) { return; }
    const std::uint64_t want = now - render::kFrameCount;

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        if (readbackFrame_[f] != want || want == 0ull) { continue; }
        const UINT64 bytes = sizeof(render::sdsm::Partition) * kMaxPartitions;
        D3D12_RANGE range{ static_cast<SIZE_T>(f) * static_cast<SIZE_T>(bytes),
                           static_cast<SIZE_T>((f + 1u) * bytes) };
        void* mapped = nullptr;
        if (SUCCEEDED(readback_->Map(0, &range, &mapped)) && mapped)
        {
            std::memcpy(readout_.data(),
                        static_cast<const std::uint8_t*>(mapped) + range.Begin,
                        static_cast<size_t>(bytes));
            D3D12_RANGE none{ 0, 0 };
            readback_->Unmap(0, &none);
            readoutValid_ = true;
        }
        readbackFrame_[f] = 0ull; // consumed
        break;
    }
}
