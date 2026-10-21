#include "rendering/shadows/LocalShadowCache.h"

#include <algorithm>
#include <cstring>

#include "core/logging/Log.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/TextureCreate.h"
#include "rendering/shadows/ShadowSettings.h"

namespace render {

// ---- GPU side --------------------------------------------------------------------------------------------------

void LocalShadowCache::EnsurePool(ID3D12Device* dev, ResourceDeclarations decls, std::uint32_t pageSize, std::uint32_t pages)
{
    pages = std::min(pages, kLocalShadowCacheMaxPagesLimit);
    if (pages == pages_ && pageSize == pageSize_ && (pages == 0u || pool_.Get())) { return; }
    pool_.Reset();
    dsvHeap_.Reset();
    srvHeap_.Reset();
    pages_ = 0;
    pageSize_ = pageSize;
    entries_.clear(); // the contents are gone: every light is a miss again
    ResetAllocator();
    if (!dev || pages == 0u || pageSize < kMinBlock) { return; }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D16_UNORM;
    clear.DepthStencil.Depth = 1.0f;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = pageSize;
    desc.Height = pageSize;
    desc.DepthOrArraySize = static_cast<UINT16>(pages);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    // Rests where the copies and the direct reads take it (kRestState); the cache pass takes it to DEPTH_WRITE for a fill
    // and back.
    if (FAILED(CreateCommittedTexture(dev, heap, D3D12_HEAP_FLAG_NONE, desc, kRestState,
                                      &clear, pool_.GetAddressOfForCreate())))
    {
        LOG_ERROR(logging::LogCategory::RenderShadow, "local shadow cache: pool of {} x {}^2 failed to allocate; no cache",
                  pages, pageSize);
        pool_.Reset();
        return;
    }
    pool_.DeclareCreated(decls, kRestState, L"LocalShadowCache");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = pages;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(dsvHeap_.GetAddressOf()))))
    {
        pool_.Reset();
        return;
    }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(srvHeap_.GetAddressOf()))))
    {
        pool_.Reset();
        dsvHeap_.Reset();
        return;
    }
    dsvIncrement_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (std::uint32_t p = 0; p < pages; ++p)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = p;
        dsv.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(p) * dsvIncrement_;
        dev->CreateDepthStencilView(pool_.Get(), &dsv, h);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R16_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = pages;
    dev->CreateShaderResourceView(pool_.Get(), &sd, srvHeap_->GetCPUDescriptorHandleForHeapStart());

    pages_ = pages;
    ResetAllocator();
    LOG_INFO(logging::LogCategory::RenderShadow, "local shadow cache pool -> {} page(s) of {}^2 D16: {:.1f} MB",
             pages, pageSize, static_cast<double>(pages) * pageSize * pageSize * 2.0 / (1024.0 * 1024.0));
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
    // The deepest free block at or above the wanted level (the smallest that holds it), split down.
    std::int32_t level = static_cast<std::int32_t>(want);
    while (level >= 0 && freeLists_[static_cast<std::size_t>(level)].empty()) { --level; }
    if (level < 0) { return false; }
    Block b = freeLists_[static_cast<std::size_t>(level)].back();
    freeLists_[static_cast<std::size_t>(level)].pop_back();
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
    if (staticVersion != staticVersion_)
    {
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
        return Mode::Uncached;
    }
    auto it = entries_.find(key.id);
    if (it != entries_.end())
    {
        Entry& e = it->second;
        e.lastUsed = frame_;
        if (e.rect.size == key.res && e.valid && e.key.viewProj == key.viewProj && e.key.encoding == key.encoding)
        {
            ++stats_.hits;
            out = e.rect;
            return Mode::Hit;
        }
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
            // Evict the least recently used entry this frame has not asked for.
            auto victim = entries_.end();
            for (auto v = entries_.begin(); v != entries_.end(); ++v)
            {
                if (v->second.lastUsed >= frame_ || v->second.rect.page < 0) { continue; }
                if (victim == entries_.end() || v->second.lastUsed < victim->second.lastUsed) { victim = v; }
            }
            if (victim == entries_.end()) { break; }
            FreeEntry(victim->second);
            entries_.erase(victim);
            ++stats_.evicted;
            placed = place();
        }
        if (!placed)
        {
            entries_.erase(key.id);
            wantedPages_ = std::max(wantedPages_, pages_ + 1u);
            ++stats_.uncached;
            return Mode::Uncached;
        }
    }
    e.key = key;
    e.valid = false;
    e.pending = true;
    ++stats_.misses;
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
