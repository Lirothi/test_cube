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

    char buf[192];
    std::snprintf(buf, sizeof(buf),
        "[VSM] allocated: pool %ux%u D16 (%u pages), page table %u entries (%.2f MB pool).\n",
        vsm::kPoolTexels, vsm::kPoolTexels, vsm::kPoolPageCount, vsm::kPageTableEntries,
        (static_cast<double>(vsm::kPoolTexels) * vsm::kPoolTexels * 2.0) / (1024.0 * 1024.0));
    OutputDebugStringA(buf);
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
    if (!pageRequestClearMat_ || !pageRequestClearMat_->GetPipelineState() ||
        !pageRequestMat_ || !pageRequestMat_->GetPipelineState())
    {
        OutputDebugStringA("[VSM] page-request PSO creation FAILED (shader compile?).\n");
        pageRequestClearMat_.reset();
        pageRequestMat_.reset();
    }
    else
    {
        OutputDebugStringA("[VSM] page-request shaders ready.\n");
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

    // Mark: one thread per screen pixel -> project into each active shadow view -> mark page.
    RecordComputeDispatch(renderer, cl, pageRequestMat_.get(), static_cast<UINT>(sizeof(vsm::PageRequestConstants)),
        [&constants](std::uint8_t* dst) { std::memcpy(dst, &constants, sizeof(constants)); },
        { depthSrv },
        { requestUav_ },
        noSampler,
        screenW, screenH,
        requestBuffer_.Get());

    // Step 19 validation (temporary, one-shot after warmup): read the request bitfield back to
    // log requested-page counts kFrameCount frames later.
    if (requestReadbackState_ == 0 && renderer->GetTotalFrameNumber() > render::kFrameCount)
    {
        if (!requestReadback_)
        {
            D3D12_HEAP_PROPERTIES rb{};
            rb.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = static_cast<UINT64>(vsm::kRequestWords) * sizeof(std::uint32_t);
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
            renderer->GetDevice()->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(requestReadback_.GetAddressOf()));
        }
        if (requestReadback_)
        {
            renderer->Transition(cl, requestBuffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->CopyBufferRegion(requestReadback_.Get(), 0, requestBuffer_.Get(), 0,
                                 static_cast<UINT64>(vsm::kRequestWords) * sizeof(std::uint32_t));
            requestReadbackFrame_ = renderer->GetTotalFrameNumber();
            requestReadbackState_ = 1;
        }
    }
}

void VirtualShadowMap::PollPageRequestDebug(Renderer* renderer)
{
    if (requestReadbackState_ != 1 || !renderer || !requestReadback_) { return; }
    if (renderer->GetTotalFrameNumber() < requestReadbackFrame_ + render::kFrameCount) { return; }

    const size_t bytes = static_cast<size_t>(vsm::kRequestWords) * sizeof(std::uint32_t);
    D3D12_RANGE readRange{ 0, bytes };
    void* mapped = nullptr;
    if (FAILED(requestReadback_->Map(0, &readRange, &mapped)) || !mapped)
    {
        requestReadbackState_ = 2;
        return;
    }
    const auto* words = reinterpret_cast<const std::uint32_t*>(mapped);

    auto popcount = [](std::uint32_t x) { std::uint32_t c = 0; while (x) { c += (x & 1u); x >>= 1u; } return c; };
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < vsm::kRequestWords; ++i) { total += popcount(words[i]); }

    // Per-view counts for the first few views (each view = kVirtualPagesPerView contiguous bits).
    std::uint32_t v0 = 0, v4 = 0; // view 0 (cascade 0) + view 4 (first spot slot)
    auto viewCount = [&](std::uint32_t view) {
        std::uint32_t c = 0;
        const std::uint32_t firstPage = view * vsm::kVirtualPagesPerView;
        for (std::uint32_t p = 0; p < vsm::kVirtualPagesPerView; ++p)
        {
            const std::uint32_t page = firstPage + p;
            if (words[page >> 5u] & (1u << (page & 31u))) { ++c; }
        }
        return c;
    };
    v0 = viewCount(0);
    v4 = viewCount(4);

    const D3D12_RANGE noWrite{ 0, 0 };
    requestReadback_->Unmap(0, &noWrite);

    char buf[192];
    std::snprintf(buf, sizeof(buf),
        "[VSM] page request: %u pages total requested (view0=%u, view4=%u of %u/view).\n",
        total, v0, v4, vsm::kVirtualPagesPerView);
    OutputDebugStringA(buf);
    requestReadbackState_ = 2;
}
