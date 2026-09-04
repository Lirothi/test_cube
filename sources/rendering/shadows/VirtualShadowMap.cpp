#include "rendering/shadows/VirtualShadowMap.h"
#include "core/logging/Log.h"
#include "rendering/core/TextureCreate.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h" // GPU_SCOPE(kVsmPageSetup): per-page cull sub-scope
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h" // PrepareRequestPass takes a RenderGraphPassContext&
#include "rendering/core/ComputeDispatch.h"
#include "rendering/shadows/ShadowGpuData.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/meshes/LodSelect.h" // render::kMaxShadowLods (per-view shadow LOD table)
#include "rendering/shadows/ShadowSettings.h"
#include "core/diagnostics/ArtifactWriter.h" // logs/vsm_pages.log (g_logPageStats file mirror)
#include "vfx/WindState.h" // W5: wind params copied into each page's shadow view CB

void VirtualShadowMap::EnsureResources(Renderer* renderer)
{
    if (IsAllocated()) { return; }
    if (!renderer || !renderer->GetDevice()) { return; }
    ID3D12Device* dev = renderer->GetDevice();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // --- Physical page pool: one D32 depth atlas (R32_TYPELESS -> D32_FLOAT DSV + R32_FLOAT SRV).
    // 32-bit ON PURPOSE (was D16, 2026-08-21): float depth makes quantization negligible
    // (2^-24 of the range vs D16's 1.5e-5), which is what lets the directional constant depth
    // bias sit at ~0 with the receiver-plane bias doing the per-tap work — UE's configuration
    // (their pool is R32 too, via atomics). Cost: 32 -> 64 MB. The Legacy CSM/spot/point atlases
    // stay D16 — they keep their own tuned constant biases. PERSISTENT; the pool IS the cache. ---
    {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = vsm::kPoolTexels;
        rd.Height = vsm::kPoolTexels;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_TYPELESS;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;

        if (FAILED(render::CreateCommittedTexture(dev, heap, D3D12_HEAP_FLAG_NONE, rd,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &cv, pagePool_.GetAddressOfForCreate())) || !pagePool_)
        {
            pagePool_.Reset();
            return;
        }
        pagePool_.DeclareCreated(renderer->Declarations(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"VSM.PagePool");
    }

    // --- Page table: StructuredBuffer<uint>[kPageTableEntries] (virtual page -> packed
    // physical page + resident flag). UAV (written by Step 20) + SRV (read by Steps 21/22). ---
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(vsm::kPageTableEntries) * sizeof(std::uint32_t);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        // P12.2: COMMON at creation. Buffers are ALWAYS created in COMMON by D3D12 (GBV id=1328);
        // the resting state is declared below, and first use promotes into it.
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(pageTable_.GetAddressOfForCreate()))) || !pageTable_)
        {
            pageTable_.Reset();
            pagePool_.Reset();
            return;
        }
        pageTable_.DeclareCreated(renderer->Declarations(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"VSM.PageTable");
    }

    // --- Step 19 page-request bitfield: DEFAULT-heap UAV, 1 bit per virtual page. ---
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(vsm::kRequestWords) * sizeof(std::uint32_t);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(requestBuffer_.GetAddressOfForCreate()))) || !requestBuffer_)
        {
            requestBuffer_.Reset(); pageTable_.Reset(); pagePool_.Reset();
            return;
        }
        requestBuffer_.DeclareCreated(renderer->Declarations(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"VSM.PageRequest");
    }

    // --- DSV for the pool (render pages into it, Step 22). ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(dsvHeap_.GetAddressOf()))) || !dsvHeap_)
        {
            pagePool_.Reset(); pageTable_.Reset(); dsvHeap_.Reset();
            return;
        }
        poolDsv_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(pagePool_.Get(), &dsv, poolDsv_);
    }

    // --- Pool SRV + page-table SRV/UAV (one non-shader-visible heap; staged into the frame
    // heap by the passes that use them). ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 4; // poolSrv, pageTableSrv, pageTableUav, requestUav
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(srvUavHeap_.GetAddressOf()))) || !srvUavHeap_)
        {
            pagePool_.Reset(); pageTable_.Reset(); requestBuffer_.Reset(); dsvHeap_.Reset(); srvUavHeap_.Reset();
            return;
        }
        const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE base = srvUavHeap_->GetCPUDescriptorHandleForHeapStart();
        poolSrv_ = base;
        pageTableSrv_ = { base.ptr + static_cast<SIZE_T>(1) * incr };
        pageTableUav_ = { base.ptr + static_cast<SIZE_T>(2) * incr };
        requestUav_ = { base.ptr + static_cast<SIZE_T>(3) * incr };

        D3D12_SHADER_RESOURCE_VIEW_DESC poolSd{};
        poolSd.Format = DXGI_FORMAT_R32_FLOAT;
        poolSd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        poolSd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        poolSd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(pagePool_.Get(), &poolSd, poolSrv_);

        D3D12_SHADER_RESOURCE_VIEW_DESC ptSd{};
        ptSd.Format = DXGI_FORMAT_UNKNOWN;
        ptSd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        ptSd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ptSd.Buffer.FirstElement = 0;
        ptSd.Buffer.NumElements = vsm::kPageTableEntries;
        ptSd.Buffer.StructureByteStride = sizeof(std::uint32_t);
        dev->CreateShaderResourceView(pageTable_.Get(), &ptSd, pageTableSrv_);

        D3D12_UNORDERED_ACCESS_VIEW_DESC ptUd{};
        ptUd.Format = DXGI_FORMAT_UNKNOWN;
        ptUd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ptUd.Buffer.FirstElement = 0;
        ptUd.Buffer.NumElements = vsm::kPageTableEntries;
        ptUd.Buffer.StructureByteStride = sizeof(std::uint32_t);
        dev->CreateUnorderedAccessView(pageTable_.Get(), nullptr, &ptUd, pageTableUav_);

        D3D12_UNORDERED_ACCESS_VIEW_DESC rqUd{};
        rqUd.Format = DXGI_FORMAT_UNKNOWN;
        rqUd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        rqUd.Buffer.FirstElement = 0;
        rqUd.Buffer.NumElements = vsm::kRequestWords;
        rqUd.Buffer.StructureByteStride = sizeof(std::uint32_t);
        dev->CreateUnorderedAccessView(requestBuffer_.Get(), nullptr, &rqUd, requestUav_);
    }

    // Step 20: the persistent page-allocation state (owner/last-frame/free-list/counters).
    EnsureAllocResources(renderer);

    char buf[192];
    std::snprintf(buf, sizeof(buf),
        "[VSM] allocated: pool %ux%u D16 (%u pages), page table %u entries (%.2f MB pool).\n",
        vsm::kPoolTexels, vsm::kPoolTexels, vsm::kPoolPageCount, vsm::kPageTableEntries,
        (static_cast<double>(vsm::kPoolTexels) * vsm::kPoolTexels * 2.0) / (1024.0 * 1024.0));
    logging::WriteRaw(logging::LogLevel::Info, logging::LogCategory::RenderShadow, buf);
}

namespace
{
    // A persistent DEFAULT-heap RWStructuredBuffer<uint>[numUints] (ALLOW_UNORDERED_ACCESS).
    // Used for the Step-20 allocation state buffers.
    //
    // P12.2: this used to take the resting state and pass it as InitialState, on the belief that a
    // buffer is "created directly there". It is not -- D3D12 creates EVERY buffer in COMMON and
    // ignores the request (GBV id=1328), so the parameter described something that never happened.
    // It is gone rather than defaulted, because a parameter the driver discards is a parameter that
    // misleads its next reader. The resting state is still declared, at the Attach on each caller,
    // which is the fact the canonical registry actually stores; state promotion makes the first use
    // correct from COMMON.
    Microsoft::WRL::ComPtr<ID3D12Resource> CreateUavUintBuffer(ID3D12Device* dev,
        std::uint32_t numUints, const wchar_t* name)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(numUints) * sizeof(std::uint32_t);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(res.GetAddressOf()))) || !res)
        {
            return nullptr;
        }
        res->SetName(name);
        return res;
    }
}

void VirtualShadowMap::ReleaseResources()
{
    // Step 24b: free every VSM GPU allocation (pool + page table + request/alloc/render buffers +
    // their descriptor heaps + the readback ring) when switching to Legacy mode, so nothing VSM is
    // resident. MUST be called at GPU idle (the switch orchestration waits first) — a freed resource
    // referenced by an in-flight frame would fault. The compute PSOs (Materials) are KEPT: they cost
    // no pool memory and are reused if VSM is re-enabled. IsAllocated() goes false → all VSM passes +
    // sampling early-out. A later EnsureResources rebuilds everything (idempotent, guards cleared).
    pagePool_.Reset();
    pageTable_.Reset();
    dsvHeap_.Reset();
    srvUavHeap_.Reset();
    requestBuffer_.Reset();

    physOwner_.Reset();
    physLastFrame_.Reset();
    freeList_.Reset();
    needsRender_.Reset();
    allocCounters_.Reset();
    allocUavHeap_.Reset();
    allocInitialized_ = false; // fresh alloc buffers need the one-shot page-table/owner init again

    pageProj_.Reset();
    pageDrawArgs_.Reset();
    renderHeap_.Reset();
    renderGroups_ = 0;
    cachedRung0Args_ = nullptr;
    // S5b.2: the pool pyramid and the pass-B set go with the pool.
    vsmHzb_.Reset();
    prevPageTable_.Reset();
    deferredPairs_.Reset();
    hzbCounters_.Reset();
    pageGroupCountB_.Reset();
    pageDrawArgsB_.Reset();
    pageArgCountB_.Reset();
    hzbHeap_.Reset();
    hzbSrv_ = prevPageTableSrv_ = deferredPairsUav_ = deferredPairsSrv_ = hzbCountersUav_ = {};
    pageGroupCountBUav_ = pageGroupCountBSrv_ = pageDrawArgsBUav_ = pageArgCountBUav_ = {};
    for (UINT m = 0; m < kHzbMips; ++m) { hzbMipUav_[m] = {}; }
    deferredCap_ = 0;
    scatterGroupsB_ = 0;
    hzbInvalidate_ = true;
    hzbThisFrame_ = false;
    lastRenderFrame_ = 0;
    hzbLastBuildFrame_ = 0;
    for (UINT i = 0; i < render::kFrameCount; ++i)
    {
        hzbReadback_[i].Reset();
        hzbReadbackPtr_[i] = nullptr;
        hzbReadbackFrame_[i] = 0;
    }
    hzbStats_.fill(0);

    for (UINT i = 0; i < render::kFrameCount; ++i)
    {
        residentReadback_[i].Reset(); // releasing implicitly unmaps the persistent map
        residentReadbackPtr_[i] = nullptr;
        residentReadbackValid_[i] = false;
    }

    debugReadback_.Reset();
    debugReadbackState_ = 0;
    debugReadbackFrame_ = 0;
    debugReadbackDoneFrame_ = 0;
    debugLoggedFrame_ = 0;
    stats_ = DebugStats{};             // dev-window readout invalid until VSM is active again
    physOwnerSnapshot_.clear();

    // Zero the cached CPU descriptor handles (their heaps are gone) so nothing stale is bound.
    poolDsv_ = poolSrv_ = pageTableSrv_ = pageTableUav_ = {};
    requestUav_ = {};
    physOwnerUav_ = physLastFrameUav_ = freeListUav_ = needsRenderUav_ = allocCountersUav_ = {};
    physOwnerSrv_ = rung0ArgsSrv_ = pageDrawArgsUav_ = pageProjUav_ = {};
}

void VirtualShadowMap::EnsureAllocResources(Renderer* renderer)
{
    if (allocCounters_) { return; } // already created (persistent)
    if (!renderer || !renderer->GetDevice()) { return; }
    ID3D12Device* dev = renderer->GetDevice();

    // Same story as PageProj: the multi-draw path leaves this in COPY_SOURCE. Combined.
    physOwner_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.PhysOwner"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr);
    physLastFrame_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.PhysLastFrame"), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    freeList_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.FreeList"), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    needsRender_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.NeedsRender"), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    allocCounters_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, 4u, L"VSM.AllocCounters"), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    if (!physOwner_ || !physLastFrame_ || !freeList_ || !needsRender_ || !allocCounters_)
    {
        physOwner_.Reset(); physLastFrame_.Reset(); freeList_.Reset();
        needsRender_.Reset(); allocCounters_.Reset();
        return;
    }

    // One non-shader-visible heap for the 5 alloc-state UAVs (staged into the frame heap per
    // dispatch alongside the existing pageTable/request UAVs).
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 5;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(allocUavHeap_.GetAddressOf()))) || !allocUavHeap_)
    {
        allocUavHeap_.Reset();
        return;
    }
    const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = allocUavHeap_->GetCPUDescriptorHandleForHeapStart();
    auto makeUav = [&](ID3D12Resource* res, std::uint32_t numUints, UINT slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h{ base.ptr + static_cast<SIZE_T>(slot) * incr };
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = numUints;
        ud.Buffer.StructureByteStride = sizeof(std::uint32_t);
        dev->CreateUnorderedAccessView(res, nullptr, &ud, h);
        return h;
    };
    physOwnerUav_     = makeUav(physOwner_.Get(),     vsm::kPoolPageCount, 0);
    physLastFrameUav_ = makeUav(physLastFrame_.Get(), vsm::kPoolPageCount, 1);
    freeListUav_      = makeUav(freeList_.Get(),      vsm::kPoolPageCount, 2);
    needsRenderUav_   = makeUav(needsRender_.Get(),   vsm::kPoolPageCount, 3);
    allocCountersUav_ = makeUav(allocCounters_.Get(), 4u,                  4);
}

void VirtualShadowMap::EnsureShaderResources(Renderer* renderer)
{
    if (shaderResourcesTried_) { return; }
    shaderResourcesTried_ = true;
    if (!renderer || !renderer->GetMaterialManager()) { return; }

    auto* mm = renderer->GetMaterialManager();
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/vsm_page_request_clear_cs.hlsl";
        cd.csEntry = "CSMain";
        pageRequestClearMat_ = mm->GetOrCreateCompute(renderer, cd);
    }
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/vsm_page_request_cs.hlsl";
        cd.csEntry = "CSMain";
        pageRequestMat_ = mm->GetOrCreateCompute(renderer, cd);
    }
    // Step 20: page-allocation compute PSOs.
    auto makeCompute = [&](const wchar_t* file)
    {
        Material::ComputeDesc cd{};
        cd.shaderFile = file;
        cd.csEntry = "CSMain";
        return mm->GetOrCreateCompute(renderer, cd);
    };
    allocInitMat_  = makeCompute(L"shaders/vsm_page_alloc_init_cs.hlsl");
    allocTouchMat_ = makeCompute(L"shaders/vsm_page_alloc_touch_cs.hlsl");
    allocFreeMat_  = makeCompute(L"shaders/vsm_page_alloc_freelist_cs.hlsl");
    allocMapMat_   = makeCompute(L"shaders/vsm_page_alloc_map_cs.hlsl");
    allocPropagateMat_ = makeCompute(L"shaders/vsm_page_propagate_cs.hlsl");
    pageSetupMat_  = makeCompute(L"shaders/vsm_page_setup_cs.hlsl");
    // Spatial scatter cull (directional clipmap). Optional: if either fails to compile the setup
    // shader still culls those pages the brute-force way, so shadows stay correct (just slower).
    pageScatterClearMat_ = makeCompute(L"shaders/vsm_page_scatter_clear_cs.hlsl");
    pageScatterMat_      = makeCompute(L"shaders/vsm_page_scatter_cs.hlsl");
    // Occlusion plan S5b.2: the pool pyramid + the deferred-pair post cull. Optional -- without
    // them the scatter defers nothing and every page draws single-pass as before.
    hzbBuildMat_ = makeCompute(L"shaders/vsm_hzb_build_cs.hlsl");
    hzbPostMat_  = makeCompute(L"shaders/vsm_hzb_post_cs.hlsl");

    // Page cache: the gated depth-clear graphics PSO (depth-only, ALWAYS-write z=1.0, no vertex input;
    // VS drives from SV_VertexID/SV_InstanceID). Optional — a failure just falls back to whole-pool clear.
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/vsm_page_clear.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = ""; // no IA input
        gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.numRT = 0;
        gd.dsvFormat = DXGI_FORMAT_D32_FLOAT; // the pool's format (see EnsureResources)
        gd.depth.DepthEnable = TRUE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        gd.blend.RenderTarget[0].BlendEnable = FALSE;
        pageClearMat_ = mm->GetOrCreateGraphics(renderer, gd);
    }

    auto ok = [](const std::shared_ptr<Material>& m) { return m && m->GetPipelineState(); };
    if (!ok(pageRequestClearMat_) || !ok(pageRequestMat_))
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "VSM page-request PSO creation FAILED (shader compile?)");
        pageRequestClearMat_.reset();
        pageRequestMat_.reset();
    }
    else if (!ok(allocInitMat_) || !ok(allocTouchMat_) || !ok(allocFreeMat_) || !ok(allocMapMat_) || !ok(pageSetupMat_))
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "VSM page-alloc/setup PSO creation FAILED (shader compile?)");
        allocInitMat_.reset(); allocTouchMat_.reset(); allocFreeMat_.reset(); allocMapMat_.reset(); pageSetupMat_.reset();
    }
    else
    {
        LOG_INFO(logging::LogCategory::RenderShadow, "VSM page-request + page-alloc + page-setup shaders ready");
    }
    if (!ok(pageClearMat_))
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "VSM page-clear PSO creation FAILED (page cache off -> whole-pool clear)");
        pageClearMat_.reset();
    }
    if (!ok(hzbBuildMat_) || !ok(hzbPostMat_))
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "VSM HZB build/post PSO creation FAILED (light-space two-pass occlusion off)");
        hzbBuildMat_.reset();
        hzbPostMat_.reset();
    }
}

VirtualShadowMap::PageRequestPoints VirtualShadowMap::PrepareRequestPass(
    RenderGraphPassContext& ctx) const
{
    // pass-flow S3c: declares in body order AND captures each point's absolute index — the
    // record bodies emit these as EmitPoint markers. The AddPass2 builder has already gated on
    // VsmActive/IsAllocated and registered the camera-depth read into the CURRENT point, which
    // is why `base` is captured before the first Use here.
    PageRequestPoints pts;
    pts.base = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(requestBuffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // S5b.2: last frame's mapping, copied aside before the allocation rewrites the table. Only
    // the knob and the buffer gate it -- whether the pyramid may be tested against is decided
    // by the render pass; a snapshot nobody reads is 55 KB.
    pts.hzbSnapshot = vsm::g_hzbCull && prevPageTable_ && pageTable_;
    if (pts.hzbSnapshot)
    {
        ctx.NextPoint();
        pts.snapshotCopy = ctx.usePoint ? *ctx.usePoint : 0u;
        ctx.Use(pageTable_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        ctx.Use(prevPageTable_.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    }
    ctx.NextPoint();
    pts.alloc = ctx.usePoint ? *ctx.usePoint : 0u;
    if (pts.hzbSnapshot) { ctx.Use(prevPageTable_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); }
    ctx.Use(pageTable_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(physOwner_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(physLastFrame_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(freeList_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(needsRender_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(allocCounters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The debug readback runs on exactly one frame; the decision is taken HERE and travels in
    // `pts.readback`, so declaration and record cannot evaluate it twice.
    pts.readback = WillRecordDebugReadback(ctx.renderer);
    if (!pts.readback) { return pts; }
    ctx.NextPoint();
    pts.readbackCopy = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(requestBuffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.Use(allocCounters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.Use(physOwner_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE);
    // ...and back to canonical — RecordDebugReadback no longer parks them in COPY_SOURCE.
    ctx.NextPoint();
    pts.readbackRestore = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(requestBuffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(allocCounters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(physOwner_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE);
    return pts;
}

// The exact gate RecordDebugReadback uses. Shared so Prepare and Record cannot drift apart —
// duplicating the condition is what makes over-registration creep back in.
bool VirtualShadowMap::WillRecordDebugReadback(Renderer* renderer) const
{
    return renderer != nullptr && debugReadbackState_ == 0 &&
           renderer->GetTotalFrameNumber() > render::kFrameCount;
}

VirtualShadowMap::PageRenderDecisions VirtualShadowMap::ComputePageRenderDecisions(
    ShadowGpuData* shadowGpu, const vfx::WindState* wind) const
{
    PageRenderDecisions d;
    if (!shadowGpu) { return d; }

    d.caching = vsm::g_pageCaching && pageClearMat_ && pageClearMat_->GetPipelineState() && perPageDirtySrv_.ptr != 0;
    const bool windAnimating = wind && wind->swayAmplitude > 0.0f && shadowGpu->HasWindCasters();
    // Wind is deliberately NOT part of forceAll any more: it dirties only the pages that
    // ANIMATE it (near clipmap levels + locals; gWindDirtyMaxLevel in the setup CB) — the far
    // levels render rigid casters and cache. A caster-LOD change DOES force everything:
    // cached pages hold geometry at the old receiver LOD, and RefreshCasterLods flags it.
    // MoverCount() is NOT here any more (Step 4, docs/vsm_page_caching_plan.md). One moving caster
    // used to dirty EVERY resident page -- traced at ~195 ms a frame against a 0.38 ms baseline
    // while a mesh was dragged in the editor. A mover now publishes its own dynamic bit in the
    // caster meta (ShadowGpuData::UpdateForFrame), and the setup CS's existing `dynamicOverlap`
    // test dirties exactly the pages it touches.
    // The rest stay: no caching means everything is dirty by definition, warmup has nothing to
    // reuse, and a caster-LOD change invalidates cached GEOMETRY rather than a position.
    d.forceAll = (!d.caching || cacheWarmup_ ||
                  shadowGpu->ConsumeCasterLodsChanged()) ? 1u : 0u;
    d.windDirtyMaxLevel = windAnimating
        ? std::min(vsm::g_windAnimateMaxLevel, vsm::kNumClipmapLevels) : 0u;

    d.useMega = shadowGpu->MegaReady() &&
                shadowGpu->MegaVertexBuffer() && shadowGpu->MegaIndexBuffer();

    const std::uint32_t activeCasters = shadowGpu->ActiveCasterCount();
    Material* pageMat = shadowGpu->IndirectShadowPageMaterial();
    d.singleDraw = vsm::g_pageDrawSingle && d.useMega &&
                   activeCasters < (1u << vsm::kPageIdShift) && pageProjSrv_.ptr != 0 &&
                   pageMat && pageMat->GetPipelineState();

    d.compactArgs = d.singleDraw && vsm::g_pageDrawCompact &&
                    pageArgCount_ && pageArgCountUav_.ptr != 0;

    d.scatterActive = pageScatterMat_ && pageScatterMat_->GetPipelineState() &&
                      pageScatterClearMat_ && pageScatterClearMat_->GetPipelineState() &&
                      pageGroupCountUav_.ptr != 0 && pageScatterDynUav_.ptr != 0 &&
                      pageGroupCountSrv_.ptr != 0 && pageScatterDynSrv_.ptr != 0 &&
                      shadowGpu->PerGroupSrv().ptr != 0;
    d.scatterLocals = d.scatterActive && vsm::g_scatterLocalViews;

    // S5b.2: the two-pass light occlusion needs the scatter (the test lives in it), the single
    // draw (pass B is one more ExecuteIndirect over the B args), both PSOs and the whole B set.
    d.hzb = vsm::g_hzbCull && d.scatterActive && d.singleDraw &&
            hzbBuildMat_ && hzbBuildMat_->GetPipelineState() && hzbPostMat_ && hzbPostMat_->GetPipelineState() &&
            vsmHzb_ && prevPageTable_ && deferredPairs_ && hzbCounters_ && pageGroupCountB_ && pageDrawArgsB_ &&
            (!d.compactArgs || pageArgCountB_) && hzbSrv_.ptr != 0 && hzbMipUav_[kHzbMips - 1].ptr != 0 &&
            prevPageTableSrv_.ptr != 0 && deferredPairsUav_.ptr != 0 && deferredPairsSrv_.ptr != 0 &&
            hzbCountersUav_.ptr != 0 && pageGroupCountBUav_.ptr != 0 && pageGroupCountBSrv_.ptr != 0 &&
            pageDrawArgsBUav_.ptr != 0 && (!d.compactArgs || pageArgCountBUav_.ptr != 0) &&
            groupsBSized_(shadowGpu) && deferredCap_ > 0 && poolSrv_.ptr != 0 &&
            perPageDirtySrv_.ptr != 0 && physOwnerSrv_.ptr != 0;
    // The pyramid describes the pool iff the last render also built it (no render slipped
    // through without a build) and no level switch voided it. The prev matrices are the ones
    // that render used (prevViewVp_), and the prev page table its mapping (the snapshot).
    d.hzbPrevValid = d.hzb && !hzbInvalidate_ && prevViewVpValid_ && lastRenderFrame_ != 0 &&
                     hzbLastBuildFrame_ == lastRenderFrame_;
    d.hzbFull = d.hzb && (hzbInvalidate_ || hzbLastBuildFrame_ == 0);

    return d;
}

// S5b.2: the pass-B buffers are sized by the group count exactly like pageGroupCount_.
bool VirtualShadowMap::groupsBSized_(ShadowGpuData* shadowGpu) const
{
    return shadowGpu && shadowGpu->MeshGroupCount() > 0 && shadowGpu->MeshGroupCount() <= scatterGroupsB_;
}

VirtualShadowMap::PageRenderDecisions VirtualShadowMap::PrepareRenderPass(
    RenderGraphPassContext& ctx, ShadowGpuData* shadowGpu, const vfx::WindState* wind)
{
    // D1.1 → pass-flow S3: the runtime predicates are decided HERE, once, registered exactly (no
    // union of reachable branches), and RETURNED — the AddPass2 builder captures them into the
    // record lambda, so the record reads the same values by construction.
    if (!IsAllocated() || !shadowGpu) { return PageRenderDecisions{}; }
    PageRenderDecisions d = ComputePageRenderDecisions(shadowGpu, wind);
    // S5b.2 cross-frame state, committed in the (serial) builder: this frame renders; if it also
    // builds the pyramid, next frame may test against it.
    hzbThisFrame_ = d.hzb;
    // Logged on change only: a headless run reads from the session log whether the stage ran,
    // and if not, which gate said no.
    {
        int reason = 0;
        if (vsm::g_hzbCull && !d.hzb)
        {
            reason = !d.scatterActive ? 1 : !d.singleDraw ? 2 : (!hzbBuildMat_ || !hzbPostMat_) ? 3
                   : !vsmHzb_ ? 4 : (!deferredPairs_ || !hzbCounters_ || !prevPageTable_) ? 5
                   : (!pageGroupCountB_ || !pageDrawArgsB_) ? 6 : !groupsBSized_(shadowGpu) ? 7
                   : (hzbSrv_.ptr == 0 || deferredPairsUav_.ptr == 0 || pageGroupCountBUav_.ptr == 0) ? 8 : 9;
        }
        const int state = d.hzb ? (1 + (d.hzbPrevValid ? 1 : 0)) : -reason;
        if (state != hzbLogged_ && ctx.renderer)
        {
            LOG_INFO(logging::LogCategory::RenderShadow, "vsm hzb cull: on={} prev-valid={} full={} off-reason={} (frame {})",
                     d.hzb ? 1 : 0, d.hzbPrevValid ? 1 : 0, d.hzbFull ? 1 : 0, reason, ctx.renderer->GetTotalFrameNumber());
            hzbLogged_ = state;
        }
    }
    if (ctx.renderer)
    {
        lastRenderFrame_ = ctx.renderer->GetTotalFrameNumber();
        if (d.hzb)
        {
            hzbLastBuildFrame_ = lastRenderFrame_;
            hzbInvalidate_ = false;
            // The counters' readback stamp (cross-frame state, the builder's): PollHzbStats reads
            // this slot kFrameCount frames from now.
            const UINT f = ctx.renderer->GetCurrentFrameIndex();
            if (f < render::kFrameCount && hzbReadback_[f]) { hzbReadbackFrame_[f] = lastRenderFrame_; }
        }
    }

    d.pointBase = ctx.usePoint ? *ctx.usePoint : 0u;
    if (d.hzb)
    {
        // The scatter reads last frame's pyramid + mapping and writes the deferred list, the
        // counters and (via the clear) pass B's counts; the pyramid's canonical is NPS already.
        ctx.Use(vsmHzb_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(prevPageTable_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(deferredPairs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(hzbCounters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(pageGroupCountB_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    ctx.Use(shadowGpu->IndirectArgsBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(physOwner_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.Use(physOwnerPrev_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(pageDrawArgs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(pageProj_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(pageVisibleList_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(perPageDirty_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (pageArgCount_) { ctx.Use(pageArgCount_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }

    // Spatial scatter cull (directional clipmap views only).
    if (d.scatterActive)
    {
        ctx.NextPoint();
        d.pointScatterWrite = ctx.usePoint ? *ctx.usePoint : 0u;
        ctx.Use(pageGroupCount_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(pageScatterDyn_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(pageTable_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.NextPoint();
        d.pointScatterRead = ctx.usePoint ? *ctx.usePoint : 0u;
        ctx.Use(pageGroupCount_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(pageScatterDyn_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Page cache snapshot: only when caching actually runs. TWO points, split around the copy:
    // a point is emitted WHOLESALE at its marker, so putting COPY_DEST and the post-copy NPS
    // restore in one point would slam physOwnerPrev to NPS before the copy records (a latent bug
    // of the single-point declaration this replaced — dormant only because g_pageCaching
    // defaults off).
    if (d.caching)
    {
        ctx.NextPoint();
        d.pointCacheCopy = ctx.usePoint ? *ctx.usePoint : 0u;
        ctx.Use(physOwner_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE);
        ctx.Use(physOwnerPrev_.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        ctx.NextPoint();
        d.pointCacheRead = ctx.usePoint ? *ctx.usePoint : 0u;
        ctx.Use(physOwnerPrev_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(perPageDirty_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Consume: args + per-instance stream + projection, then the pool as the depth target.
    ctx.NextPoint();
    d.pointConsume = ctx.usePoint ? *ctx.usePoint : 0u;
    // P12.1: hand the Rung 0 args BACK. This pass borrows them as an SRV at its first point above,
    // and it is the only pass that reads them in any state other than their resting one. Returning
    // them here is what makes the resting state deterministic: without it the buffer ended the
    // frame in whichever state the last-scheduled toucher happened to leave, which differed by
    // level and by whether this pass was built at all. It is also the state the spot/point shadow
    // passes -- scheduled AFTER this one -- ExecuteIndirect from.
    ctx.Use(shadowGpu->IndirectArgsBuffer(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ctx.Use(pageDrawArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    if (d.compactArgs) { ctx.Use(pageArgCount_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); }
    // pageProj goes to an SRV on the single-draw path and to a per-page root CBV on the loop.
    // One COMBINED read state now, not one per page-draw path — the record side transitions to
    // the union so both paths agree with the canonical table (see RecordPageRender).
    ctx.Use(pageProj_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                             D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    ctx.Use(pageVisibleList_.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    ctx.Use(pagePool_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (!d.hzb) { return d; }

    // ---- S5b.2, after pass A's draw: the pyramid of the pages pass A rendered ----
    ctx.NextPoint();
    d.pointHzbBuild = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(pagePool_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(vsmHzb_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(perPageDirty_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // ---- the deferred pairs against it, survivors appended after pass A's list entries ----
    ctx.NextPoint();
    d.pointHzbPost = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(vsmHzb_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(deferredPairs_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(pageVisibleList_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(pageGroupCountB_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(hzbCounters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // ---- pass B's args from its counts (the setup CS in pass-B mode) ----
    ctx.NextPoint();
    d.pointSetupB = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(pageGroupCountB_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(pageDrawArgsB_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (d.compactArgs) { ctx.Use(pageArgCountB_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    ctx.Use(perPageDirty_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // read through its UAV
    // ---- pass B into the same pages; the counters out to the readout ----
    ctx.NextPoint();
    d.pointDrawB = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(pagePool_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ctx.Use(pageDrawArgsB_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    if (d.compactArgs) { ctx.Use(pageArgCountB_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); }
    ctx.Use(pageVisibleList_.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    ctx.Use(hzbCounters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.NextPoint();
    d.pointHzbRestore = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(hzbCounters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return d;
}

void VirtualShadowMap::EnsureFrameResources(Renderer* renderer, ShadowGpuData* shadowGpu)
{
    if (!renderer || !IsAllocated()) { return; }
    EnsureShaderResources(renderer);
    // Same guard the render path used to apply inline: without mesh groups / this frame's
    // Rung 0 args there is nothing to size the per-page buffers against, and the pass
    // early-outs anyway.
    if (shadowGpu) { EnsureRenderResources(renderer, shadowGpu); }
}

void VirtualShadowMap::RecordPageRequest(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const vsm::PageRequestConstants& constants, D3D12_CPU_DESCRIPTOR_HANDLE depthSrv,
    UINT screenW, UINT screenH)
{
    if (!renderer || !cl || !IsAllocated() || depthSrv.ptr == 0 || screenW == 0 || screenH == 0) { return; }
    // Creation happens in EnsureFrameResources, before the graph runs (barrier plan step 4).
    if (!pageRequestClearMat_ || !pageRequestMat_) { return; }

    // The request bitfield is UAV-written by both dispatches. pass-flow S3c: its barrier rides
    // the base point, emitted by the pass body's marker before this is called.

    const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};

    // Clear the bitfield.
    RecordComputeDispatch(renderer, cl, pageRequestClearMat_.get(), sizeof(std::uint32_t) * 4,
        [](std::uint8_t* dst) { std::uint32_t v[4] = { vsm::kRequestWords, 0u, 0u, 0u }; std::memcpy(dst, v, sizeof(v)); },
        {},
        { requestUav_ },
        noSampler,
        vsm::kRequestWords, 1,
        requestBuffer_.Get());

    // Mark: one thread per DOWN-SAMPLED screen block (reduced res) -> project the block-center
    // pixel into each active local shadow view -> mark its mip page. The downscale comes from the
    // constants (lodParams.z), so the CPU dispatch size and the shader's block stride stay in sync
    // when it is tuned at runtime.
    UINT ds = static_cast<UINT>(constants.lodParams.z);
    if (ds < 1u) { ds = 1u; }
    const UINT reqW = (screenW + ds - 1u) / ds;
    const UINT reqH = (screenH + ds - 1u) / ds;
    RecordComputeDispatch(renderer, cl, pageRequestMat_.get(), static_cast<UINT>(sizeof(vsm::PageRequestConstants)),
        [&constants](std::uint8_t* dst) { std::memcpy(dst, &constants, sizeof(constants)); },
        { depthSrv },
        { requestUav_ },
        noSampler,
        reqW, reqH,
        requestBuffer_.Get());
    // Leaves requestBuffer_ in UNORDERED_ACCESS (the mark dispatch's UAV barrier) so
    // RecordPageAllocate, called right after this on the same command list, can read it.
}

void VirtualShadowMap::RecordPageAllocate(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                          const PageRequestPoints& pts,
                                          const vsm::ClipmapSquares* squares)
{
    if (!renderer || !cl || !IsAllocated() || !allocCounters_ || !allocUavHeap_) { return; }
    // Creation happens in EnsureFrameResources, before the graph runs (barrier plan step 4).
    if (!allocInitMat_ || !allocTouchMat_ || !allocFreeMat_ || !allocMapMat_) { return; }

    const std::uint32_t numEntries = vsm::kPageTableEntries;
    const std::uint32_t numPages = vsm::kPoolPageCount;
    const std::uint32_t curFrame = static_cast<std::uint32_t>(renderer->GetTotalFrameNumber());

    // S5b.2: the page table as it stood during last frame's render, before this frame's
    // allocation touches it (declared by PrepareRequestPass under the same gate).
    if (pts.hzbSnapshot && prevPageTable_)
    {
        renderer->EmitPoint(cl, pts.snapshotCopy);
        cl->CopyBufferRegion(prevPageTable_.Get(), 0, pageTable_.Get(), 0,
                             static_cast<UINT64>(vsm::kPageTableEntries) * sizeof(std::uint32_t));
    }
    // pass-flow S3c: the six alloc buffers' barriers (whatever this frame actually needs) come
    // from the compiled alloc point, emitted wholesale.
    renderer->EmitPoint(cl, pts.alloc);

    struct AllocCB { std::uint32_t numEntries, numPages, curFrame, lruThreshold; };
    auto writeCB = [&](std::uint8_t* dst)
    {
        AllocCB c{ numEntries, numPages, curFrame, vsm::g_lruThreshold };
        std::memcpy(dst, &c, sizeof(c));
    };
    const UINT cbSize = static_cast<UINT>(sizeof(AllocCB));
    const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};

    // Pass 0 (one-shot): clear the page table + mark all physical pages free. Persists across
    // frames + level switches (the pool IS the cache), so this runs exactly once.
    if (!allocInitialized_)
    {
        RecordComputeDispatch(renderer, cl, allocInitMat_.get(), cbSize, writeCB,
            {},
            { pageTableUav_, physOwnerUav_, physLastFrameUav_ },
            noSampler,
            numEntries, 1,
            pageTable_.Get());
        renderer->UAVBarrier(cl, physOwner_.Get());
        renderer->UAVBarrier(cl, physLastFrame_.Get());
        allocInitialized_ = true;
    }

    // Pass 1 (touch): keep resident+requested pages alive (LRU) + reset this frame's counters.
    RecordComputeDispatch(renderer, cl, allocTouchMat_.get(), cbSize, writeCB,
        {},
        { requestUav_, pageTableUav_, physLastFrameUav_, allocCountersUav_ },
        noSampler,
        numEntries, 1,
        physLastFrame_.Get());
    renderer->UAVBarrier(cl, allocCounters_.Get());

    // Pass 2 (build free list): evict LRU-stale pages, append free physical pages to the list.
    RecordComputeDispatch(renderer, cl, allocFreeMat_.get(), cbSize, writeCB,
        {},
        { physOwnerUav_, physLastFrameUav_, pageTableUav_, freeListUav_, allocCountersUav_ },
        noSampler,
        numPages, 1,
        freeList_.Get());
    renderer->UAVBarrier(cl, allocCounters_.Get());
    renderer->UAVBarrier(cl, pageTable_.Get());
    renderer->UAVBarrier(cl, physOwner_.Get());

    // Pass 3 (allocate): map each requested-but-not-resident page to a free physical page + append
    // it to the needs-render list.
    RecordComputeDispatch(renderer, cl, allocMapMat_.get(), cbSize, writeCB,
        {},
        { requestUav_, pageTableUav_, physOwnerUav_, physLastFrameUav_, freeListUav_, needsRenderUav_, allocCountersUav_ },
        noSampler,
        numEntries, 1,
        needsRender_.Get());

    // Pass 4 (propagate): give every UNMAPPED clipmap page a pointer to the nearest mapped page of
    // a coarser level. Must run AFTER pass 3, or it would miss this frame's new mappings, and it
    // rewrites every non-mapped clipmap page unconditionally because the table persists across
    // frames (a pointer left from last frame may name a page that now belongs to someone else).
    // See shaders/vsm_page_propagate_cs.hlsl for why the read/write race is benign.
    // Gated on SMRT being ON. Nothing else reads the fallback bit -- every existing consumer tests
    // "mapped here" (bit 31), which these entries never set -- so with SMRT off this dispatch would
    // be pure cost for output nobody reads.
    if (vsm::g_smrtRayCount > 0u && allocPropagateMat_ && squares && squares->count > 0)
    {
        renderer->UAVBarrier(cl, pageTable_.Get());

        struct PropagateCB
        {
            std::uint32_t numClipLevels, pagesPerAxis, pad0, pad1;
            vsm::ClipmapLevelSquare level[vsm::kNumClipmapLevels];
        };
        static_assert(sizeof(PropagateCB) ==
                          16 + sizeof(vsm::ClipmapLevelSquare) * vsm::kNumClipmapLevels,
                      "PropagateCB must stay a tight mirror of VsmPropagateCB in the shader");
        auto writePropagateCB = [&](std::uint8_t* dst)
        {
            PropagateCB c{};
            c.numClipLevels = squares->count;
            c.pagesPerAxis = vsm::kVirtualPagesL0Axis;
            for (std::uint32_t l = 0; l < squares->count && l < vsm::kNumClipmapLevels; ++l)
            {
                c.level[l] = squares->level[l];
            }
            std::memcpy(dst, &c, sizeof(c));
        };
        const std::uint32_t propagateThreads =
            squares->count * vsm::kVirtualPagesL0Axis * vsm::kVirtualPagesL0Axis;
        RecordComputeDispatch(renderer, cl, allocPropagateMat_.get(),
            static_cast<UINT>(sizeof(PropagateCB)), writePropagateCB,
            {},
            { pageTableUav_ },
            noSampler,
            propagateThreads, 1,
            pageTable_.Get());
    }

    // Step 20 debug: sample the request bitfield + alloc counters a few frames later.
    RecordDebugReadback(renderer, cl, pts);
}

void VirtualShadowMap::RecordDebugReadback(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                           const PageRequestPoints& pts)
{
    // pass-flow S3c: the decision was taken once, in PrepareRequestPass, and travels here as a
    // capture — evaluating WillRecordDebugReadback again is the drift this replaces.
    if (!pts.readback) { return; }

    const UINT64 reqBytes   = static_cast<UINT64>(vsm::kRequestWords) * sizeof(std::uint32_t);
    const UINT64 cntBytes   = 4ull * sizeof(std::uint32_t);
    const UINT64 ownerBytes = static_cast<UINT64>(vsm::kPoolPageCount) * sizeof(std::uint32_t); // physOwner snapshot (dev grid)
    if (!debugReadback_)
    {
        D3D12_HEAP_PROPERTIES rb{};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = reqBytes + cntBytes + ownerBytes;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        renderer->GetDevice()->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(debugReadback_.GetAddressOf()));
    }
    if (!debugReadback_) { return; }

    // pass-flow S3c: one marker moves all three sources to COPY_SOURCE before the first copy
    // (the first named Transition used to emit the whole point anyway — this makes it explicit).
    renderer->EmitPoint(cl, pts.readbackCopy);
    cl->CopyBufferRegion(debugReadback_.Get(), 0, requestBuffer_.Get(), 0, reqBytes);
    cl->CopyBufferRegion(debugReadback_.Get(), reqBytes, allocCounters_.Get(), 0, cntBytes);
    if (physOwner_)
    {
        cl->CopyBufferRegion(debugReadback_.Get(), reqBytes + cntBytes, physOwner_.Get(), 0, ownerBytes);
    }

    // Step 7 prerequisite (D2's frame epilogue): return these to their CANONICAL states instead
    // of parking them in COPY_SOURCE — a one-frame parked state is exactly what a static
    // canonical table cannot express. One marker restores all of them after the copies.
    renderer->EmitPoint(cl, pts.readbackRestore);

    debugReadbackFrame_ = renderer->GetTotalFrameNumber();
    debugReadbackState_ = 1;
}

void VirtualShadowMap::EnsureRenderResources(Renderer* renderer, ShadowGpuData* shadowGpu)
{
    if (!renderer || !renderer->GetDevice() || !shadowGpu) { return; }
    ID3D12Device* dev = renderer->GetDevice();
    const std::uint32_t groups = shadowGpu->MeshGroupCount();
    const std::uint32_t casters = shadowGpu->CasterCount();
    ID3D12Resource* rung0Args = shadowGpu->IndirectArgsBuffer();
    if (groups == 0 || !rung0Args) { return; }

    const std::uint32_t argUints = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) / 4; // 5
    // Per-instance caster LOD: every [page][group] layout is really [page][virtual group],
    // where the scatter buckets each instance by its own receiver LOD -> x kMaxShadowLods.
    constexpr std::uint32_t kLods = render::kMaxShadowLods;
    if (!pageDrawArgs_ || groups > renderGroups_)
    {
        pageDrawArgs_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount * groups * kLods * argUints, L"VSM.PageDrawArgs"), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, nullptr);
        renderGroups_ = groups;
        pageDrawArgsUav_ = {}; // force descriptor rebuild
    }
    if (!pageProj_)
    {
        // 64 uints (256 bytes) per page: a float4x4 + padding for root-CBV alignment.
        // Step 7 prereq: g_pageDrawSingle is RUNTIME-toggleable, and the two paths rest this
        // buffer in different states. Both are READ states and legally combine, so one
        // declaration covers either path and no re-declaration on toggle is needed.
        pageProj_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount * 64u, L"VSM.PageProj"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr);
        pageProjUav_ = {};
    }
    // Per-page-cull plan (Step 1): per (page, caster-slot) visible list, sized pool-pages x casters.
    // Allocated dormant here; the setup shader writes it and the draw binds it starting Step 2.
    if (!pageVisibleList_ || casters > renderCasters_)
    {
        const std::uint32_t cap = (casters > 0u ? casters : 1u) * kLods; // kLods buckets per group, each sized for the whole group
        pageVisibleList_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount * cap, L"VSM.PageVisibleList"), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr);
        renderCasters_ = cap;
        pageVisibleListUav_ = {}; // force descriptor rebuild
    }
    // Page-caching plan (Step 1): fixed-size (kPoolPageCount) caching buffers, allocated once.
    // physOwnerPrev_ = last frame's physOwner (new-page detect); perPageDirty_ = per-page dirty bit.
    if (!physOwnerPrev_)
    {
        physOwnerPrev_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.PhysOwnerPrev"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
        physOwnerPrevSrv_ = {};
    }
    if (!perPageDirty_)
    {
        perPageDirty_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.PerPageDirty"), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
        perPageDirtyUav_ = {}; perPageDirtySrv_ = {};
    }
    // Compacted draw args: the append counter / ExecuteIndirect count buffer (element 0 only).
    if (!pageArgCount_)
    {
        pageArgCount_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, 4u, L"VSM.PageArgCount"), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
        pageArgCountUav_ = {};
    }
    // Spatial scatter cull: per (page, mesh-group) count/cursor + per-page dynamic-overlap flag.
    if (!pageGroupCount_ || groups > scatterGroups_)
    {
        pageGroupCount_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount * groups * kLods, L"VSM.PageGroupCount"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
        scatterGroups_ = groups;
        pageGroupCountUav_ = {}; pageGroupCountSrv_ = {};
    }
    if (!pageScatterDyn_)
    {
        pageScatterDyn_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.PageScatterDyn"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
        pageScatterDynUav_ = {}; pageScatterDynSrv_ = {};
    }
    if (!pageDrawArgs_ || !pageProj_ || !physOwnerPrev_ || !perPageDirty_ ||
        !pageGroupCount_ || !pageScatterDyn_) { return; }
    // S5b.2: the pool pyramid + the pass-B set (its own heap; failure just keeps the feature off).
    EnsureHzbResources(renderer, groups, casters);

    // Resident-set readback ring (physOwner snapshots), persistently mapped.
    if (!residentReadback_[0])
    {
        D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(vsm::kPoolPageCount) * sizeof(std::uint32_t);
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        for (UINT i = 0; i < render::kFrameCount; ++i)
        {
            if (SUCCEEDED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(residentReadback_[i].GetAddressOf()))))
            {
                void* p = nullptr;
                if (SUCCEEDED(residentReadback_[i]->Map(0, nullptr, &p))) { residentReadbackPtr_[i] = static_cast<const std::uint32_t*>(p); }
            }
        }
    }

    const bool needHeap = !renderHeap_ || cachedRung0Args_ != rung0Args ||
                          physOwnerSrv_.ptr == 0 || pageDrawArgsUav_.ptr == 0 || pageProjUav_.ptr == 0 ||
                          pageVisibleListUav_.ptr == 0 || physOwnerPrevSrv_.ptr == 0 ||
                          perPageDirtyUav_.ptr == 0 || perPageDirtySrv_.ptr == 0 ||
                          pageGroupCountUav_.ptr == 0 || pageGroupCountSrv_.ptr == 0 ||
                          pageScatterDynUav_.ptr == 0 || pageScatterDynSrv_.ptr == 0 ||
                          pageProjSrv_.ptr == 0 || pageArgCountUav_.ptr == 0;
    if (!needHeap) { return; }

    if (!renderHeap_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 14; // + pageGroupCount UAV/SRV + pageScatterDyn UAV/SRV + pageProj SRV + argCount UAV
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(renderHeap_.GetAddressOf()))) || !renderHeap_)
        {
            renderHeap_.Reset();
            return;
        }
    }
    const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = renderHeap_->GetCPUDescriptorHandleForHeapStart();
    physOwnerSrv_       = { base.ptr + static_cast<SIZE_T>(0) * incr };
    rung0ArgsSrv_       = { base.ptr + static_cast<SIZE_T>(1) * incr };
    pageDrawArgsUav_    = { base.ptr + static_cast<SIZE_T>(2) * incr };
    pageProjUav_        = { base.ptr + static_cast<SIZE_T>(3) * incr };
    pageVisibleListUav_ = { base.ptr + static_cast<SIZE_T>(4) * incr };
    physOwnerPrevSrv_   = { base.ptr + static_cast<SIZE_T>(5) * incr };
    perPageDirtyUav_    = { base.ptr + static_cast<SIZE_T>(6) * incr };
    perPageDirtySrv_    = { base.ptr + static_cast<SIZE_T>(7) * incr };
    pageGroupCountUav_  = { base.ptr + static_cast<SIZE_T>(8) * incr };
    pageGroupCountSrv_  = { base.ptr + static_cast<SIZE_T>(9) * incr };
    pageScatterDynUav_  = { base.ptr + static_cast<SIZE_T>(10) * incr };
    pageScatterDynSrv_  = { base.ptr + static_cast<SIZE_T>(11) * incr };
    pageProjSrv_        = { base.ptr + static_cast<SIZE_T>(12) * incr };
    pageArgCountUav_    = { base.ptr + static_cast<SIZE_T>(13) * incr };

    D3D12_SHADER_RESOURCE_VIEW_DESC oSd{};
    oSd.Format = DXGI_FORMAT_UNKNOWN;
    oSd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    oSd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    oSd.Buffer.NumElements = vsm::kPoolPageCount;
    oSd.Buffer.StructureByteStride = sizeof(std::uint32_t);
    dev->CreateShaderResourceView(physOwner_.Get(), &oSd, physOwnerSrv_);

    D3D12_SHADER_RESOURCE_VIEW_DESC aSd{};
    aSd.Format = DXGI_FORMAT_R32_TYPELESS;
    aSd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    aSd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    aSd.Buffer.NumElements = static_cast<UINT>(rung0Args->GetDesc().Width / 4);
    aSd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    dev->CreateShaderResourceView(rung0Args, &aSd, rung0ArgsSrv_);

    D3D12_UNORDERED_ACCESS_VIEW_DESC dUd{};
    dUd.Format = DXGI_FORMAT_R32_TYPELESS;
    dUd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    dUd.Buffer.NumElements = static_cast<UINT>(pageDrawArgs_->GetDesc().Width / 4);
    dUd.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(pageDrawArgs_.Get(), nullptr, &dUd, pageDrawArgsUav_);

    D3D12_UNORDERED_ACCESS_VIEW_DESC pUd{};
    pUd.Format = DXGI_FORMAT_R32_TYPELESS;
    pUd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    pUd.Buffer.NumElements = static_cast<UINT>(pageProj_->GetDesc().Width / 4);
    pUd.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(pageProj_.Get(), nullptr, &pUd, pageProjUav_);

    // Single-draw page render: the same buffer as a StructuredBuffer<float4> for the VSM_PAGE VS.
    // The setup CS writes each page a 256-byte slot (root-CBV alignment) = 16 float4s, so page p's
    // matrix rows are elements p*16+0..3 and its wind tail elements p*16+12 and +13.
    D3D12_SHADER_RESOURCE_VIEW_DESC pSd{};
    pSd.Format = DXGI_FORMAT_UNKNOWN;
    pSd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    pSd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    pSd.Buffer.NumElements = vsm::kPoolPageCount * 16u;
    pSd.Buffer.StructureByteStride = 16u;
    dev->CreateShaderResourceView(pageProj_.Get(), &pSd, pageProjSrv_);

    // Per-page visible list: structured uint UAV (the setup shader appends caster ids). Dormant in
    // Step 1 (created but not bound). Guarded because the buffer alloc can fail independently.
    if (pageVisibleList_)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC vUd{};
        vUd.Format = DXGI_FORMAT_UNKNOWN;
        vUd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        vUd.Buffer.NumElements = static_cast<UINT>(pageVisibleList_->GetDesc().Width / sizeof(std::uint32_t));
        vUd.Buffer.StructureByteStride = sizeof(std::uint32_t);
        dev->CreateUnorderedAccessView(pageVisibleList_.Get(), nullptr, &vUd, pageVisibleListUav_);
    }

    // Page cache (Step 1): physOwnerPrev SRV (setup reads last frame's owner), perPageDirty UAV
    // (setup writes the dirty bit) + SRV (the gated depth-clear reads it). Dormant until Step 2.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = vsm::kPoolPageCount;
        sd.Buffer.StructureByteStride = sizeof(std::uint32_t);
        dev->CreateShaderResourceView(physOwnerPrev_.Get(), &sd, physOwnerPrevSrv_);
        dev->CreateShaderResourceView(perPageDirty_.Get(), &sd, perPageDirtySrv_);

        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = vsm::kPoolPageCount;
        ud.Buffer.StructureByteStride = sizeof(std::uint32_t);
        dev->CreateUnorderedAccessView(perPageDirty_.Get(), nullptr, &ud, perPageDirtyUav_);
        // Scatter cull: per-page dynamic-overlap flag (same shape as perPageDirty).
        dev->CreateShaderResourceView(pageScatterDyn_.Get(), &sd, pageScatterDynSrv_);
        dev->CreateUnorderedAccessView(pageScatterDyn_.Get(), nullptr, &ud, pageScatterDynUav_);

        // Compacted-args counter. RAW (like pageDrawArgs_), NOT structured: the CPU zeroes it with
        // ClearUnorderedAccessViewUint each frame, and that call rejects structured buffers outright
        // (GBV id=1156). The shader declares it as RWByteAddressBuffer to match.
        if (pageArgCount_)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC aud{};
            aud.Format = DXGI_FORMAT_R32_TYPELESS;
            aud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            aud.Buffer.NumElements = static_cast<UINT>(pageArgCount_->GetDesc().Width / 4);
            aud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            dev->CreateUnorderedAccessView(pageArgCount_.Get(), nullptr, &aud, pageArgCountUav_);
        }

        // Scatter cull: per (page, mesh-group) count/cursor — sized pool-pages x groups.
        const UINT cntElems = static_cast<UINT>(pageGroupCount_->GetDesc().Width / sizeof(std::uint32_t));
        D3D12_SHADER_RESOURCE_VIEW_DESC csd = sd; csd.Buffer.NumElements = cntElems;
        dev->CreateShaderResourceView(pageGroupCount_.Get(), &csd, pageGroupCountSrv_);
        D3D12_UNORDERED_ACCESS_VIEW_DESC cud = ud; cud.Buffer.NumElements = cntElems;
        dev->CreateUnorderedAccessView(pageGroupCount_.Get(), nullptr, &cud, pageGroupCountUav_);
    }

    cachedRung0Args_ = rung0Args;
}

// Occlusion plan S5b.2: the pool pyramid (R32_FLOAT, half the pool, 7 mips), last frame's page
// table, the deferred (caster, page) pairs, the counters and pass B's counts/args/count buffer.
// One non-shader-visible heap of 16 descriptors, staged per dispatch like everything else here.
// Sized like their pass-A twins (groups, casters); the pairs list caps at 8 per caster -- past
// that the scatter draws the pair in pass A (conservative) and counts the overflow.
void VirtualShadowMap::EnsureHzbResources(Renderer* renderer, std::uint32_t groups, std::uint32_t casters)
{
    if (!renderer || !renderer->GetDevice()) { return; }
    ID3D12Device* dev = renderer->GetDevice();
    constexpr std::uint32_t kLods = render::kMaxShadowLods;
    constexpr std::uint32_t argUints = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) / 4;
    const std::uint32_t hzbMip0 = vsm::kPoolTexels / 2u;

    bool rebuildHeap = !hzbHeap_;
    if (!vsmHzb_)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = hzbMip0;
        rd.Height = hzbMip0;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = static_cast<UINT16>(kHzbMips);
        rd.Format = DXGI_FORMAT_R32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(render::CreateCommittedTexture(dev, heap, D3D12_HEAP_FLAG_NONE, rd,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, vsmHzb_.GetAddressOfForCreate())) || !vsmHzb_)
        {
            vsmHzb_.Reset();
            return;
        }
        vsmHzb_.DeclareCreated(renderer->Declarations(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"VSM.Hzb");
        hzbInvalidate_ = true; // fresh texture: full build before anything trusts it
        rebuildHeap = true;
    }
    if (!prevPageTable_)
    {
        prevPageTable_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPageTableEntries, L"VSM.PrevPageTable"),
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
        rebuildHeap = true;
    }
    const std::uint32_t wantCap = std::max<std::uint32_t>(1024u, 8u * (casters > 0u ? casters : 1u));
    if (!deferredPairs_ || wantCap > deferredCap_)
    {
        deferredPairs_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, wantCap * 2u, L"VSM.DeferredPairs"),
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
        deferredCap_ = deferredPairs_ ? wantCap : 0u;
        rebuildHeap = true;
    }
    if (!hzbCounters_)
    {
        hzbCounters_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, 4u, L"VSM.HzbCounters"),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
        rebuildHeap = true;
    }
    if (!pageGroupCountB_ || groups > scatterGroupsB_)
    {
        pageGroupCountB_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount * groups * kLods, L"VSM.PageGroupCountB"),
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
        pageDrawArgsB_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, vsm::kPoolPageCount * groups * kLods * argUints, L"VSM.PageDrawArgsB"),
                              D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, nullptr);
        scatterGroupsB_ = (pageGroupCountB_ && pageDrawArgsB_) ? groups : 0u;
        rebuildHeap = true;
    }
    if (!pageArgCountB_)
    {
        pageArgCountB_.Attach(renderer->Declarations(), CreateUavUintBuffer(dev, 4u, L"VSM.PageArgCountB"),
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
        rebuildHeap = true;
    }
    if (!vsmHzb_ || !prevPageTable_ || !deferredPairs_ || !hzbCounters_ || !pageGroupCountB_ || !pageDrawArgsB_ || !pageArgCountB_) { return; }

    if (!hzbReadback_[0])
    {
        D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 4u * sizeof(std::uint32_t);
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        for (UINT i = 0; i < render::kFrameCount; ++i)
        {
            if (SUCCEEDED(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(hzbReadback_[i].GetAddressOf()))))
            {
                hzbReadback_[i]->SetName(L"VSM.HzbReadback");
                void* p = nullptr;
                if (SUCCEEDED(hzbReadback_[i]->Map(0, nullptr, &p))) { hzbReadbackPtr_[i] = static_cast<const std::uint32_t*>(p); }
            }
            hzbReadbackFrame_[i] = 0;
        }
    }

    if (!rebuildHeap) { return; }
    if (!hzbHeap_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 16; // hzb SRV + 7 mip UAVs + prevPageTable SRV + pairs UAV/SRV + counters UAV + countsB UAV/SRV + argsB UAV + argCountB UAV
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(hzbHeap_.GetAddressOf()))) || !hzbHeap_)
        {
            hzbHeap_.Reset();
            return;
        }
    }
    const UINT incr = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = hzbHeap_->GetCPUDescriptorHandleForHeapStart();
    auto slot = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{ base.ptr + static_cast<SIZE_T>(i) * incr }; };
    hzbSrv_ = slot(0);
    for (UINT m = 0; m < kHzbMips; ++m) { hzbMipUav_[m] = slot(1 + m); }
    prevPageTableSrv_ = slot(8);
    deferredPairsUav_ = slot(9);
    deferredPairsSrv_ = slot(10);
    hzbCountersUav_ = slot(11);
    pageGroupCountBUav_ = slot(12);
    pageGroupCountBSrv_ = slot(13);
    pageDrawArgsBUav_ = slot(14);
    pageArgCountBUav_ = slot(15);

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = kHzbMips;
        dev->CreateShaderResourceView(vsmHzb_.Get(), &sd, hzbSrv_);
        for (UINT m = 0; m < kHzbMips; ++m)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_FLOAT;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            ud.Texture2D.MipSlice = m;
            dev->CreateUnorderedAccessView(vsmHzb_.Get(), nullptr, &ud, hzbMipUav_[m]);
        }
    }
    auto structuredSrv = [&](ID3D12Resource* res, UINT elems, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE h)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = elems;
        sd.Buffer.StructureByteStride = stride;
        dev->CreateShaderResourceView(res, &sd, h);
    };
    auto structuredUav = [&](ID3D12Resource* res, UINT elems, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE h)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = elems;
        ud.Buffer.StructureByteStride = stride;
        dev->CreateUnorderedAccessView(res, nullptr, &ud, h);
    };
    auto rawUav = [&](ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE h)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = static_cast<UINT>(res->GetDesc().Width / 4);
        ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        dev->CreateUnorderedAccessView(res, nullptr, &ud, h);
    };
    structuredSrv(prevPageTable_.Get(), vsm::kPageTableEntries, sizeof(std::uint32_t), prevPageTableSrv_);
    structuredUav(deferredPairs_.Get(), deferredCap_, 2u * sizeof(std::uint32_t), deferredPairsUav_);
    structuredSrv(deferredPairs_.Get(), deferredCap_, 2u * sizeof(std::uint32_t), deferredPairsSrv_);
    structuredUav(hzbCounters_.Get(), 4u, sizeof(std::uint32_t), hzbCountersUav_);
    const UINT cntElems = static_cast<UINT>(pageGroupCountB_->GetDesc().Width / sizeof(std::uint32_t));
    structuredUav(pageGroupCountB_.Get(), cntElems, sizeof(std::uint32_t), pageGroupCountBUav_);
    structuredSrv(pageGroupCountB_.Get(), cntElems, sizeof(std::uint32_t), pageGroupCountBSrv_);
    rawUav(pageDrawArgsB_.Get(), pageDrawArgsBUav_);
    rawUav(pageArgCountB_.Get(), pageArgCountBUav_);
}

void VirtualShadowMap::PollHzbStats(Renderer* renderer)
{
    // The slot of frame N - kFrameCount: its fence passed in this frame's BeginFrame.
    if (!renderer) { return; }
    const std::uint64_t now = renderer->GetTotalFrameNumber();
    if (now < render::kFrameCount) { return; }
    const std::uint64_t want = now - render::kFrameCount;
    for (UINT s = 0; s < render::kFrameCount; ++s)
    {
        if (hzbReadbackFrame_[s] != want || want == 0 || !hzbReadbackPtr_[s]) { continue; }
        std::memcpy(hzbStats_.data(), hzbReadbackPtr_[s], 4u * sizeof(std::uint32_t));
        hzbReadbackFrame_[s] = 0;
        return;
    }
}

void VirtualShadowMap::RecordPageRender(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    ShadowGpuData* shadowGpu, const vsm::ViewProjEntry* views, std::uint32_t viewCount,
    const vfx::WindState* wind, const PageRenderDecisions& dec)
{
    if (!renderer || !cl || !IsAllocated() || !shadowGpu || !views) { return; }
    // Creation happens in EnsureFrameResources, before the graph runs (barrier plan step 4).
    if (!pageSetupMat_) { return; }
    if (!shadowGpu->IndirectDrawReady()) { return; } // needs this frame's Rung 0 cull output
    if (!pageDrawArgs_ || !pageProj_ || !renderHeap_ || !pageVisibleList_) { return; }

    // The LOOP fallback draws into the pool, whose DSV is D32 — it needs the pool-format twin of
    // the Legacy PSO (one PSO cannot serve two depth formats).
    Material* indirectMat = shadowGpu->IndirectShadowPoolMaterial();
    ID3D12CommandSignature* sig = renderer->GetDrawIndexedCommandSignature();
    if (!indirectMat || !indirectMat->GetPipelineState() || !sig) { return; }

    const UINT f = renderer->GetCurrentFrameIndex();
    if (f >= render::kFrameCount) { return; }
    const std::uint32_t groups = shadowGpu->MeshGroupCount();
    if (groups == 0 || groups > renderGroups_) { return; }
    // Per-page cull inputs: the unified world-AABB bounds (region f), the per-caster group ids, and
    // the per-page-list UAV. All present whenever count_>0 (guaranteed by Rebuild); bail otherwise
    // (OOM edge) rather than binding a null descriptor.
    const D3D12_CPU_DESCRIPTOR_HANDLE boundsSrv = shadowGpu->UnifiedBoundsSrv(f);
    const D3D12_CPU_DESCRIPTOR_HANDLE casterGroupSrv = shadowGpu->CasterGroupSrv();
    // The setup shader always reads physOwnerPrev (t4) + casterMeta (t5) + writes perPageDirty (u3),
    // so those must be non-null whenever it dispatches (bail otherwise, like the other inputs).
    const D3D12_CPU_DESCRIPTOR_HANDLE casterMetaSrv = shadowGpu->CasterMetaSrv(renderer->GetCurrentFrameIndex());
    if (boundsSrv.ptr == 0 || casterGroupSrv.ptr == 0 || pageVisibleListUav_.ptr == 0 ||
        physOwnerPrevSrv_.ptr == 0 || casterMetaSrv.ptr == 0 || perPageDirtyUav_.ptr == 0) { return; }
    const std::uint32_t activeCasters = shadowGpu->ActiveCasterCount();

    // Per-view "matrix changed" bits for the page cache (gViewDirtyMask; see the setup CB
    // comment). A cached page's id carries no scroll offset, so any view movement makes its
    // content stand for the wrong world rect with the owner id unchanged — the only honest
    // answer until pages scroll is to re-render that whole view. Bit-exact compare on purpose:
    // a static camera rebuilds bit-identical matrices, so a parked frame caches fully.
    std::uint32_t viewDirtyMask[2] = { 0u, 0u };
    // S5b.2: last frame's matrices, kept for the scatter's prev-pyramid test before the compare
    // below replaces them with this frame's.
    DirectX::XMFLOAT4X4 prevVp[vsm::kMaxVirtualViews];
    std::memcpy(prevVp, prevViewVp_, sizeof(prevVp));
    {
        const std::uint32_t nv = (viewCount < vsm::kMaxVirtualViews) ? viewCount : vsm::kMaxVirtualViews;
        for (std::uint32_t v = 0; v < nv; ++v)
        {
            const bool changed = !prevViewVpValid_ ||
                std::memcmp(&prevViewVp_[v], &views[v].viewProj, sizeof(DirectX::XMFLOAT4X4)) != 0;
            if (changed) { viewDirtyMask[v >> 5u] |= (1u << (v & 31u)); }
            prevViewVp_[v] = views[v].viewProj;
        }
        prevViewVpValid_ = true;
    }

    // Page cache: active only when the clear PSO + dirty SRV are ready (the inputs above are already
    // guaranteed here). When off, force every page dirty (gForceAll=1) so the whole-pool-clear +
    // draw-all fallback stays correct. When on, force all only when a static caster moved this frame
    // (MoverCount>0) or a rebuild happened — not covered by the per-page dynamic-overlap test.
    // D1.1 → pass-flow S3: `dec` arrives as a parameter, captured by the AddPass2 builder from
    // the very PrepareRenderPass call that made this frame's declarations — the record cannot
    // read anything else.
    const bool caching = dec.caching;
    // Force a full render when caching is off, on the warmup frame (physOwnerPrev_ still garbage), or
    // when a static caster moved / a rebuild happened (not covered by the per-page dynamic test).
    // W5: a wind caster sways in the VERTEX shader, so it is neither a "mover" nor a dynamic caster —
    // the per-page dirty test would call its page clean and cache a stale, frozen shadow pose.
    const bool windAnimating = wind && wind->swayAmplitude > 0.0f && shadowGpu->HasWindCasters();
    const std::uint32_t forceAll = dec.forceAll;

    // Consolidated caster VB/IB (built once at level load, ShadowGpuData::EnsureMegaBuffer): when
    // ready, the draw loop below binds geometry ONCE + issues one ExecuteIndirect(maxCount=groups)
    // per page instead of a bind + draw per (page, mesh-group).
    const bool useMega = dec.useMega;

    // Single-draw page render: ONE ExecuteIndirect over every (page, group) arg instead of the
    // kPoolPageCount-iteration loop below. Decided HERE, above the scatter dispatch, because it
    // drives gPageIdShift in BOTH the scatter CB and the setup CB — the two writers of
    // PageVisibleList must agree on the packing within a frame.
    //   - useMega: without the consolidated VB/IB the loop must re-bind geometry per mesh-group, so
    //     it cannot collapse to one draw at all.
    //   - activeCasters bound: the caster slot id has to survive in the low kPageIdShift bits.
    //   - pageProjSrv_/pageMat: the VS reads the projection from the SRV and needs its own PSO.
    Material* pageMat = shadowGpu->IndirectShadowPageMaterial();
    const bool singleDraw = dec.singleDraw;
    if (vsm::g_pageDrawSingle && !singleDraw)
    {
        // Log the reason ONCE per distinct cause — silently falling back to the loop is exactly the
        // failure mode that makes a "why is this still slow / still blinking" hunt expensive.
        const int reason = !useMega                                 ? 1
                         : (activeCasters >= (1u << vsm::kPageIdShift)) ? 2
                         : (pageProjSrv_.ptr == 0)                  ? 3 : 4;
        if (reason != singleDrawFallbackLogged_)
        {
            singleDrawFallbackLogged_ = reason;
            static const char* const kReason[5] = { "",
                "mega buffer unavailable (heterogeneous caster meshes)",
                "caster count exceeds the packed-id budget",
                "pageProj SRV missing",
                "VSM_PAGE PSO unavailable" };
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "[VSM] single-draw page render OFF: %s -- using the per-page loop.\n",
                          kReason[reason]);
            // A Warning in the session log, readable from any run: the per-page loop is the slow
            // path, and "why is dragging this mesh 150x slower than flying the camera" cost a
            // long hunt precisely because the engine knew the answer and whispered it somewhere
            // unreadable (DBWIN, then a side file — both gone).
            logging::WriteRaw(logging::LogLevel::Warning, logging::LogCategory::RenderShadow, msg);
        }
    }
    else if (singleDraw) { singleDrawFallbackLogged_ = 0; }

    // Compacted draw args: only meaningful on the single-draw path — the loop derives each page's
    // argOffset from (page, group), so a compacted buffer would feed it other pages' records.
    const bool compactArgs = dec.compactArgs;

    // --- Setup compute: per physical page, build the off-center projection AND cull the caster set
    // to the page's frustum, writing a per-page compacted visible list + per-page draw args. Reads
    // Rung0 args (per-group index count) + physOwner + the unified world AABBs + per-caster group. ---
    // pass-flow S3b: EmitPoint markers — the compiled point carries whatever barriers this frame
    // needs for the entry uses (args, physOwner ping-pong, the page UAV set).
    renderer->EmitPoint(cl, dec.pointBase);
    if (compactArgs)
    {
        // Zero the append counter before the setup bumps it. ClearUnorderedAccessViewUint needs the
        // view BOTH in a non-shader-visible heap (renderHeap_) and in the currently bound
        // shader-visible one, hence the stage — cheaper than a dispatch for four bytes.
        const UINT zero[4] = { 0u, 0u, 0u, 0u };
        cl->ClearUnorderedAccessViewUint(renderer->StageSrvUavTable({ pageArgCountUav_ }).gpu,
                                         pageArgCountUav_, pageArgCount_.Get(), zero, 0, nullptr);
        renderer->UAVBarrier(cl, pageArgCount_.Get());
    }

    // --- Spatial scatter cull (directional clipmap): clear the per (page, group) counters, then let
    // every caster append itself into the few pages its AABB covers. This replaces the O(pages x
    // casters) brute force for clipmap pages; local (perspective) views still cull in the setup. ---
    const D3D12_CPU_DESCRIPTOR_HANDLE perGroupSrv = shadowGpu->PerGroupSrv();
    const bool scatterActive = dec.scatterActive;
    const bool scatterLocals = dec.scatterLocals;
    if (scatterActive)
    {
        GPU_SCOPE(cl, ProfilerScopes::kVsmPageScatter);
        renderer->EmitPoint(cl, dec.pointScatterWrite);

        const std::uint32_t countElems = vsm::kPoolPageCount * groups * render::kMaxShadowLods;
        // S5b.2: pass B's counts + the counters are zeroed by the same clear (placeholders bound
        // and `clearB = 0` when the two-pass occlusion is off this frame).
        const bool hzb = dec.hzb;
        struct ClearCB { std::uint32_t countElems, numPages, clearB, p1; };
        RecordComputeDispatch(renderer, cl, pageScatterClearMat_.get(), sizeof(ClearCB),
            [&](std::uint8_t* dst) { ClearCB c{ countElems, vsm::kPoolPageCount, hzb ? 1u : 0u, 0u }; std::memcpy(dst, &c, sizeof(c)); },
            {}, { pageGroupCountUav_, pageScatterDynUav_,
                  hzb ? pageGroupCountBUav_ : pageGroupCountUav_, hzb ? hzbCountersUav_ : pageScatterDynUav_ },
            D3D12_GPU_DESCRIPTOR_HANDLE{},
            countElems, 1, pageGroupCount_.Get());
        renderer->UAVBarrier(cl, pageScatterDyn_.Get());
        if (hzb)
        {
            renderer->UAVBarrier(cl, pageGroupCountB_.Get());
            renderer->UAVBarrier(cl, hzbCounters_.Get());
        }

        struct ScatterCB
        {
            std::uint32_t numCasters, numGroups, numLevels, pageIdShift;
            // S5: local (spot/point) views scatter too. They follow the clipmap levels in the
            // dispatch's y axis, and the matrix table became EVERY view rather than just the 8
            // clipmap ones — same indexing as the setup CB (0..31 local, 32..39 clipmap).
            std::uint32_t numLocalViews;
            std::uint32_t perInstanceLod; // A/B lever; mirrors gPerInstanceLod
            std::uint32_t _pad0[2];
            DirectX::XMFLOAT4X4 viewProj[vsm::kMaxVirtualViews];
            // Per SCATTER TARGET (clipmap levels, then locals): the view's tier-curve LOD.
            // Only a lower bound -- each caster's own receiver LOD can push past it COARSER,
            // never finer (LodSelect.h's per-instance contract).  Mirrors gTargetLod.
            DirectX::XMUINT4 targetLod[(vsm::kMaxVirtualViews + 3) / 4];
            // S5b.2: (on, prevValid, deferred capacity, 0) + LAST frame's matrices. Mirrors
            // gHzbParams / gPrevViewProj.
            DirectX::XMUINT4 hzbParams{ 0u, 0u, 0u, 0u };
            DirectX::XMFLOAT4X4 prevViewProj[vsm::kMaxVirtualViews];
        };
        RecordComputeDispatch(renderer, cl, pageScatterMat_.get(), static_cast<UINT>(sizeof(ScatterCB)),
            [&](std::uint8_t* dst)
            {
                ScatterCB c{};
                c.numCasters = activeCasters;
                c.numGroups = groups;
                c.numLevels = vsm::kNumClipmapLevels;
                // Single-draw page render: pack the physical page index into the high bits of every
                // visible-list entry so ONE draw can serve all pages. MUST match the setup CB below —
                // both write the same list and the VS decodes it with one rule.
                c.pageIdShift = singleDraw ? vsm::kPageIdShift : 0u;
                c.numLocalViews = scatterLocals ? vsm::kNumLocalVirtualViews : 0u;
                c.perInstanceLod = vsm::g_perInstanceCasterLod ? 1u : 0u;
                // Every view, straight through. An inactive local slot stays the ZERO matrix this
                // value-initialised struct starts with, which the shader's perspective path rejects
                // on its own (maxW <= 0) — and such a view owns no resident pages either.
                const std::uint32_t nv = (viewCount < vsm::kMaxVirtualViews) ? viewCount : vsm::kMaxVirtualViews;
                for (std::uint32_t v = 0; v < nv; ++v) { c.viewProj[v] = views[v].viewProj; }
                // Target t maps to cull-view: clipmap level L -> 4 locals-offset... precisely:
                // VSM view = isLocal ? t - numLevels : 32 + t, cull view = VSM view + 4 (the
                // same +4 the setup shader applies as rung0View).
                const std::uint32_t targets = vsm::kNumClipmapLevels +
                    (scatterLocals ? vsm::kNumLocalVirtualViews : 0u);
                for (std::uint32_t t = 0; t < targets && t < vsm::kMaxVirtualViews; ++t)
                {
                    const bool isLocal = (t >= vsm::kNumClipmapLevels);
                    const std::uint32_t vsmView = isLocal ? (t - vsm::kNumClipmapLevels)
                                                          : (vsm::kNumLocalVirtualViews + t);
                    const std::uint32_t lod = shadowGpu->ViewLodAt(vsmView + 4u);
                    reinterpret_cast<std::uint32_t*>(&c.targetLod[0])[t] = lod;
                }
                // S5b.2: the pyramid's pages were drawn with LAST frame's matrices -- the ones
                // prevViewVp_ held before the view-dirty compare above overwrote it (prevVp).
                c.hzbParams = DirectX::XMUINT4(dec.hzb ? 1u : 0u, dec.hzbPrevValid ? 1u : 0u, deferredCap_, 0u);
                for (std::uint32_t v = 0; v < vsm::kMaxVirtualViews; ++v) { c.prevViewProj[v] = prevVp[v]; }
                std::memcpy(dst, &c, sizeof(c));
            },
            { boundsSrv, casterGroupSrv, casterMetaSrv, pageTableSrv_, perGroupSrv,
              shadowGpu->CasterLodSrv(f),
              dec.hzb ? prevPageTableSrv_ : pageTableSrv_, dec.hzb ? hzbSrv_ : poolSrv_ },
            { pageGroupCountUav_, pageVisibleListUav_, pageScatterDynUav_,
              dec.hzb ? deferredPairsUav_ : pageScatterDynUav_, dec.hzb ? hzbCountersUav_ : pageScatterDynUav_ },
            D3D12_GPU_DESCRIPTOR_HANDLE{},
            // y now spans clipmap levels AND local views: RecordComputeDispatch rounds up to its
            // 8-thread groups, so 8 + 32 = 40 becomes 5 groups.
            activeCasters,
            vsm::kNumClipmapLevels + (scatterLocals ? vsm::kNumLocalVirtualViews : 0u),
            pageGroupCount_.Get());
        renderer->UAVBarrier(cl, pageVisibleList_.Get());
        renderer->UAVBarrier(cl, pageScatterDyn_.Get());
        // The setup pass reads the counts + dyn flags as SRVs.
        renderer->EmitPoint(cl, dec.pointScatterRead);
    }

    constexpr std::uint32_t kLods = render::kMaxShadowLods;                    // KMAX_SHADOW_LODS
    constexpr std::uint32_t kViewLodVec4 = (render::kMaxShadowViews + 3u) / 4u; // 44 cull-views packed 4/uint4
    struct SetupCB
    {
        std::uint32_t numGroups, argBaseElems, numPages, numCasters;
        std::uint32_t forceAll, megaActive, flatLod, numLods; // per-view LOD: mega on/off + fallback LOD
        std::uint32_t scatterActive, pageIdShift, compactArgs; // 1 = the scatter pass produced clipmap lists
        std::uint32_t scatterLocals; // S5: 1 = LOCAL views were scattered too (was _pad5)
        std::uint32_t windDirtyMaxLevel = 0; // mirrors gWindDirtyMaxLevel (wind page-cache range)
        std::uint32_t viewDirtyMask[2]{};    // mirrors gViewDirtyMask (view matrix changed bits)
        std::uint32_t passB{};               // S5b.2: mirrors gPassB (took the pad that kept wind0 on its row)
        // W5: the wind tail of the shadow PerView CB, verbatim. The setup shader stores these two
        // float4s at byte 192 of each page's 256-byte PageProj slot, which the page draw binds as
        // b1 — so the per-page shadow VS reads the same wind the gbuffer does. Field order matches
        // `cbuffer PerView` in gbuffer_common.hlsli / shadow_indirect_csm.hlsl.
        DirectX::XMFLOAT4 wind0{ 0.0f, 0.0f, 1.0f, 0.0f }; // time, prevTime, dirX, dirZ
        DirectX::XMFLOAT4 wind1{ 0.0f, 0.0f, 1.0f, 1.0f }; // swayAmp, swayFreq, gustMul, prevGustMul
        DirectX::XMFLOAT4 windFade{ 0.0f, 0.0f, 0.0f, 0.0f }; // camPos.xyz + fade end (0 = off); mirrors gWindFade
        DirectX::XMFLOAT4X4 vp[vsm::kMaxVirtualViews];
        DirectX::XMUINT4 viewLod[kViewLodVec4];               // per cull-view shadow LOD (packed 4/vec)
        // The per-(group,lod) mega ranges and the per-group LOD override used to sit here as arrays
        // sized kMaxMegaGroups -- the thing that capped the shadow path at 64 groups. They are SRVs
        // now (t9/t10), sized by the real group count. viewLod stays: 44 views, fixed.
    };
    // Region base in 5-uint arg units. MUST come from the ring's PHYSICAL region stride: the args
    // ring is grow-only (EnsureUavRing reuses a larger prior-level allocation), so after a level
    // switch regionBytes can exceed kMaxShadowViews*groups*20 — recomputing the base from the live
    // counts made frame regions 1..kFrameCount-1 read misaligned args (VSM shadow flicker on every
    // level switch that shrank the mesh-group count).
    const std::uint32_t argBaseElems = f * static_cast<std::uint32_t>(
        shadowGpu->IndirectArgsRegionBytes() / sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
    // Sub-scope: this dispatch is the per-page instance cull (O(pages x casters)), which scales with the
    // CASTER COUNT, while the draws below scale with triangles/fill. Split so a regression is attributable.
    // S5b.2: the same constants serve the pass-B invocation of the setup (only `passB` differs),
    // so the filler is a lambda both dispatches share.
    const auto writeSetupCB = [&](std::uint8_t* dst, std::uint32_t passB)
        {
            SetupCB cb{};
            cb.passB = passB;
            cb.numGroups = groups; cb.argBaseElems = argBaseElems; cb.numPages = vsm::kPoolPageCount;
            cb.numCasters = activeCasters; // static + GI when GI folding is active, else static only
            cb.forceAll = forceAll;        // page cache: 1 = mark every resident page dirty this frame
            cb.windDirtyMaxLevel = dec.windDirtyMaxLevel;
            cb.viewDirtyMask[0] = viewDirtyMask[0];
            cb.viewDirtyMask[1] = viewDirtyMask[1];
            // World-distance sway falloff (see gWindFade in the setup shader): reaches zero at
            // the outer extent of the last SWAYING level, so the rigid levels beyond pick up
            // exactly where the falloff ends and the clipmap blend band sees one geometry.
            // Off (w=0) when calm, when everything is rigid, or when everything sways (old mode).
            if (dec.windDirtyMaxLevel > 0 && dec.windDirtyMaxLevel < vsm::kNumClipmapLevels)
            {
                const DirectX::XMFLOAT3& cp = shadowGpu->LastCameraPos();
                const float fadeEnd = vsm::g_clipmapBaseExtent *
                    static_cast<float>(1u << (dec.windDirtyMaxLevel - 1u));
                cb.windFade = DirectX::XMFLOAT4(cp.x, cp.y, cp.z, fadeEnd);
            }
            cb.megaActive = useMega ? 1u : 0u; // per-view LOD: mega on = absolute mega start, off = lod-relative
            cb.numLods = kLods;
            cb.scatterActive = scatterActive ? 1u : 0u;
            cb.scatterLocals = scatterLocals ? 1u : 0u;
            cb.pageIdShift = singleDraw ? vsm::kPageIdShift : 0u; // must match the scatter CB's value
            cb.compactArgs = compactArgs ? 1u : 0u;
            // Fallback flat LOD (mega off): the per-page bind can't know each page's view, so all pages
            // use one LOD = the near directional (clipmap level 0) view's LOD.
            cb.flatLod = shadowGpu->ViewLodAt(render::kMaxShadowViews - vsm::kNumClipmapLevels);
            if (wind) // W5: null / swayAmp 0 leaves WindOffset returning exactly 0 (rigid casters)
            {
                cb.wind0 = DirectX::XMFLOAT4(wind->time, wind->prevTime,
                                             wind->windDirXZ.x, wind->windDirXZ.y);
                cb.wind1 = DirectX::XMFLOAT4(wind->swayAmplitude, wind->swayFrequency,
                                             wind->gustMul, wind->prevGustMul);
            }
            const std::uint32_t n = (viewCount < vsm::kMaxVirtualViews) ? viewCount : vsm::kMaxVirtualViews;
            for (std::uint32_t i = 0; i < n; ++i) { cb.vp[i] = views[i].viewProj; }
            // Per-view shadow LOD (cull-view layout, packed 4/uint4) + per-(group,lod) mega geometry.
            const std::vector<std::uint32_t>& vl = shadowGpu->ViewLod();
            for (std::uint32_t v = 0; v < render::kMaxShadowViews && v < vl.size(); ++v)
            {
                reinterpret_cast<std::uint32_t*>(cb.viewLod)[v] = vl[v];
            }
            std::memcpy(dst, &cb, sizeof(cb));
        };
    { GPU_SCOPE(cl, ProfilerScopes::kVsmPageSetup);
    RecordComputeDispatch(renderer, cl, pageSetupMat_.get(), static_cast<UINT>(sizeof(SetupCB)),
        [&](std::uint8_t* dst) { writeSetupCB(dst, 0u); },
        // POSITIONAL: this list IS t0..t11 in order. groupLodMega (t9) is static region 0;
        // groupLodOverride (t10) is per-frame -- it is rewritten every frame by RefreshChunkGroupLods.
        // t11 (S5b.2, pass B's counts) is a placeholder in pass A: the shader reads it only under gPassB.
        { physOwnerSrv_, rung0ArgsSrv_, boundsSrv, casterGroupSrv, physOwnerPrevSrv_, casterMetaSrv,
          pageGroupCountSrv_, perGroupSrv, pageScatterDynSrv_,
          shadowGpu->GroupLodMegaSrv(), shadowGpu->GroupLodOverrideSrv(f), pageGroupCountSrv_ },
        // u4 = the compacted-args counter. Its descriptor must be valid even when compaction is off
        // (the root signature declares the range); the shader only touches it when gCompactArgs != 0,
        // so a stand-in on the OOM path is never read.
        { pageDrawArgsUav_, pageProjUav_, pageVisibleListUav_, perPageDirtyUav_,
          pageArgCountUav_.ptr != 0 ? pageArgCountUav_ : perPageDirtyUav_ },
        D3D12_GPU_DESCRIPTOR_HANDLE{},
        vsm::kPoolPageCount, 1,
        pageDrawArgs_.Get());
    renderer->UAVBarrier(cl, pageProj_.Get());
    renderer->UAVBarrier(cl, pageVisibleList_.Get());
    } // end VsmPageRender.Setup scope

    // Page cache: after the setup has READ physOwnerPrev, snapshot this frame's physOwner into it for
    // next frame's new-page detection, and make the dirty bits readable by the gated clear (VS SRV).
    if (caching)
    {
        renderer->UAVBarrier(cl, perPageDirty_.Get());
        // Two markers around the copy — the split-point declaration guarantees physOwnerPrev is
        // still COPY_DEST while the copy records (see PrepareRenderPass).
        renderer->EmitPoint(cl, dec.pointCacheCopy);
        cl->CopyBufferRegion(physOwnerPrev_.Get(), 0, physOwner_.Get(), 0,
                             static_cast<UINT64>(vsm::kPoolPageCount) * sizeof(std::uint32_t));
        renderer->EmitPoint(cl, dec.pointCacheRead);
        cacheWarmup_ = false; // physOwnerPrev_ now holds this frame's owners -> new-page detect valid next frame
    }

    // Resident-set for the draw LOOP (opt-in, g_residentIterOnly): read this ring slot's
    // kFrameCount-old physOwner snapshot (owner != INVALID was resident; skip the rest — the ~free
    // pages are what make the full-pool loop expensive). Then snapshot THIS frame's physOwner for
    // kFrameCount frames later. ON by default (the CPU saving is worth the artifact); OFF →
    // residentSet null → iterate the whole pool (no snapshot latency, no motion flicker).
    // The single-draw path skips this entirely: it issues no per-page CPU work to skip, so there is
    // nothing for a stale snapshot to get wrong — that is the artifact this whole path buys back.
    const std::uint32_t* residentSet = nullptr;
    if (!singleDraw && vsm::g_residentIterOnly && residentReadback_[f])
    {
        residentSet = residentReadbackValid_[f] ? residentReadbackPtr_[f] : nullptr;
        // physOwner is already in its combined NPS|COPY_SOURCE read state from the base point —
        // the old named Transition here was an idempotent re-assert the compile never barriers.
        cl->CopyBufferRegion(residentReadback_[f].Get(), 0, physOwner_.Get(), 0,
                             static_cast<UINT64>(vsm::kPoolPageCount) * sizeof(std::uint32_t));
        residentReadbackValid_[f] = true;
    }

    // Consume: args -> INDIRECT_ARGUMENT, per-page list -> per-instance stream, and the projection
    // -> a VS SRV on the single-draw path (the VSM_PAGE shader reads it as StructuredBuffer<float4>)
    // or a per-page root CBV on the loop path.
    // One marker for the whole consume point: args -> INDIRECT_ARGUMENT, the projection/list to
    // their COMBINED read states (union keeps g_pageDrawSingle runtime-flippable against the
    // canonical table), and the pool back to DEPTH_WRITE (the light passes' declared reads leave
    // it in SRV). With the page cache the later clear is GATED to dirty pages; without it the
    // whole pool is cleared.
    renderer->EmitPoint(cl, dec.pointConsume);
    cl->OMSetRenderTargets(0, nullptr, FALSE, &poolDsv_);
    if (caching)
    {
        // One instance per pool page under a full-pool viewport; the VS emits a z=1.0 cell quad for a
        // dirty page (PerPageDirty) and a clipped degenerate for a clean one. Overwrites only dirty cells.
        const float poolTexels = static_cast<float>(vsm::kPoolTexels);
        D3D12_VIEWPORT fullVp{ 0.0f, 0.0f, poolTexels, poolTexels, 0.0f, 1.0f };
        D3D12_RECT fullSc{ 0, 0, static_cast<LONG>(vsm::kPoolTexels), static_cast<LONG>(vsm::kPoolTexels) };
        cl->RSSetViewports(1, &fullVp);
        cl->RSSetScissorRects(1, &fullSc);
        auto clearHandle = renderer->GetRenderContextPool()->Acquire();
        RenderContext& cctx = clearHandle.ref();
        cctx.srvTable[0] = renderer->StageSrvUavTable({ perPageDirtySrv_ }).gpu;
        pageClearMat_->Bind(cl, cctx, false);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cl->DrawInstanced(4u, vsm::kPoolPageCount, 0u, 0u);
    }
    else
    {
        cl->ClearDepthStencilView(poolDsv_, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    RenderContext& ctx = ctxHandle.ref();
    const bool maskedActive = shadowGpu->MaskedShadowsActive();
    if (singleDraw)
    {
        // ONE table based at t0. The VSM_PAGE root signature folds what the loop path splits across
        // t0 and t3, because Material::Bind keys tables by their base register and silently drops any
        // base >= RenderContext::kMaxBindings (4) — a second table at t4 would never be bound.
        // Only the descriptors actually used are staged (pageProj sits BEFORE the albedos precisely
        // so the unused albedo slots of the 20-wide range need no dummy descriptors), matching what
        // the loop path already does with its 16-wide albedo range.
        // ctx.cbv[1] stays UNSET on purpose: this permutation has no CBV(b1) at all, and pageProj_ is
        // held in NON_PIXEL_SHADER_RESOURCE here — binding it as a root CBV would contradict that.
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 20> tbl{};
        size_t n = 0;
        tbl[n++] = shadowGpu->InstanceReadSrv(f);   // t0
        if (maskedActive)
        {
            tbl[n++] = shadowGpu->CasterGroupSrv(); // t1
            tbl[n++] = shadowGpu->GroupMaskSrv();   // t2
            tbl[n++] = pageProjSrv_;                // t3
            const std::uint32_t albedos = shadowGpu->MaskedAlbedoCount();
            const auto& src = shadowGpu->MaskedAlbedoSrvs();
            for (std::uint32_t a = 0; a < albedos && n < tbl.size(); ++a) { tbl[n++] = src[a]; } // t4..
        }
        else
        {
            tbl[n++] = pageProjSrv_;                // t1
        }
        ctx.srvTable[0] = renderer->StageSrvUavTable(tbl, n).gpu;
    }
    else if (maskedActive)
    {
        ctx.cbv[1] = pageProj_->GetGPUVirtualAddress(); // initial b1 (overridden per page below)
        // C2: masked PSO — t0..t2 = instances + casterGroup + groupMask, t3.. = masked albedos.
        ctx.srvTable[0] = renderer->StageSrvUavTable({ shadowGpu->InstanceReadSrv(f),
                                                       shadowGpu->CasterGroupSrv(),
                                                       shadowGpu->GroupMaskSrv() }).gpu;
        ctx.srvTable[3] = renderer->StageSrvUavTable(shadowGpu->MaskedAlbedoSrvs(),
                                                     shadowGpu->MaskedAlbedoCount()).gpu;
    }
    else
    {
        ctx.cbv[1] = pageProj_->GetGPUVirtualAddress(); // initial b1 (overridden per page below)
        ctx.srvTable[0] = renderer->StageSrvUavTable({ shadowGpu->InstanceReadSrv(f) }).gpu; // unified copy (Step 2), else ring
    }

    // WHERE THIS PASS'S TIME IS *NOT* (measured 2026-07-30 on the 610-palm grove; raster = the pass
    // minus its Setup/Scatter sub-scopes, baseline 1.083 ms). Each of these was tried by deleting the
    // work outright, so the numbers are hard ceilings, not estimates:
    //   - masked alpha texture fetch removed entirely .... 1.079  (free)
    //   - wind sway removed from the shadow VS ........... 1.073  (free)
    //   - whole-pool depth clear skipped ................. 1.086  (free)
    //   - opaque/masked PSO split (opaque gets the null-PS
    //     depth fast path + CULL_BACK) ................... 1.11 ms and CPU 0.137 -> 0.23  (REGRESSION)
    // So there is nothing left to shave in the shaders, the sampler, or the clear: the cost is the raw
    // rasterization of thin alpha-card foliage into the pages (triangle setup + quad coverage + depth),
    // multiplied by how many pages each caster overlaps. The ONLY levers that move it are reducing
    // geometry x pages: coarser caster LOD (render::g_shadowLodBias, ~-28% at max) and a larger
    // vsm::g_clipmapBaseExtent (fewer, coarser pages). Do not spend time on a "cheaper masked shader".
    (singleDraw ? pageMat : indirectMat)->Bind(cl, ctx, false);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Per-instance stream = the per-PAGE visible list (the setup culled each page's casters into its
    // own slice). Each draw's StartInstanceLocation (baked per page/group by the setup) offsets into it.
    D3D12_VERTEX_BUFFER_VIEW visVBV{};
    visVBV.BufferLocation = pageVisibleList_->GetGPUVirtualAddress();
    visVBV.SizeInBytes = static_cast<UINT>(pageVisibleList_->GetDesc().Width);
    visVBV.StrideInBytes = sizeof(std::uint32_t);
    cl->IASetVertexBuffers(1, 1, &visVBV);

    // Mega path: bind the consolidated caster VB/IB ONCE (the per-group offsets were baked into the
    // args by the setup shader), so each page is a single ExecuteIndirect(maxCount=groups).
    if (useMega)
    {
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = shadowGpu->MegaVertexBuffer()->GetGPUVirtualAddress();
        vbv.SizeInBytes = shadowGpu->MegaVertexBytes();
        vbv.StrideInBytes = shadowGpu->MegaStride();
        cl->IASetVertexBuffers(0, 1, &vbv);
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = shadowGpu->MegaIndexBuffer()->GetGPUVirtualAddress();
        ibv.SizeInBytes = shadowGpu->MegaIndexBytes();
        ibv.Format = shadowGpu->MegaIndexFormat();
        cl->IASetIndexBuffer(&ibv);
    }

    if (singleDraw)
    {
        // ONE ExecuteIndirect over every (page, group) record — the args are already laid out
        // contiguously as [page][group], so this reads them unchanged with argOffset 0. The per-page
        // viewport became a clip-space scale/bias in the VS and the per-page scissor became the four
        // SV_ClipDistance page borders, so a single full-pool viewport serves all 1024 cells.
        //
        // Free and clean pages carry InstanceCount = 0, written by the setup CS THIS frame, so they
        // cost nothing AND cannot be stale — which is exactly what removes the g_residentIterOnly
        // blink: there is no longer any CPU-side skip for a kFrameCount-old snapshot to get wrong.
        const float poolTexels = static_cast<float>(vsm::kPoolTexels);
        D3D12_VIEWPORT vp{ 0.0f, 0.0f, poolTexels, poolTexels, 0.0f, 1.0f };
        D3D12_RECT     sc{ 0, 0, static_cast<LONG>(vsm::kPoolTexels), static_cast<LONG>(vsm::kPoolTexels) };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        // With compaction the counter caps the walk at the records actually appended (a few hundred
        // instead of pages x groups); maxCount stays the buffer capacity, which is what D3D12 clamps
        // the counter against. Without it, every fixed-layout record is walked and the empty ones are
        // zero-instance no-ops.
        renderer->ExecuteIndirect(cl, sig, vsm::kPoolPageCount * groups * render::kMaxShadowLods,
                                  pageDrawArgs_.Get(), 0,
                                  compactArgs ? pageArgCount_.Get() : nullptr, 0);
        if (dec.hzb)
        {
            RecordHzbPassB(renderer, cl, shadowGpu, views, viewCount, dec, sig, pageMat, ctx,
                           static_cast<UINT>(sizeof(SetupCB)), writeSetupCB);
        }
        return;
    }

    const auto& groupMeshes = shadowGpu->GroupMeshes();
    const D3D12_GPU_VIRTUAL_ADDRESS projVA = pageProj_->GetGPUVirtualAddress();
    const UINT64 argStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    for (std::uint32_t p = 0; p < vsm::kPoolPageCount; ++p)
    {
        if (residentSet && residentSet[p] == 0xFFFFFFFFu) { continue; } // free page (stale snapshot) -> skip
        const UINT gx = p % vsm::kPoolPagesPerAxis;
        const UINT gy = p / vsm::kPoolPagesPerAxis;
        const float ox = static_cast<float>(gx * vsm::kPageSize);
        const float oy = static_cast<float>(gy * vsm::kPageSize);
        D3D12_VIEWPORT vp{ ox, oy, static_cast<float>(vsm::kPageSize), static_cast<float>(vsm::kPageSize), 0.0f, 1.0f };
        D3D12_RECT sc{ static_cast<LONG>(ox), static_cast<LONG>(oy),
                       static_cast<LONG>(ox) + static_cast<LONG>(vsm::kPageSize),
                       static_cast<LONG>(oy) + static_cast<LONG>(vsm::kPageSize) };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->SetGraphicsRootConstantBufferView(0, projVA + static_cast<UINT64>(p) * 256u); // b1 = this page's projection

        if (useMega)
        {
            // All of this page's args are contiguous (one per VIRTUAL group = group x LOD
            // bucket); empty buckets draw 0 instances (no-op).
            const UINT64 argOff = static_cast<UINT64>(p) * groups * render::kMaxShadowLods * argStride;
            renderer->ExecuteIndirect(cl, sig, groups * render::kMaxShadowLods,
                                      pageDrawArgs_.Get(), argOff, nullptr, 0);
            continue;
        }

        // Fallback (heterogeneous meshes): bind + draw per (mesh-group, LOD bucket) -- the
        // per-mesh IB is what varies per bucket here, so each bucket is its own bind + draw.
        // Empty buckets carry InstanceCount 0 and draw nothing.
        for (std::uint32_t g = 0; g < groups; ++g)
        {
            const Mesh* mesh = (g < groupMeshes.size()) ? groupMeshes[g] : nullptr;
            if (!mesh) { continue; }
            ID3D12Resource* vb = mesh->GetVertexBufferResource();
            if (!vb) { continue; }
            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = vb->GetGPUVirtualAddress();
            vbv.SizeInBytes = static_cast<UINT>(vb->GetDesc().Width);
            vbv.StrideInBytes = mesh->GetVertexStride();
            cl->IASetVertexBuffers(0, 1, &vbv);
            for (std::uint32_t lod = 0; lod < render::kMaxShadowLods; ++lod)
            {
                ID3D12Resource* ib = mesh->GetLodIndexBufferResource(
                    (g < shadowGpu->StaticGroupCount()) ? mesh->ClampExplicitLod(lod) : 0u);
                if (!ib) { continue; }
                D3D12_INDEX_BUFFER_VIEW ibv{};
                ibv.BufferLocation = ib->GetGPUVirtualAddress();
                ibv.SizeInBytes = static_cast<UINT>(ib->GetDesc().Width);
                ibv.Format = mesh->GetIndexFormat();
                cl->IASetIndexBuffer(&ibv);
                const UINT64 argOff = static_cast<UINT64>(
                    (p * groups + g) * render::kMaxShadowLods + lod) * argStride;
                renderer->ExecuteIndirect(cl, sig, 1, pageDrawArgs_.Get(), argOff, nullptr, 0);
            }
        }
    }
}

// Occlusion plan S5b.2, after pass A's single draw: the pool pyramid from the pages pass A
// rendered, the deferred pairs retested against it (survivors appended after pass A's list
// entries), pass B's args from the post cull's counts, pass B into the same pages, the counters
// out to the readout. Same command list, same viewport/targets/streams as pass A -- only the
// graphics PSO has to be re-bound after the compute dispatches in between. Every point the
// builder declared is emitted whatever the data says.
void VirtualShadowMap::RecordHzbPassB(Renderer* renderer, ID3D12GraphicsCommandList* cl, ShadowGpuData* shadowGpu,
    const vsm::ViewProjEntry* views, std::uint32_t viewCount, const PageRenderDecisions& dec,
    ID3D12CommandSignature* sig, Material* pageMat, RenderContext& ctx, UINT setupCbSize,
    const std::function<void(std::uint8_t*, std::uint32_t)>& writeSetupCB)
{
    const UINT f = renderer->GetCurrentFrameIndex();
    const std::uint32_t groups = shadowGpu->MeshGroupCount();
    const std::uint32_t activeCasters = shadowGpu->ActiveCasterCount();
    const D3D12_GPU_DESCRIPTOR_HANDLE noSampler{};
    constexpr std::uint32_t kLods = render::kMaxShadowLods;

    // 1. The pyramid: one 16x16 group per physical page, dirty pages only (all resident + the
    //    free ones zeroed on a full build).
    {
        GPU_SCOPE(cl, ProfilerScopes::kVsmHzbBuild);
        renderer->EmitPoint(cl, dec.pointHzbBuild);
        struct BuildCB { std::uint32_t full, p0, p1, p2; };
        auto cb = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(BuildCB)), render::kConstantBufferAlignment);
        if (cb.cpu)
        {
            const BuildCB c{ dec.hzbFull ? 1u : 0u, 0u, 0u, 0u };
            std::memcpy(cb.cpu, &c, sizeof(c));
            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& rc = h.ref();
            rc.cbv[0] = cb.gpu;
            rc.srvTable[0] = renderer->StageSrvUavTable({ poolSrv_, physOwnerSrv_, perPageDirtySrv_ }).gpu;
            rc.uavTable[0] = renderer->StageSrvUavTable({ hzbMipUav_[0], hzbMipUav_[1], hzbMipUav_[2], hzbMipUav_[3],
                                                          hzbMipUav_[4], hzbMipUav_[5], hzbMipUav_[6] }).gpu;
            rc.samplerTable[0] = noSampler;
            hzbBuildMat_->Bind(cl, rc);
            cl->Dispatch(vsm::kPoolPagesPerAxis, vsm::kPoolPagesPerAxis, 1); // numthreads(16,16,1) = one page
            renderer->UAVBarrier(cl, vsmHzb_.Get());
        }
    }

    // 2. The deferred pairs against it, then pass B's args from the counts they produced.
    {
        GPU_SCOPE(cl, ProfilerScopes::kVsmHzbPost);
        renderer->EmitPoint(cl, dec.pointHzbPost);
        struct PostCB
        {
            std::uint32_t numCasters, numGroups, numLevels, pageIdShift;
            std::uint32_t perInstanceLod, deferredCap, pad0, pad1;
            DirectX::XMFLOAT4X4 viewProj[vsm::kMaxVirtualViews];
            DirectX::XMUINT4 targetLod[(vsm::kMaxVirtualViews + 3) / 4];
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE boundsSrv = shadowGpu->UnifiedBoundsSrv(f);
        RecordComputeDispatch(renderer, cl, hzbPostMat_.get(), static_cast<UINT>(sizeof(PostCB)),
            [&](std::uint8_t* dst)
            {
                PostCB c{};
                c.numCasters = activeCasters;
                c.numGroups = groups;
                c.numLevels = vsm::kNumClipmapLevels;
                c.pageIdShift = vsm::kPageIdShift; // single draw is a precondition of dec.hzb
                c.perInstanceLod = vsm::g_perInstanceCasterLod ? 1u : 0u;
                c.deferredCap = deferredCap_;
                const std::uint32_t nv = (viewCount < vsm::kMaxVirtualViews) ? viewCount : vsm::kMaxVirtualViews;
                for (std::uint32_t v = 0; v < nv; ++v) { c.viewProj[v] = views[v].viewProj; }
                // Clipmap targets only (the scatter defers no local pair): target t = level t.
                for (std::uint32_t t = 0; t < vsm::kNumClipmapLevels; ++t)
                {
                    reinterpret_cast<std::uint32_t*>(&c.targetLod[0])[t] = shadowGpu->ViewLodAt(vsm::kNumLocalVirtualViews + t + 4u);
                }
                std::memcpy(dst, &c, sizeof(c));
            },
            { boundsSrv, shadowGpu->CasterGroupSrv(), shadowGpu->CasterMetaSrv(f), pageTableSrv_, shadowGpu->PerGroupSrv(),
              shadowGpu->CasterLodSrv(f), deferredPairsSrv_, pageGroupCountSrv_, perPageDirtySrv_, hzbSrv_ },
            { pageGroupCountBUav_, pageVisibleListUav_, hzbCountersUav_ },
            noSampler,
            deferredCap_, 1,
            pageVisibleList_.Get());
        renderer->UAVBarrier(cl, pageGroupCountB_.Get());
        renderer->UAVBarrier(cl, hzbCounters_.Get());

        renderer->EmitPoint(cl, dec.pointSetupB);
        if (dec.compactArgs)
        {
            const UINT zero[4] = { 0u, 0u, 0u, 0u };
            cl->ClearUnorderedAccessViewUint(renderer->StageSrvUavTable({ pageArgCountBUav_ }).gpu,
                                             pageArgCountBUav_, pageArgCountB_.Get(), zero, 0, nullptr);
            renderer->UAVBarrier(cl, pageArgCountB_.Get());
        }
        // The setup in pass-B mode: the same constants but `passB`, t11 = pass B's counts, u0/u4 =
        // pass B's args + counter. t1 (Rung 0 args, never read by this shader) and u1/u2 (the
        // projections and the list, written by pass A only) get placeholders: those resources are
        // in their draw states now, and a table may not hold a hole.
        RecordComputeDispatch(renderer, cl, pageSetupMat_.get(), setupCbSize,
            [&](std::uint8_t* dst) { writeSetupCB(dst, 1u); },
            { physOwnerSrv_, physOwnerSrv_, boundsSrv, shadowGpu->CasterGroupSrv(), physOwnerPrevSrv_, shadowGpu->CasterMetaSrv(f),
              pageGroupCountSrv_, shadowGpu->PerGroupSrv(), pageScatterDynSrv_,
              shadowGpu->GroupLodMegaSrv(), shadowGpu->GroupLodOverrideSrv(f), pageGroupCountBSrv_ },
            { pageDrawArgsBUav_, perPageDirtyUav_, perPageDirtyUav_, perPageDirtyUav_,
              dec.compactArgs ? pageArgCountBUav_ : perPageDirtyUav_ },
            noSampler,
            vsm::kPoolPageCount, 1,
            pageDrawArgsB_.Get());
    }

    // 3. Pass B: the survivors into the same pages. Viewport, scissor, targets and both vertex
    //    streams are still pass A's; only the PSO (replaced by the compute dispatches) is re-bound.
    {
        GPU_SCOPE(cl, ProfilerScopes::kVsmPageDrawB);
        renderer->EmitPoint(cl, dec.pointDrawB);
        pageMat->Bind(cl, ctx, false);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        renderer->ExecuteIndirect(cl, sig, vsm::kPoolPageCount * groups * kLods,
                                  pageDrawArgsB_.Get(), 0,
                                  dec.compactArgs ? pageArgCountB_.Get() : nullptr, 0);
        if (hzbReadback_[f])
        {
            cl->CopyBufferRegion(hzbReadback_[f].Get(), 0, hzbCounters_.Get(), 0, 4u * sizeof(std::uint32_t));
        }
        renderer->EmitPoint(cl, dec.pointHzbRestore);
    }
}

void VirtualShadowMap::PollPageRequestDebug(Renderer* renderer)
{
    if (!renderer) { return; }
    // Re-arm a new sample once the cooldown elapses (see RecordDebugReadback).
    if (debugReadbackState_ == 2 &&
        renderer->GetTotalFrameNumber() >= debugReadbackDoneFrame_ + kRequestReadbackPeriod)
    {
        debugReadbackState_ = 0;
        return;
    }
    if (debugReadbackState_ != 1 || !debugReadback_) { return; }
    if (renderer->GetTotalFrameNumber() < debugReadbackFrame_ + render::kFrameCount) { return; }

    const size_t reqBytes   = static_cast<size_t>(vsm::kRequestWords) * sizeof(std::uint32_t);
    const size_t cntBytes   = 4 * sizeof(std::uint32_t);
    const size_t ownerBytes = static_cast<size_t>(vsm::kPoolPageCount) * sizeof(std::uint32_t);
    D3D12_RANGE readRange{ 0, reqBytes + cntBytes + ownerBytes };
    void* mapped = nullptr;
    if (FAILED(debugReadback_->Map(0, &readRange, &mapped)) || !mapped)
    {
        debugReadbackState_ = 2;
        return;
    }
    const auto* words = reinterpret_cast<const std::uint32_t*>(mapped);
    const auto* counters = reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::uint8_t*>(mapped) + reqBytes);

    auto isSet = [&](std::uint32_t page) { return (words[page >> 5u] & (1u << (page & 31u))) != 0u; };

    // Total requested pages + a per-mip-level histogram across all local views (confirms mip
    // selection: far receivers should populate the coarse levels). Each view occupies
    // kPagesPerView contiguous bits, laid out level 0..kMaxMipLevel via LevelPageOffset.
    std::uint32_t total = 0;
    std::uint32_t perLevel[vsm::kNumMipLevels] = {};
    for (std::uint32_t v = 0; v < vsm::kMaxVirtualViews; ++v)
    {
        const std::uint32_t viewBase = v * vsm::kPagesPerView;
        for (std::uint32_t lvl = 0; lvl < vsm::kNumMipLevels; ++lvl)
        {
            const std::uint32_t base = viewBase + vsm::LevelPageOffset(lvl);
            const std::uint32_t n = vsm::LevelPageCount(lvl);
            for (std::uint32_t p = 0; p < n; ++p) { if (isSet(base + p)) { ++perLevel[lvl]; ++total; } }
        }
    }

    // Per-view request counts for the 8 spot views (slots 0..7) — spots with 0 pages are the ones
    // whose receivers the request never covers (starved -> no VSM shadow).
    std::uint32_t perSpot[8] = {};
    for (std::uint32_t v = 0; v < 8; ++v)
    {
        const std::uint32_t viewBase = v * vsm::kPagesPerView;
        for (std::uint32_t p = 0; p < vsm::kPagesPerView; ++p) { if (isSet(viewBase + p)) { ++perSpot[v]; } }
    }

    // Alloc counters (VSM_CNT_*): resident = carried-over survivors + newly allocated this frame.
    const std::uint32_t freeCount = counters[0]; // post-allocate: leftover free pages (may underflow if pool full)
    const std::uint32_t newAlloc  = counters[1]; // pages allocated (needs-render) this frame
    const std::uint32_t failCount = counters[2]; // requested pages that couldn't allocate (pool full)
    const std::uint32_t resident  = counters[3] + newAlloc;
    (void)freeCount;

    // Physical-page ownership snapshot for the dev-window grid (physical -> owning virtual page).
    const auto* owners = reinterpret_cast<const std::uint32_t*>(
        static_cast<const std::uint8_t*>(mapped) + reqBytes + cntBytes);
    physOwnerSnapshot_.assign(owners, owners + vsm::kPoolPageCount);

    const D3D12_RANGE noWrite{ 0, 0 };
    debugReadback_->Unmap(0, &noWrite);

    // Publish the live stats the dev-window "VSM" tab reads.
    stats_.valid = true;
    stats_.sampleFrame = debugReadbackFrame_;
    stats_.requested = total;
    stats_.resident = resident;
    stats_.newAlloc = newAlloc;
    stats_.fail = failCount;
    for (std::uint32_t l = 0; l < vsm::kNumMipLevels; ++l) { stats_.perLevel[l] = perLevel[l]; }

    // File log — every sample (no DBWIN throttle), logs/vsm_pages.log. The per-view residency
    // breakdown comes from the physOwner snapshot (owner id / kPagesPerView = view), which is what
    // the round-trip perf investigation needs: WHICH views' resident sets grew
    // (docs/bug_shadow_lod_bias_perf.md §4). Truncated on the first sample of a run.
    if (vsm::g_logPageStats)
    {
        std::uint32_t ownersTotal = 0, ownersLocal = 0;
        std::uint32_t ownersClip[vsm::kNumClipmapLevels] = {};
        for (std::uint32_t p = 0; p < vsm::kPoolPageCount; ++p)
        {
            const std::uint32_t o = physOwnerSnapshot_[p];
            if (o == 0xFFFFFFFFu) { continue; } // VSM_INVALID = free
            ++ownersTotal;
            const std::uint32_t v = o / vsm::kPagesPerView;
            if (v < vsm::kNumLocalVirtualViews) { ++ownersLocal; }
            else if (v - vsm::kNumLocalVirtualViews < vsm::kNumClipmapLevels)
            {
                ++ownersClip[v - vsm::kNumLocalVirtualViews];
            }
        }
        diag::WriteArtifactf("vsm_pages.log", diag::ArtifactMode::PerRunTruncate,
            "frame=%llu bias=%d req=%u (L0=%u L1=%u L2=%u L3=%u L4=%u) resident=%u new=%u fail=%u"
            " | owners: total=%u local=%u clip=[%u %u %u %u %u %u %u %u]\n",
            static_cast<unsigned long long>(debugReadbackFrame_), render::g_shadowLodBias,
            total, perLevel[0], perLevel[1], perLevel[2], perLevel[3], perLevel[4],
            resident, newAlloc, failCount,
            ownersTotal, ownersLocal,
            ownersClip[0], ownersClip[1], ownersClip[2], ownersClip[3],
            ownersClip[4], ownersClip[5], ownersClip[6], ownersClip[7]);
    }

    // Session-log mirror — OFF by default (vsm::g_logPageStats), throttled independently of the
    // (faster) stats sampling so a captured stress/dev run is not flooded. The on-screen readout
    // updates every sample regardless; this only mirrors it into the log when explicitly enabled.
    if (vsm::g_logPageStats && renderer->GetTotalFrameNumber() >= debugLoggedFrame_ + kDbwinLogPeriod)
    {
        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "[VSM] request %u (L0=%u L1=%u L2=%u L3=%u L4=%u) | resident=%u new=%u fail=%u | spots[%u %u %u %u %u %u %u %u] (pool=%u).\n",
            total, perLevel[0], perLevel[1], perLevel[2], perLevel[3], perLevel[4],
            resident, newAlloc, failCount,
            perSpot[0], perSpot[1], perSpot[2], perSpot[3], perSpot[4], perSpot[5], perSpot[6], perSpot[7],
            vsm::kPoolPageCount);
        logging::WriteRaw(logging::LogLevel::Debug, logging::LogCategory::RenderShadow, buf);
        debugLoggedFrame_ = renderer->GetTotalFrameNumber();
    }
    debugReadbackState_ = 2;
    debugReadbackDoneFrame_ = renderer->GetTotalFrameNumber();
}
