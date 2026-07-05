#include "rendering/renderables/VirtualShadowMap.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "rendering/core/Renderer.h"
#include "rendering/core/ComputeDispatch.h"

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

    auto ok = [](const std::shared_ptr<Material>& m) { return m && m->GetPipelineState(); };
    if (!ok(pageRequestClearMat_) || !ok(pageRequestMat_))
    {
        OutputDebugStringA("[VSM] page-request PSO creation FAILED (shader compile?).\n");
        pageRequestClearMat_.reset();
        pageRequestMat_.reset();
    }
    else if (!ok(allocInitMat_) || !ok(allocTouchMat_) || !ok(allocFreeMat_) || !ok(allocMapMat_))
    {
        OutputDebugStringA("[VSM] page-alloc PSO creation FAILED (shader compile?).\n");
        allocInitMat_.reset(); allocTouchMat_.reset(); allocFreeMat_.reset(); allocMapMat_.reset();
    }
    else
    {
        OutputDebugStringA("[VSM] page-request + page-alloc shaders ready.\n");
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

    // Mark: one thread per DOWN-SAMPLED screen block (Step 19b reduced res, ≈1/64 the threads) ->
    // project the block-center pixel into each active local shadow view -> mark its mip page.
    const UINT ds = vsm::kRequestDownscale;
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
        AllocCB c{ numEntries, numPages, curFrame, vsm::kLruFrameThreshold };
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

    const UINT64 reqBytes = static_cast<UINT64>(vsm::kRequestWords) * sizeof(std::uint32_t);
    const UINT64 cntBytes = 4ull * sizeof(std::uint32_t);
    if (!debugReadback_)
    {
        D3D12_HEAP_PROPERTIES rb{};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = reqBytes + cntBytes;
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
    debugReadbackFrame_ = renderer->GetTotalFrameNumber();
    debugReadbackState_ = 1;
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

    const size_t reqBytes = static_cast<size_t>(vsm::kRequestWords) * sizeof(std::uint32_t);
    const size_t cntBytes = 4 * sizeof(std::uint32_t);
    D3D12_RANGE readRange{ 0, reqBytes + cntBytes };
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

    // Alloc counters (VSM_CNT_*): resident = carried-over survivors + newly allocated this frame.
    const std::uint32_t freeCount = counters[0]; // post-allocate: leftover free pages (may underflow if pool full)
    const std::uint32_t newAlloc  = counters[1]; // pages allocated (needs-render) this frame
    const std::uint32_t failCount = counters[2]; // requested pages that couldn't allocate (pool full)
    const std::uint32_t resident  = counters[3] + newAlloc;
    (void)freeCount;

    const D3D12_RANGE noWrite{ 0, 0 };
    debugReadback_->Unmap(0, &noWrite);

    char buf[288];
    std::snprintf(buf, sizeof(buf),
        "[VSM] request %u pages (L0=%u L1=%u L2=%u L3=%u L4=%u) | alloc: resident=%u newThisFrame=%u fail=%u (pool=%u).\n",
        total, perLevel[0], perLevel[1], perLevel[2], perLevel[3], perLevel[4],
        resident, newAlloc, failCount, vsm::kPoolPageCount);
    OutputDebugStringA(buf);
    debugReadbackState_ = 2;
    debugReadbackDoneFrame_ = renderer->GetTotalFrameNumber();
}
