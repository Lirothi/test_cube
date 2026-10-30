#include "rendering/shadows/LocalShadowCache.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/logging/Log.h"
#include "rendering/core/BarrierTranslation.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/TextureCreate.h"
#include "rendering/shadows/ShadowSettings.h"

namespace render {

// ---- GPU side --------------------------------------------------------------------------------------------------

bool LocalShadowCache::CreatePoolTexture(ID3D12Device* dev, std::uint32_t pages, D3D12_RESOURCE_STATES state,
                                         Microsoft::WRL::ComPtr<ID3D12Resource>& out) const
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D16_UNORM;
    clear.DepthStencil.Depth = 1.0f;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = pageSize_;
    desc.Height = pageSize_;
    desc.DepthOrArraySize = static_cast<UINT16>(pages);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    out.Reset();
    if (FAILED(gpu::CreateCommittedTexture(dev, heap, D3D12_HEAP_FLAG_NONE, desc, state, &clear, MemTag::ShadowLocal,
                                           L"LocalShadowCache", out.GetAddressOf())) || !out)
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "local shadow cache: pool of {} x {}^2 failed to allocate", pages, pageSize_);
        out.Reset();
        return false;
    }
    return true;
}

bool LocalShadowCache::CreatePoolViews(ID3D12Device* dev, ID3D12Resource* pool, std::uint32_t pages,
                                       Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& dsvHeap,
                                       Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& srvHeap)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = pages;
    if (FAILED(gpu::CreateDescriptorHeap(dev, &hd, MemTag::ShadowLocal, L"LocalShadowCache.DsvHeap", IID_PPV_ARGS(dsvHeap.ReleaseAndGetAddressOf()))))
    {
        return false;
    }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    if (FAILED(gpu::CreateDescriptorHeap(dev, &hd, MemTag::ShadowLocal, L"LocalShadowCache.SrvHeap", IID_PPV_ARGS(srvHeap.ReleaseAndGetAddressOf()))))
    {
        dsvHeap.Reset();
        return false;
    }
    dsvIncrement_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (std::uint32_t p = 0; p < pages; ++p)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = p;
        dsv.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(p) * dsvIncrement_;
        dev->CreateDepthStencilView(pool, &dsv, h);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R16_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = pages;
    dev->CreateShaderResourceView(pool, &sd, srvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

void LocalShadowCache::EnsurePool(ID3D12Device* dev, ResourceDeclarations decls, std::uint32_t pageSize, std::uint32_t pages)
{
    pages = std::min(pages, kLocalShadowCacheMaxPagesLimit);
    if (pages == pages_ && pageSize == pageSize_ && (pages == 0u || pool_.Get())) { return; }
    pool_.Reset();
    dsvHeap_.Reset();
    srvHeap_.Reset();
    pages_ = 0;
    pageSize_ = pageSize;
    const size_t emptied = entries_.size();
    if (emptied != 0u) { ++totals_.flushes; }
    entries_.clear(); // the contents are gone: every light is a miss again
    ResetAllocator();
    if (!dev || pages == 0u || pageSize < kMinBlock) { return; }

    // Rests where the copies and the direct reads take it (kRestState); the cache pass takes it to DEPTH_WRITE for a fill
    // and back.
    Microsoft::WRL::ComPtr<ID3D12Resource> pool;
    if (!CreatePoolTexture(dev, pages, kRestState, pool)) { return; }
    if (!CreatePoolViews(dev, pool.Get(), pages, dsvHeap_, srvHeap_)) { return; }
    pool_.Attach(decls, std::move(pool), kRestState, L"LocalShadowCache");
    pages_ = pages;
    ResetAllocator();
    LOG_INFO(logging::LogCategory::RenderShadow, "local shadow cache pool -> {} page(s) of {}^2 D16: {:.1f} MB ({} cached view(s) emptied)",
             pages, pageSize, static_cast<double>(pages) * pageSize * pageSize * 2.0 / (1024.0 * 1024.0), emptied);
}

bool LocalShadowCache::GrowPool(ID3D12Device* dev, ResourceDeclarations decls, std::uint32_t pages,
                                ID3D12GraphicsCommandList* cl, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive)
{
    pages = std::min(pages, kLocalShadowCacheMaxPagesLimit);
    if (!dev || !cl || !keepAlive || !pool_.Get() || pages <= pages_ || freeLists_.empty()) { return false; }
    Microsoft::WRL::ComPtr<ID3D12Resource> grown;
    if (!CreatePoolTexture(dev, pages, D3D12_RESOURCE_STATE_COPY_DEST, grown)) { return false; }
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv, srv;
    if (!CreatePoolViews(dev, grown.Get(), pages, dsv, srv)) { return false; }
    // The old pages into the new array, a whole subresource each (a depth resource copies only whole subresources), then
    // the new array to the rest state: the frame finds it as a freshly declared pool would be.
    ID3D12Resource* old = pool_.Get();
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = old;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = kRestState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers::EmitOne(cl, b); // the enhanced form when the textures were made with layouts
    for (std::uint32_t p = 0; p < pages_; ++p)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = grown.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = p;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = old;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = p;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    b.Transition.pResource = grown.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = kRestState;
    barriers::EmitOne(cl, b); // the enhanced form when the textures were made with layouts
    keepAlive->push_back(Microsoft::WRL::ComPtr<ID3D12Resource>(old)); // freed after the batch's wait
    pool_.Attach(decls, std::move(grown), kRestState, L"LocalShadowCache");
    dsvHeap_ = std::move(dsv);
    srvHeap_ = std::move(srv);
    for (std::uint32_t p = pages_; p < pages; ++p) { freeLists_[0].push_back(Block{ static_cast<std::uint16_t>(p), 0, 0 }); }
    LOG_INFO(logging::LogCategory::RenderShadow, "local shadow cache pool -> {} page(s) of {}^2 D16: {:.1f} MB (grown from {}, "
             "{} cached view(s) kept)", pages, pageSize_, static_cast<double>(pages) * pageSize_ * pageSize_ * 2.0 / (1024.0 * 1024.0),
             pages_, entries_.size());
    pages_ = pages;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE LocalShadowCache::PoolSrv() const
{
    return srvHeap_ ? srvHeap_->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

void LocalShadowCache::BindTarget(ID3D12GraphicsCommandList* cl, const Rect& r, bool clear) const
{
    if (!cl || !dsvHeap_ || r.page < 0 || static_cast<std::uint32_t>(r.page) >= pages_) { return; }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += static_cast<SIZE_T>(r.page) * dsvIncrement_;
    cl->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    const D3D12_VIEWPORT vp{ static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.size),
                             static_cast<float>(r.size), 0.0f, 1.0f };
    const D3D12_RECT sc{ static_cast<LONG>(r.x), static_cast<LONG>(r.y), static_cast<LONG>(r.x + r.size),
                         static_cast<LONG>(r.y + r.size) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    if (clear) { cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &sc); }
}

Material* LocalShadowCache::CopyMaterial(Renderer* renderer)
{
    if (!copyMatTried_ && renderer && renderer->GetMaterialManager())
    {
        copyMatTried_ = true;
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/shadow_cache_copy.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = ""; // no IA input: a full-viewport triangle from SV_VertexID
        gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.numRT = 0;
        gd.dsvFormat = DXGI_FORMAT_D16_UNORM; // the atlases' and the pool's
        gd.depth.DepthEnable = TRUE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        gd.blend.RenderTarget[0].BlendEnable = FALSE;
        copyMat_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
        if (!copyMat_ || !copyMat_->GetPipelineState())
        {
            LOG_ERROR(logging::LogCategory::RenderShadow, "shadow_cache_copy.hlsl did not build a PSO; no local shadow cache");
            copyMat_.reset();
        }
    }
    return copyMat_.get();
}

// ---- CPU side ---------------------------------------------------------------------------------------------------

std::uint32_t LocalShadowCache::LevelOf(std::uint32_t size) const
{
    std::uint32_t level = 0;
    for (std::uint32_t s = pageSize_; s > size && s > kMinBlock; s >>= 1) { ++level; }
    return level;
}

void LocalShadowCache::ResetAllocator()
{
    freeLists_.clear();
    if (pageSize_ < kMinBlock) { return; }
    freeLists_.resize(LevelOf(kMinBlock) + 1u);
    for (std::uint32_t p = 0; p < pages_; ++p) { freeLists_[0].push_back(Block{ static_cast<std::uint16_t>(p), 0, 0 }); }
}

bool LocalShadowCache::Allocate(std::uint32_t size, Rect& out)
{
    if (freeLists_.empty() || size < kMinBlock || size > pageSize_) { return false; }
    const std::uint32_t want = LevelOf(size);
    // The deepest free block at or above the wanted level (the smallest that holds it), split down -- and of those the
    // one on the page with the least free space left, so the emptier pages stay whole for the big requests: a point
    // block of 512^2 faces needs a whole 2048^2 page, and with the small squares spread over all eight pages the Lamps
    // had half the pool free and no whole page (2026-10-30).
    std::int32_t level = static_cast<std::int32_t>(want);
    while (level >= 0 && freeLists_[static_cast<std::size_t>(level)].empty()) { --level; }
    if (level < 0) { return false; }
    auto& from = freeLists_[static_cast<std::size_t>(level)];
    std::size_t pick = from.size() - 1u;
    if (from.size() > 1u)
    {
        std::vector<std::uint64_t> pageFree(pages_, 0u);
        for (std::size_t l = 0; l < freeLists_.size(); ++l)
        {
            const std::uint64_t side = pageSize_ >> l;
            for (const Block& f : freeLists_[l]) { if (f.page < pages_) { pageFree[f.page] += side * side; } }
        }
        for (std::size_t i = 0; i < from.size(); ++i)
        {
            if (pageFree[from[i].page] < pageFree[from[pick].page]) { pick = i; }
        }
    }
    Block b = from[pick];
    from[pick] = from.back();
    from.pop_back();
    while (static_cast<std::uint32_t>(level) < want)
    {
        ++level;
        const std::uint16_t half = static_cast<std::uint16_t>((pageSize_ >> level) / kMinBlock);
        auto& list = freeLists_[static_cast<std::size_t>(level)];
        list.push_back(Block{ b.page, static_cast<std::uint16_t>(b.x + half), b.y });
        list.push_back(Block{ b.page, b.x, static_cast<std::uint16_t>(b.y + half) });
        list.push_back(Block{ b.page, static_cast<std::uint16_t>(b.x + half), static_cast<std::uint16_t>(b.y + half) });
    }
    out.page = b.page;
    out.x = b.x * kMinBlock;
    out.y = b.y * kMinBlock;
    out.size = pageSize_ >> want;
    return true;
}

// A point light's 3f x 2f block: one buddy square of 4f, its faces in the top-left 2f quarter and the left column of the
// top-right one; the bottom two quarters and that column's right half go straight back to the free lists (no area lost
// to the 4f rounding), and FreeEntry returns the three used pieces, which the buddy merge then joins with the rest.
bool LocalShadowCache::AllocatePointBlock(std::uint32_t faceRes, Rect& out)
{
    if (!PointBlockFits(faceRes)) { return false; }
    Rect whole{};
    if (!Allocate(4u * faceRes, whole)) { return false; }
    const std::uint32_t f = faceRes;
    const auto giveBack = [&](std::uint32_t x, std::uint32_t y, std::uint32_t size)
    {
        freeLists_[LevelOf(size)].push_back(Block{ static_cast<std::uint16_t>(whole.page),
            static_cast<std::uint16_t>(x / kMinBlock), static_cast<std::uint16_t>(y / kMinBlock) });
    };
    giveBack(whole.x, whole.y + 2u * f, 2u * f);          // bottom-left quarter
    giveBack(whole.x + 2u * f, whole.y + 2u * f, 2u * f); // bottom-right quarter
    giveBack(whole.x + 3u * f, whole.y, f);               // the top-right quarter's right column
    giveBack(whole.x + 3u * f, whole.y + f, f);
    out.page = whole.page;
    out.x = whole.x;
    out.y = whole.y;
    out.size = f;
    return true;
}

void LocalShadowCache::FreeEntry(const Entry& e)
{
    if (!e.block)
    {
        Free(e.rect);
        return;
    }
    if (e.rect.page < 0) { return; }
    const std::uint32_t f = e.rect.size;
    Free(Rect{ e.rect.page, e.rect.x, e.rect.y, 2u * f });
    Free(Rect{ e.rect.page, e.rect.x + 2u * f, e.rect.y, f });
    Free(Rect{ e.rect.page, e.rect.x + 2u * f, e.rect.y + f, f });
}

void LocalShadowCache::Free(const Rect& r)
{
    if (r.page < 0 || freeLists_.empty()) { return; }
    std::uint32_t level = LevelOf(r.size);
    Block b{ static_cast<std::uint16_t>(r.page), static_cast<std::uint16_t>(r.x / kMinBlock),
             static_cast<std::uint16_t>(r.y / kMinBlock) };
    // Merge with the three siblings while all of them are free.
    while (level > 0)
    {
        const std::uint16_t units = static_cast<std::uint16_t>((pageSize_ >> level) / kMinBlock);
        const std::uint16_t px = static_cast<std::uint16_t>(b.x - (b.x % (2u * units)));
        const std::uint16_t py = static_cast<std::uint16_t>(b.y - (b.y % (2u * units)));
        auto& list = freeLists_[level];
        std::array<std::size_t, 3> found{};
        std::size_t n = 0;
        for (std::size_t i = 0; i < list.size() && n < 3; ++i)
        {
            const Block& o = list[i];
            if (o.page != b.page || (o.x == b.x && o.y == b.y)) { continue; }
            if ((o.x == px || o.x == px + units) && (o.y == py || o.y == py + units)) { found[n++] = i; }
        }
        if (n < 3) { break; }
        std::sort(found.begin(), found.end());
        for (std::size_t k = 3; k-- > 0;) { list.erase(list.begin() + static_cast<std::ptrdiff_t>(found[k])); }
        b = Block{ b.page, px, py };
        --level;
    }
    freeLists_[level].push_back(b);
}

void LocalShadowCache::BeginFrame(std::uint64_t frame, std::uint32_t staticVersion)
{
    frame_ = frame;
    stats_ = Stats{};
    wantedPages_ = pages_;
    fillsRan_ = false;
    ++totals_.frames;
    if (staticVersion != staticVersion_)
    {
        ++totals_.staticBumps;
        // A static caster moved, appeared or went: every cached picture may hold it (UE flushes only the lights it
        // interacts with -- FLightPrimitiveInteraction::FlushCachedShadowMapData; editing is not the hot path here).
        for (auto& [id, e] : entries_) { e.valid = false; }
        staticVersion_ = staticVersion;
    }
}

LocalShadowCache::Mode LocalShadowCache::Request(const Key& key, Rect& out, bool pointBlock)
{
    out = Rect{};
    if (pages_ == 0u || freeLists_.empty() || key.res < kMinBlock || key.res > pageSize_ ||
        (pointBlock && !PointBlockFits(key.res)))
    {
        if (pages_ == 0u) { wantedPages_ = std::max(wantedPages_, 1u); }
        ++stats_.uncached;
        ++totals_.uncached;
        return Mode::Uncached;
    }
    std::uint64_t* missReason = &totals_.missNew;
    auto it = entries_.find(key.id);
    if (it != entries_.end())
    {
        Entry& e = it->second;
        e.lastUsed = frame_;
        if (e.rect.size == key.res && e.valid && e.key.viewProj == key.viewProj && e.key.encoding == key.encoding)
        {
            ++stats_.hits;
            ++totals_.hits;
            out = e.rect;
            return Mode::Hit;
        }
        missReason = e.rect.size != key.res ? &totals_.missRes : !e.valid ? &totals_.missStale : &totals_.missMoved;
        if (e.rect.size != key.res) // a new resolution needs a new square (UE recaches on a size change too)
        {
            FreeEntry(e);
            e.rect = Rect{};
        }
        e.valid = false;
    }
    Entry& e = entries_[key.id];
    e.lastUsed = frame_;
    if (e.rect.page < 0)
    {
        e.block = pointBlock;
        const auto place = [&]() { return pointBlock ? AllocatePointBlock(key.res, e.rect) : Allocate(key.res, e.rect); };
        bool placed = place();
        while (!placed)
        {
            // Evict the least recently used entry neither this frame nor the last one asked for. Not "this frame" alone:
            // the lights not yet asked this frame are the ones asked AFTER this request, and a request that fails anyway
            // threw them out for nothing -- they refilled next frame (wind_test's Lamps in flight: 0.23 evictions and 0.22
            // refills a frame, with half the pool free but no whole page for a point block, 2026-10-30).
            auto victim = entries_.end();
            for (auto v = entries_.begin(); v != entries_.end(); ++v)
            {
                if (v->second.lastUsed + 1u >= frame_ || v->second.rect.page < 0) { continue; }
                if (victim == entries_.end() || v->second.lastUsed < victim->second.lastUsed) { victim = v; }
            }
            if (victim == entries_.end()) { break; }
            FreeEntry(victim->second);
            entries_.erase(victim);
            ++stats_.evicted;
            ++totals_.evicted;
            placed = place();
        }
        if (!placed)
        {
            entries_.erase(key.id);
            wantedPages_ = std::max(wantedPages_, pages_ + 1u);
            if (pointBlock) { ++totals_.blockNoRoom; } // the caller places the six faces on their own instead
            else
            {
                ++stats_.uncached;
                ++totals_.uncached;
                ++totals_.uncachedNoRoom;
            }
            std::uint64_t freeTexels = 0, usedNow = 0;
            std::uint32_t largest = 0;
            for (std::size_t l = 0; l < freeLists_.size(); ++l)
            {
                const std::uint64_t side = pageSize_ >> l;
                freeTexels += side * side * freeLists_[l].size();
                if (!freeLists_[l].empty()) { largest = std::max(largest, static_cast<std::uint32_t>(side)); }
            }
            for (const auto& [id, v] : entries_) { usedNow += v.lastUsed >= frame_ ? 1u : 0u; }
            const double freeM = static_cast<double>(freeTexels) * 1e-6;
            const double poolM = static_cast<double>(pages_) * pageSize_ * pageSize_ * 1e-6;
            if (pointBlock) // two call sites: each throttled on its own
            {
                LOG_INFO_THROTTLED(std::chrono::seconds(5), logging::LogCategory::RenderShadow,
                    "local shadow cache: no room for a point block of {}^2 faces ({} pages, {:.1f} of {:.1f} Mtexels free, largest "
                    "free square {}^2; {} entries, {} asked this frame) -- its six faces cached on their own", key.res, pages_,
                    freeM, poolM, largest, entries_.size(), usedNow);
            }
            else
            {
                LOG_INFO_THROTTLED(std::chrono::seconds(5), logging::LogCategory::RenderShadow,
                    "local shadow cache: no room for a {}^2 square ({} pages, {:.1f} of {:.1f} Mtexels free, largest free square "
                    "{}^2; {} entries, {} asked this frame) -- drawn uncached", key.res, pages_, freeM, poolM, largest,
                    entries_.size(), usedNow);
            }
            return Mode::Uncached;
        }
    }
    e.key = key;
    e.valid = false;
    e.pending = true;
    ++stats_.misses;
    ++*missReason;
    totals_.fillTexels += static_cast<std::uint64_t>(key.res) * key.res * (pointBlock ? 6u : 1u);
    out = e.rect;
    stats_.entries = static_cast<std::uint32_t>(entries_.size());
    return Mode::Miss;
}

void LocalShadowCache::CommitFills(bool ran)
{
    fillsRan_ = ran;
    for (auto& [id, e] : entries_)
    {
        if (!e.pending) { continue; }
        e.valid = ran;
        e.pending = false;
    }
    stats_.entries = static_cast<std::uint32_t>(entries_.size());
}

} // namespace render
