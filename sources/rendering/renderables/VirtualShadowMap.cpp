#include "rendering/renderables/VirtualShadowMap.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "rendering/core/Renderer.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/renderables/ShadowGpuData.h"
#include "rendering/meshes/Mesh.h"

void VirtualShadowMap::EnsureResources(Renderer* renderer)
{
    if (IsAllocated()) { return; }
    if (!renderer || !renderer->GetDevice()) { return; }
    ID3D12Device* dev = renderer->GetDevice();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // --- Physical page pool: one D16 depth atlas (R16_TYPELESS -> D16 DSV + R16_UNORM SRV),
    // mirroring the existing shadow atlases. PERSISTENT; the pool IS the cache. ---
    {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = vsm::kPoolTexels;
        rd.Height = vsm::kPoolTexels;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16_TYPELESS;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D16_UNORM;
        cv.DepthStencil.Depth = 1.0f;

        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(pagePool_.GetAddressOf()))) || !pagePool_)
        {
            pagePool_.Reset();
            return;
        }
        pagePool_->SetName(L"VSM.PagePool");
        renderer->SetResourceState(pagePool_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
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

        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(pageTable_.GetAddressOf()))) || !pageTable_)
        {
            pageTable_.Reset();
            pagePool_.Reset();
            return;
        }
        pageTable_->SetName(L"VSM.PageTable");
        renderer->SetResourceState(pageTable_.Get(), D3D12_RESOURCE_STATE_COMMON);
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
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(requestBuffer_.GetAddressOf()))) || !requestBuffer_)
        {
            requestBuffer_.Reset(); pageTable_.Reset(); pagePool_.Reset();
            return;
        }
        requestBuffer_->SetName(L"VSM.PageRequest");
        renderer->SetResourceState(requestBuffer_.Get(), D3D12_RESOURCE_STATE_COMMON);
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
        dsv.Format = DXGI_FORMAT_D16_UNORM;
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
        poolSd.Format = DXGI_FORMAT_R16_UNORM;
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
    OutputDebugStringA(buf);
}

namespace
{
    // A persistent DEFAULT-heap RWStructuredBuffer<uint>[numUints] (ALLOW_UNORDERED_ACCESS,
    // created COMMON). Used for the Step-20 allocation state buffers.
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

    physOwner_     = CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.PhysOwner");
    physLastFrame_ = CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.PhysLastFrame");
    freeList_      = CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.FreeList");
    needsRender_   = CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.NeedsRender");
    allocCounters_ = CreateUavUintBuffer(dev, 4u, L"VSM.AllocCounters");
    if (!physOwner_ || !physLastFrame_ || !freeList_ || !needsRender_ || !allocCounters_)
    {
        physOwner_.Reset(); physLastFrame_.Reset(); freeList_.Reset();
        needsRender_.Reset(); allocCounters_.Reset();
        return;
    }
    renderer->SetResourceState(physOwner_.Get(), D3D12_RESOURCE_STATE_COMMON);
    renderer->SetResourceState(physLastFrame_.Get(), D3D12_RESOURCE_STATE_COMMON);
    renderer->SetResourceState(freeList_.Get(), D3D12_RESOURCE_STATE_COMMON);
    renderer->SetResourceState(needsRender_.Get(), D3D12_RESOURCE_STATE_COMMON);
    renderer->SetResourceState(allocCounters_.Get(), D3D12_RESOURCE_STATE_COMMON);

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
    pageSetupMat_  = makeCompute(L"shaders/vsm_page_setup_cs.hlsl");

    auto ok = [](const std::shared_ptr<Material>& m) { return m && m->GetPipelineState(); };
    if (!ok(pageRequestClearMat_) || !ok(pageRequestMat_))
    {
        OutputDebugStringA("[VSM] page-request PSO creation FAILED (shader compile?).\n");
        pageRequestClearMat_.reset();
        pageRequestMat_.reset();
    }
    else if (!ok(allocInitMat_) || !ok(allocTouchMat_) || !ok(allocFreeMat_) || !ok(allocMapMat_) || !ok(pageSetupMat_))
    {
        OutputDebugStringA("[VSM] page-alloc/setup PSO creation FAILED (shader compile?).\n");
        allocInitMat_.reset(); allocTouchMat_.reset(); allocFreeMat_.reset(); allocMapMat_.reset(); pageSetupMat_.reset();
    }
    else
    {
        OutputDebugStringA("[VSM] page-request + page-alloc + page-setup shaders ready.\n");
    }
}

void VirtualShadowMap::RecordPageRequest(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const vsm::PageRequestConstants& constants, D3D12_CPU_DESCRIPTOR_HANDLE depthSrv,
    UINT screenW, UINT screenH)
{
    if (!renderer || !cl || !IsAllocated() || depthSrv.ptr == 0 || screenW == 0 || screenH == 0) { return; }
    EnsureShaderResources(renderer);
    if (!pageRequestClearMat_ || !pageRequestMat_) { return; }

    // The request bitfield is UAV-written by both dispatches (this pass owns its state).
    renderer->Transition(cl, requestBuffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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

void VirtualShadowMap::RecordPageAllocate(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !cl || !IsAllocated() || !allocCounters_ || !allocUavHeap_) { return; }
    EnsureShaderResources(renderer);
    if (!allocInitMat_ || !allocTouchMat_ || !allocFreeMat_ || !allocMapMat_) { return; }

    const std::uint32_t numEntries = vsm::kPageTableEntries;
    const std::uint32_t numPages = vsm::kPoolPageCount;
    const std::uint32_t curFrame = static_cast<std::uint32_t>(renderer->GetTotalFrameNumber());

    // These persistent buffers live in UNORDERED_ACCESS between frames; a debug-readback frame
    // parks request/counters in COPY_SOURCE, so re-assert UAV at the top (idempotent otherwise).
    renderer->Transition(cl, pageTable_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, physOwner_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, physLastFrame_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, freeList_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, needsRender_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, allocCounters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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

    // Step 20 debug: sample the request bitfield + alloc counters a few frames later.
    RecordDebugReadback(renderer, cl);
}

void VirtualShadowMap::RecordDebugReadback(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (debugReadbackState_ != 0 || renderer->GetTotalFrameNumber() <= render::kFrameCount) { return; }

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

    renderer->Transition(cl, requestBuffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(debugReadback_.Get(), 0, requestBuffer_.Get(), 0, reqBytes);
    renderer->Transition(cl, allocCounters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(debugReadback_.Get(), reqBytes, allocCounters_.Get(), 0, cntBytes);
    if (physOwner_)
    {
        renderer->Transition(cl, physOwner_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyBufferRegion(debugReadback_.Get(), reqBytes + cntBytes, physOwner_.Get(), 0, ownerBytes);
    }
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
    if (!pageDrawArgs_ || groups > renderGroups_)
    {
        pageDrawArgs_ = CreateUavUintBuffer(dev, vsm::kPoolPageCount * groups * argUints, L"VSM.PageDrawArgs");
        renderGroups_ = groups;
        if (pageDrawArgs_) { renderer->SetResourceState(pageDrawArgs_.Get(), D3D12_RESOURCE_STATE_COMMON); }
        pageDrawArgsUav_ = {}; // force descriptor rebuild
    }
    if (!pageProj_)
    {
        // 64 uints (256 bytes) per page: a float4x4 + padding for root-CBV alignment.
        pageProj_ = CreateUavUintBuffer(dev, vsm::kPoolPageCount * 64u, L"VSM.PageProj");
        if (pageProj_) { renderer->SetResourceState(pageProj_.Get(), D3D12_RESOURCE_STATE_COMMON); }
        pageProjUav_ = {};
    }
    // Per-page-cull plan (Step 1): per (page, caster-slot) visible list, sized pool-pages x casters.
    // Allocated dormant here; the setup shader writes it and the draw binds it starting Step 2.
    if (!pageVisibleList_ || casters > renderCasters_)
    {
        const std::uint32_t cap = casters > 0u ? casters : 1u;
        pageVisibleList_ = CreateUavUintBuffer(dev, vsm::kPoolPageCount * cap, L"VSM.PageVisibleList");
        renderCasters_ = cap;
        if (pageVisibleList_) { renderer->SetResourceState(pageVisibleList_.Get(), D3D12_RESOURCE_STATE_COMMON); }
        pageVisibleListUav_ = {}; // force descriptor rebuild
    }
    // Page-caching plan (Step 1): fixed-size (kPoolPageCount) caching buffers, allocated once.
    // physOwnerPrev_ = last frame's physOwner (new-page detect); perPageDirty_ = per-page dirty bit.
    if (!physOwnerPrev_)
    {
        physOwnerPrev_ = CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.PhysOwnerPrev");
        if (physOwnerPrev_) { renderer->SetResourceState(physOwnerPrev_.Get(), D3D12_RESOURCE_STATE_COMMON); }
        physOwnerPrevSrv_ = {};
    }
    if (!perPageDirty_)
    {
        perPageDirty_ = CreateUavUintBuffer(dev, vsm::kPoolPageCount, L"VSM.PerPageDirty");
        if (perPageDirty_) { renderer->SetResourceState(perPageDirty_.Get(), D3D12_RESOURCE_STATE_COMMON); }
        perPageDirtyUav_ = {}; perPageDirtySrv_ = {};
    }
    if (!pageDrawArgs_ || !pageProj_ || !physOwnerPrev_ || !perPageDirty_) { return; }

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
                          perPageDirtyUav_.ptr == 0 || perPageDirtySrv_.ptr == 0;
    if (!needHeap) { return; }

    if (!renderHeap_)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 8; // + physOwnerPrevSrv, perPageDirtyUav, perPageDirtySrv (page cache)
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
    }

    cachedRung0Args_ = rung0Args;
}

void VirtualShadowMap::RecordPageRender(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    ShadowGpuData* shadowGpu, const vsm::ViewProjEntry* views, std::uint32_t viewCount)
{
    if (!renderer || !cl || !IsAllocated() || !shadowGpu || !views) { return; }
    EnsureShaderResources(renderer);
    if (!pageSetupMat_) { return; }
    if (!shadowGpu->IndirectDrawReady()) { return; } // needs this frame's Rung 0 cull output
    EnsureRenderResources(renderer, shadowGpu);
    if (!pageDrawArgs_ || !pageProj_ || !renderHeap_ || !pageVisibleList_) { return; }

    Material* indirectMat = shadowGpu->IndirectShadowMaterial();
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
    if (boundsSrv.ptr == 0 || casterGroupSrv.ptr == 0 || pageVisibleListUav_.ptr == 0) { return; }
    const std::uint32_t activeCasters = shadowGpu->ActiveCasterCount();

    // Consolidated caster VB/IB (built once at level load, ShadowGpuData::EnsureMegaBuffer): when
    // ready, the draw loop below binds geometry ONCE + issues one ExecuteIndirect(maxCount=groups)
    // per page instead of a bind + draw per (page, mesh-group).
    const bool useMega = shadowGpu->MegaReady() &&
                         shadowGpu->MegaVertexBuffer() && shadowGpu->MegaIndexBuffer();

    // --- Setup compute: per physical page, build the off-center projection AND cull the caster set
    // to the page's frustum, writing a per-page compacted visible list + per-page draw args. Reads
    // Rung0 args (per-group index count) + physOwner + the unified world AABBs + per-caster group. ---
    renderer->Transition(cl, shadowGpu->IndirectArgsBuffer(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, physOwner_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, pageDrawArgs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, pageProj_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, pageVisibleList_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    constexpr std::uint32_t kMaxMegaGroups = 64u; // matches VSM_MAX_SETUP_GROUPS in vsm_page_setup_cs.hlsl
    struct SetupCB
    {
        std::uint32_t numGroups, argBaseElems, numPages, numCasters;
        DirectX::XMFLOAT4X4 vp[vsm::kMaxVirtualViews];
        DirectX::XMUINT4 groupMega[kMaxMegaGroups]; // x=baseVertex, y=startIndex (0 = per-mesh args)
    };
    const std::uint32_t argBaseElems = f * render::kMaxShadowViews * groups; // region f, 5-uint arg units
    RecordComputeDispatch(renderer, cl, pageSetupMat_.get(), static_cast<UINT>(sizeof(SetupCB)),
        [&](std::uint8_t* dst)
        {
            SetupCB cb{};
            cb.numGroups = groups; cb.argBaseElems = argBaseElems; cb.numPages = vsm::kPoolPageCount;
            cb.numCasters = activeCasters; // static + GI when GI folding is active, else static only
            const std::uint32_t n = (viewCount < vsm::kMaxVirtualViews) ? viewCount : vsm::kMaxVirtualViews;
            for (std::uint32_t i = 0; i < n; ++i) { cb.vp[i] = views[i].viewProj; }
            // Mega offsets (0 unless the consolidated path is active) so the setup rebases each
            // group's draw args into the mega VB/IB.
            const std::uint32_t gm = (groups < kMaxMegaGroups) ? groups : kMaxMegaGroups;
            for (std::uint32_t g = 0; useMega && g < gm; ++g)
            {
                cb.groupMega[g].x = shadowGpu->GroupBaseVertex(g);
                cb.groupMega[g].y = shadowGpu->GroupStartIndex(g);
            }
            std::memcpy(dst, &cb, sizeof(cb));
        },
        { physOwnerSrv_, rung0ArgsSrv_, boundsSrv, casterGroupSrv },
        { pageDrawArgsUav_, pageProjUav_, pageVisibleListUav_ },
        D3D12_GPU_DESCRIPTOR_HANDLE{},
        vsm::kPoolPageCount, 1,
        pageDrawArgs_.Get());
    renderer->UAVBarrier(cl, pageProj_.Get());
    renderer->UAVBarrier(cl, pageVisibleList_.Get());

    // Resident-set for the draw loop (opt-in, g_residentIterOnly): read this ring slot's
    // kFrameCount-old physOwner snapshot (owner != INVALID was resident; skip the rest — the ~free
    // pages are what make the full-pool loop expensive). Then snapshot THIS frame's physOwner for
    // kFrameCount frames later. OFF by default → residentSet null → iterate the whole pool (no
    // snapshot latency, no motion flicker).
    const std::uint32_t* residentSet = nullptr;
    if (vsm::g_residentIterOnly && residentReadback_[f])
    {
        residentSet = residentReadbackValid_[f] ? residentReadbackPtr_[f] : nullptr;
        renderer->Transition(cl, physOwner_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyBufferRegion(residentReadback_[f].Get(), 0, physOwner_.Get(), 0,
                             static_cast<UINT64>(vsm::kPoolPageCount) * sizeof(std::uint32_t));
        residentReadbackValid_[f] = true;
    }

    // Consume: args -> INDIRECT_ARGUMENT, projection -> root CBV, per-page list -> per-instance stream.
    renderer->Transition(cl, pageDrawArgs_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    renderer->Transition(cl, pageProj_.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    renderer->Transition(cl, pageVisibleList_.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // --- Render: clear the whole pool (Step 23 will gate to changed pages), then draw each pool
    // page's casters into its 128² cell via ExecuteIndirect. The pool is left in SRV by the light
    // passes' declared reads, so transition it back to DEPTH_WRITE here (manual, like the buffers). ---
    renderer->Transition(cl, pagePool_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cl->OMSetRenderTargets(0, nullptr, FALSE, &poolDsv_);
    cl->ClearDepthStencilView(poolDsv_, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    RenderContext& ctx = ctxHandle.ref();
    ctx.cbv[1] = pageProj_->GetGPUVirtualAddress(); // initial b1 (overridden per page below)
    ctx.srvTable[0] = renderer->StageSrvUavTable({ shadowGpu->InstanceReadSrv(f) }).gpu; // unified copy (Step 2), else ring
    indirectMat->Bind(cl, ctx, false);
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
            // All `groups` args for this page are contiguous; empty groups draw 0 instances (no-op).
            const UINT64 argOff = static_cast<UINT64>(p) * groups * argStride;
            renderer->ExecuteIndirect(cl, sig, groups, pageDrawArgs_.Get(), argOff, nullptr, 0);
            continue;
        }

        // Fallback (heterogeneous meshes): bind + draw per mesh-group.
        for (std::uint32_t g = 0; g < groups; ++g)
        {
            const Mesh* mesh = (g < groupMeshes.size()) ? groupMeshes[g] : nullptr;
            if (!mesh) { continue; }
            ID3D12Resource* vb = mesh->GetVertexBufferResource();
            ID3D12Resource* ib = mesh->GetIndexBufferResource();
            if (!vb || !ib) { continue; }
            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = vb->GetGPUVirtualAddress();
            vbv.SizeInBytes = static_cast<UINT>(vb->GetDesc().Width);
            vbv.StrideInBytes = mesh->GetVertexStride();
            cl->IASetVertexBuffers(0, 1, &vbv);
            D3D12_INDEX_BUFFER_VIEW ibv{};
            ibv.BufferLocation = ib->GetGPUVirtualAddress();
            ibv.SizeInBytes = static_cast<UINT>(ib->GetDesc().Width);
            ibv.Format = mesh->GetIndexFormat();
            cl->IASetIndexBuffer(&ibv);
            const UINT64 argOff = static_cast<UINT64>(p * groups + g) * argStride;
            renderer->ExecuteIndirect(cl, sig, 1, pageDrawArgs_.Get(), argOff, nullptr, 0);
        }
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

    // DBWIN log throttled independently of the (faster) stats sampling so a captured stress/dev run
    // is not flooded — the on-screen readout updates every sample, the log line only periodically.
    if (renderer->GetTotalFrameNumber() >= debugLoggedFrame_ + kDbwinLogPeriod)
    {
        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "[VSM] request %u (L0=%u L1=%u L2=%u L3=%u L4=%u) | resident=%u new=%u fail=%u | spots[%u %u %u %u %u %u %u %u] (pool=%u).\n",
            total, perLevel[0], perLevel[1], perLevel[2], perLevel[3], perLevel[4],
            resident, newAlloc, failCount,
            perSpot[0], perSpot[1], perSpot[2], perSpot[3], perSpot[4], perSpot[5], perSpot[6], perSpot[7],
            vsm::kPoolPageCount);
        OutputDebugStringA(buf);
        debugLoggedFrame_ = renderer->GetTotalFrameNumber();
    }
    debugReadbackState_ = 2;
    debugReadbackDoneFrame_ = renderer->GetTotalFrameNumber();
}
