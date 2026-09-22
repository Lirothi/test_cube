#include "rendering/streaming/TextureStreaming.h"

#include <algorithm>
#include <string>

#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "materials/Texture2D.h"
#include "rendering/core/MemoryReport.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/TextureCreate.h"
#include "rendering/streaming/StreamingSettings.h"

namespace streaming {

namespace {

// Resting state of every material texture (Texture2D.cpp kShaderReadStates).
constexpr D3D12_RESOURCE_STATES kShaderReadStates =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

UINT64 Align512(UINT64 v) { return (v + 511ull) & ~511ull; }

std::uint64_t RetiredBytesProvider(const void* self)
{
    return static_cast<const TextureStreaming*>(self)->RetiredBytes();
}

std::uint64_t ResidentBytesProvider(const void* self)
{
    return static_cast<const TextureStreaming*>(self)->ResidentBytes();
}

struct CopyOp
{
    ID3D12Resource* dst = nullptr;
    UINT dstMip = 0;
    ID3D12Resource* src = nullptr;
    UINT srcMip = 0;
    bool fromRing = false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ringFootprint{};
};

} // namespace

void TextureStreaming::Init(ID3D12Device* device)
{
    Shutdown();
    device_ = device;
    if (!device_) { return; }
    io_.Start();
    manager_.Init();
    // One `self` per provider: the report keys unregistration by it.
    render::RegisterMemoryProvider("tex", &ResidentBytesProvider, this);
    render::RegisterMemoryProvider("tex.ret", &RetiredBytesProvider, &retire_);
    lastReadout_ = std::chrono::steady_clock::now();
    start_ = lastReadout_;
}

float TextureStreaming::Now_() const
{
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count();
}

void TextureStreaming::Shutdown()
{
    if (!device_) { return; }
    manager_.Shutdown();
    io_.Stop();
    for (auto& s : swaps_) { s->newRes.Reset(); }
    swaps_.clear();
    orphanIo_.clear();
    retire_.ReleaseAll();
    ring_.Shutdown();
    for (Entry& e : entries_)
    {
        if (e.tex) { e.tex->DetachStreaming(); }
    }
    entries_.clear();
    freeEntries_.clear();
    registered_ = 0;
    render::UnregisterMemoryProvider(this);
    render::UnregisterMemoryProvider(&retire_);
    device_ = nullptr;
}

int TextureStreaming::Register(Texture2D* tex)
{
    if (!device_ || !tex) { return -1; }
    std::uint32_t idx;
    if (!freeEntries_.empty()) { idx = freeEntries_.back(); freeEntries_.pop_back(); }
    else { idx = static_cast<std::uint32_t>(entries_.size()); entries_.emplace_back(); }
    Entry& e = entries_[idx];
    e.tex = tex;
    ++e.gen;
    e.swap = nullptr;
    e.fading = false;
    e.fade = MipBiasFade{};
    // StreamableTextureResource.cpp:227: the fade starts at the loaded count, no interpolation.
    e.fade.SetNewMipCount(static_cast<float>(tex->GetResidentMips()), static_cast<float>(tex->GetResidentMips()),
                          0.0f, Now_(), g_mipFadeIn, g_mipFadeOut, g_mipFade);
    ++registered_;
    tex->AttachStreaming(this, static_cast<int>(idx));
    return static_cast<int>(idx);
}

void TextureStreaming::Unregister(Texture2D* tex)
{
    if (!tex) { return; }
    const int idx = tex->StreamingIndex();
    tex->DetachStreaming();
    if (idx < 0 || static_cast<std::size_t>(idx) >= entries_.size()) { return; }
    Entry& e = entries_[static_cast<std::size_t>(idx)];
    if (e.tex != tex) { return; }
    e.tex = nullptr;
    ++e.gen;      // any swap still pointing here fails Alive_ and is dropped at the next boundary
    e.swap = nullptr;
    --registered_;
    freeEntries_.push_back(static_cast<std::uint32_t>(idx));
}

bool TextureStreaming::EnsureRing_()
{
    if (ring_.Ready()) { return true; }
    const UINT64 bytes = static_cast<UINT64>(std::max(1, g_tempMemoryMB)) << 20;
    if (!ring_.Init(device_, bytes)) { return false; }
    LOG_INFO(logging::LogCategory::Render, "[texstream] upload ring {} MB", bytes >> 20);
    return true;
}

bool TextureStreaming::Alive_(const Swap& s) const
{
    return s.entry < entries_.size() && entries_[s.entry].tex != nullptr && entries_[s.entry].gen == s.gen;
}

void TextureStreaming::DropSwap_(Swap* s, std::uint64_t frameNo)
{
    if (s->hasRing) { ring_.Stamp(s->ringEntry, frameNo); } // nothing on the GPU read it
    if (s->io)
    {
        s->io->cancel.store(true, std::memory_order_release);
        if (!s->io->Finished()) { orphanIo_.push_back(std::move(s->io)); }
    }
    s->newRes.Reset();
    if (s->entry < entries_.size() && entries_[s->entry].swap == s) { entries_[s->entry].swap = nullptr; }
    std::erase_if(swaps_, [s](const std::unique_ptr<Swap>& p) { return p.get() == s; });
}

void TextureStreaming::TickManager(Renderer* renderer, const SceneFrameData& frame)
{
    if (!device_) { return; }
    manager_.Tick(*this, renderer, frame, frameNo_);
}

UINT TextureStreaming::InFlightTarget(std::uint32_t i) const
{
    if (i >= entries_.size() || !entries_[i].swap) { return 0; }
    return entries_[i].swap->newResident;
}

bool TextureStreaming::RequestResident(std::uint32_t i, UINT mips, float lastRenderAge)
{
    if (i >= entries_.size() || !entries_[i].tex || entries_[i].swap) { return false; }
    if (mips == entries_[i].tex->GetResidentMips()) { return false; }
    return IssueSwap_(i, mips, lastRenderAge);
}

// A4: per-frame MinLOD clamp of every texture mid-fade; the SRV rewrite is CPU-only.
void TextureStreaming::UpdateFades_(Renderer* renderer)
{
    fadingCount_ = 0;
    const float now = Now_();
    for (Entry& e : entries_)
    {
        if (!e.tex || !e.fading) { continue; }
        const float bias = g_mipFade ? e.fade.CalcMipBias(now) : 0.0f;
        if (bias <= 0.0f || !e.fade.Active(now))
        {
            e.tex->SetMinLodClamp(renderer, 0.0f);
            e.fading = false;
            continue;
        }
        e.tex->SetMinLodClamp(renderer, bias);
        ++fadingCount_;
    }
}

bool TextureStreaming::CancelRequest(std::uint32_t i)
{
    if (i >= entries_.size() || !entries_[i].swap) { return false; }
    Swap* s = entries_[i].swap;
    if (s->stage == Swap::Stage::Copied) { return false; } // on the GPU already; it lands next frame
    DropSwap_(s, frameNo_);
    return true;
}

UINT64 TextureStreaming::ResidentBytes() const
{
    UINT64 total = 0;
    for (const Entry& e : entries_) { if (e.tex) { total += e.tex->GetResidentBytes(); } }
    return total;
}

void TextureStreaming::OnFrameBegin(Renderer* renderer, std::uint64_t frameNo)
{
    if (!device_ || !renderer) { return; }
    frameNo_ = frameNo;
    {
        const auto now = std::chrono::steady_clock::now();
        if (lastFrameTime_ != std::chrono::steady_clock::time_point{})
        {
            const double dt = std::chrono::duration<double, std::milli>(now - lastFrameTime_).count();
            dtSumMs_ += dt;
            ++dtCount_;
            if (frameHadSwaps_) { dtMaxSwapMs_ = std::max(dtMaxSwapMs_, dt); ++swapFrames_; }
            else { dtMaxQuietMs_ = std::max(dtMaxQuietMs_, dt); }
        }
        lastFrameTime_ = now;
        frameHadSwaps_ = false;
    }
    ring_.ReleaseCompleted(frameNo);
    retire_.Release(frameNo);
    std::erase_if(orphanIo_, [](const std::unique_ptr<IoRequest>& r) { return r->Finished(); });

    for (std::size_t i = 0; i < swaps_.size();)
    {
        Swap* s = swaps_[i].get();
        const bool alive = Alive_(*s);
        bool removed = false;
        switch (s->stage)
        {
        case Swap::Stage::Copied:
            if (s->copyFrame < frameNo) // last frame's copies are submitted; this frame reads the new one
            {
                if (alive)
                {
                    Entry& e = entries_[s->entry];
                    Texture2D* tex = e.tex;
                    const UINT64 oldBytes = tex->GetResidentBytes();
                    GpuResource old;
                    tex->AdoptResource(renderer, std::move(s->newRes), s->newResident, old);
                    retire_.Retire(std::move(old), oldBytes, frameNo + render::kFrameCount);
                    ++swapsDone_;
                    frameHadSwaps_ = true;
                    e.swap = nullptr;
                    // StreamableTextureResource.cpp:262 FinalizeStreaming: fade from the old count to
                    // the new. A stream-out already dropped its mips here (UE fades BEFORE dropping),
                    // so only an arrival walks its clamp down.
                    e.fade.SetNewMipCount(static_cast<float>(std::max(s->newResident, s->oldResident)),
                                          static_cast<float>(s->newResident), s->lastRenderAge, Now_(),
                                          g_mipFadeIn, g_mipFadeOut, g_mipFade);
                    e.fading = g_mipFade && s->newResident > s->oldResident && e.fade.Active(Now_());
                    if (!e.fading) { tex->SetMinLodClamp(renderer, 0.0f); }
                }
                s->newRes.Reset();
                swaps_.erase(swaps_.begin() + static_cast<std::ptrdiff_t>(i));
                removed = true;
            }
            break;
        case Swap::Stage::WaitIo:
            if (!alive) { DropSwap_(s, frameNo); removed = true; break; }
            switch (s->io->GetState())
            {
            case IoRequest::State::Done: s->stage = Swap::Stage::ReadyToCopy; break;
            case IoRequest::State::Failed:
            case IoRequest::State::Cancelled:
                ++swapsFailed_;
                DropSwap_(s, frameNo);
                removed = true;
                break;
            default: break;
            }
            break;
        case Swap::Stage::ReadyToCopy:
            if (!alive) { DropSwap_(s, frameNo); removed = true; }
            break;
        }
        if (!removed) { ++i; }
    }

    UpdateFades_(renderer);
    if (g_enabled) { IssueRequests_(frameNo); }
    if (g_dumpRows && manager_.GetStats().cycles > 0)
    {
        g_dumpRows = false;
        const TextureStreamingManager::Stats& m = manager_.GetStats();
        LOG_INFO(logging::LogCategory::Render, "[texstream] rows (frame {}, cycle {}): mips resident needed(vis/hid) wanted budgeted requested texel seen | path",
                 frameNo, m.cycles);
        for (const TextureStreamingManager::Row& r : manager_.Rows())
        {
            LOG_INFO(logging::LogCategory::Render, "[texstream]   mips {} resident {} needed {}/{} wanted {} budgeted {} requested {} texel {:.2f} seen {:.1f}{} | {}",
                     r.mipCount, r.resident, r.visibleWanted, r.hiddenWanted, r.wanted, r.budgeted, r.requested,
                     r.texelFactor, r.lastSeen < 1.0e5f ? r.lastSeen : -1.0f, r.unknownRef ? " unknown-ref" : (r.terrain ? " terrain" : ""), r.path);
        }
    }
    {
        const TextureStreamingManager::Stats& m = manager_.GetStats();
        if (m.cycles != lastCycleSeen_)
        {
            lastCycleSeen_ = m.cycles;
            const unsigned lagging = m.deltaHistogram[7] + m.deltaHistogram[8];
            lagMax_ = std::max(lagMax_, lagging);
            if (lagging) { ++lagCycles_; }
        }
    }
    Readout_(frameNo);
}

void TextureStreaming::IssueRequests_(std::uint64_t /*frameNo*/)
{
    if (g_forceMips <= 0 || entries_.empty()) { return; }
    int ioInFlight = 0;
    for (const auto& s : swaps_) { if (s->stage == Swap::Stage::WaitIo) { ++ioInFlight; } }
    for (std::uint32_t i = 0; i < entries_.size(); ++i)
    {
        Entry& e = entries_[i];
        if (!e.tex || e.swap) { continue; }
        const UINT fileMips = e.tex->GetFileMipCount();
        if (fileMips == 0 || !e.tex->IsStreamable() || !e.tex->GetResource()) { continue; }
        const UINT wanted = static_cast<UINT>(std::clamp(g_forceMips, 1, static_cast<int>(fileMips)));
        const UINT resident = e.tex->GetResidentMips();
        if (wanted == resident) { continue; }
        if (wanted > resident)
        {
            if (ioInFlight >= g_maxIoInFlight) { continue; }
            if (IssueSwap_(i, wanted)) { ++ioInFlight; }
        }
        else
        {
            IssueSwap_(i, wanted);
        }
    }
}

// The new resource's desc and, for a stream-in, the ring layout + the read request.
bool TextureStreaming::IssueSwap_(std::uint32_t entryIdx, UINT wanted, float lastRenderAge)
{
    Entry& e = entries_[entryIdx];
    Texture2D* tex = e.tex;
    const streaming::DdsMipTable& table = tex->GetMipTable();
    const UINT mipCount = table.mipCount;
    const UINT oldResident = tex->GetResidentMips();
    if (wanted == 0 || wanted > mipCount || oldResident == 0) { return false; }

    auto swap = std::make_unique<Swap>();
    swap->entry = entryIdx;
    swap->gen = e.gen;
    swap->oldResident = oldResident;
    swap->newResident = wanted;
    swap->lastRenderAge = lastRenderAge;
    swap->newDesc = tex->GetResource()->GetDesc();
    swap->newDesc.Width = table.mips[mipCount - wanted].width;
    swap->newDesc.Height = table.mips[mipCount - wanted].height;
    swap->newDesc.MipLevels = static_cast<UINT16>(wanted);
    swap->newBytes = table.TailBytes(mipCount - wanted);

    if (wanted > oldResident)
    {
        if (!EnsureRing_()) { return false; }
        const UINT newMips = wanted - oldResident;   // resource mips [0, newMips) come from the file
        const UINT fileMip0 = mipCount - wanted;
        std::vector<UINT> rows(newMips);
        std::vector<UINT64> rowBytes(newMips);
        swap->ringFootprints.resize(newMips);
        UINT64 offset = 0;
        for (UINT m = 0; m < newMips; ++m)
        {
            UINT64 total = 0;
            device_->GetCopyableFootprints(&swap->newDesc, m, 1, offset, &swap->ringFootprints[m], &rows[m], &rowBytes[m], &total);
            offset = Align512(swap->ringFootprints[m].Offset + total);
            const DdsMipTable::Mip& mip = table.mips[fileMip0 + m];
            if (rowBytes[m] != mip.rowPitchBytes || UINT64(rows[m]) * rowBytes[m] != mip.sliceBytes)
            {
                LOG_ERROR(logging::LogCategory::Asset, "[texstream] footprint mismatch on {} mip {}: skipping", tex->GetSourcePath(), fileMip0 + m);
                return false;
            }
        }
        UINT64 ringOffset = 0;
        if (!ring_.Alloc(offset, swap->ringEntry, ringOffset)) { return false; } // ring full: try next frame
        swap->hasRing = true;
        auto io = std::make_unique<IoRequest>();
        io->path = tex->GetSourcePath();
        io->fileOffset = table.mips[fileMip0].fileOffset;
        io->fileBytes = 0;
        io->ringBase = ring_.Cpu();
        io->mips.resize(newMips);
        for (UINT m = 0; m < newMips; ++m)
        {
            swap->ringFootprints[m].Offset += ringOffset;
            const DdsMipTable::Mip& mip = table.mips[fileMip0 + m];
            IoMipLayout& l = io->mips[m];
            l.fileOffset = mip.fileOffset;
            l.ringOffset = swap->ringFootprints[m].Offset;
            l.rows = rows[m];
            l.rowBytes = mip.rowPitchBytes;
            l.rowPitch = swap->ringFootprints[m].Footprint.RowPitch;
            io->fileBytes += mip.sliceBytes;
        }
        swap->io = std::move(io);
        swap->stage = Swap::Stage::WaitIo;
        io_.Submit(swap->io.get());
    }
    else
    {
        swap->stage = Swap::Stage::ReadyToCopy; // stream-out: shared mips only
    }
    e.swap = swap.get();
    swaps_.push_back(std::move(swap));
    return true;
}

std::function<void(RenderGraphPassContext)> TextureStreaming::BuildPass(Renderer* renderer, RenderGraphPassContext& ctx)
{
    if (!device_ || !renderer || !g_enabled || swaps_.empty()) { return {}; }
    const std::uint64_t frameNo = renderer->GetTotalFrameNumber();

    std::vector<Swap*> ready;
    for (auto& s : swaps_)
    {
        if (s->stage == Swap::Stage::ReadyToCopy && Alive_(*s)) { ready.push_back(s.get()); }
        if (static_cast<int>(ready.size()) >= std::max(0, g_maxPerFrame)) { break; }
    }
    if (ready.empty()) { return {}; }

    std::vector<CopyOp> ops;
    std::vector<Swap*> recorded;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (Swap* s : ready)
    {
        Texture2D* tex = entries_[s->entry].tex;
        ID3D12Resource* old = tex->GetResource();
        if (!old || tex->GetResidentMips() != s->oldResident)
        {
            DropSwap_(s, frameNo);
            continue;
        }
        // Created in its RESTING state: the barrier compile assumes every declared resource rests
        // canonical at frame start (the creation state has no consumer, ResourceDeclarations.h),
        // so point A below is the one that takes it to COPY_DEST -- GBV 1334 otherwise.
        if (FAILED(render::CreateCommittedTexture(device_, hp, D3D12_HEAP_FLAG_NONE, s->newDesc,
                kShaderReadStates, nullptr, s->newRes.GetAddressOfForCreate())))
        {
            LOG_ERROR(logging::LogCategory::Render, "[texstream] CreateCommittedTexture failed for {} ({} mips)", tex->GetSourcePath(), s->newResident);
            ++swapsFailed_;
            DropSwap_(s, frameNo);
            continue;
        }
        const std::wstring name = L"Tex2D:" + tex->GetSourcePath() + L":s" + std::to_wstring(tex->GetSrvGeneration() + 1u);
        s->newRes.DeclareCreated(renderer->Declarations(), kShaderReadStates, kShaderReadStates, name.c_str());
        recorded.push_back(s);
    }
    if (recorded.empty()) { return {}; }

    ctx.NextPoint();
    const std::uint32_t pointCopy = ctx.usePoint ? *ctx.usePoint : 0u;
    for (Swap* s : recorded)
    {
        Texture2D* tex = entries_[s->entry].tex;
        ID3D12Resource* old = tex->GetResource();
        ID3D12Resource* fresh = s->newRes.Get();
        const UINT newMips = s->newResident > s->oldResident ? s->newResident - s->oldResident : 0u;
        for (UINT m = 0; m < newMips; ++m)
        {
            CopyOp op;
            op.dst = fresh; op.dstMip = m; op.src = ring_.Buffer(); op.fromRing = true;
            op.ringFootprint = s->ringFootprints[m];
            ops.push_back(op);
        }
        // UE AsyncReallocateTexture2D_RenderThread: the shared tail, smallest mips aligned.
        const UINT shared = std::min(s->oldResident, s->newResident);
        const UINT srcOff = s->oldResident - shared;
        const UINT dstOff = s->newResident - shared;
        for (UINT m = 0; m < shared; ++m)
        {
            CopyOp op;
            op.dst = fresh; op.dstMip = dstOff + m; op.src = old; op.srcMip = srcOff + m;
            ops.push_back(op);
        }
        ctx.Use(fresh, D3D12_RESOURCE_STATE_COPY_DEST);
        ctx.Use(old, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    ctx.NextPoint();
    const std::uint32_t pointRead = ctx.usePoint ? *ctx.usePoint : 0u;
    for (Swap* s : recorded)
    {
        ctx.Use(entries_[s->entry].tex->GetResource(), kShaderReadStates);
        ctx.Use(s->newRes.Get(), kShaderReadStates);
        s->stage = Swap::Stage::Copied;
        s->copyFrame = frameNo;
        if (s->hasRing) { ring_.Stamp(s->ringEntry, frameNo + render::kFrameCount); }
        if (s->newResident > s->oldResident) { bytesStreamedIn_ += s->newBytes - entries_[s->entry].tex->GetResidentBytes(); }
    }
    frameHadSwaps_ = true;

    return [renderer, ops = std::move(ops), pointCopy, pointRead](RenderGraphPassContext c)
    {
        CPU_SCOPE(ProfilerScopes::kPassTextureStreaming);
        auto t = c.BeginCL();
        SetCommandListName(t.cl, c.pass);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassTextureStreaming);
            renderer->EmitPoint(t.cl, pointCopy);
            for (const CopyOp& op : ops)
            {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = op.dst;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = op.dstMip;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = op.src;
                if (op.fromRing)
                {
                    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    src.PlacedFootprint = op.ringFootprint;
                }
                else
                {
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    src.SubresourceIndex = op.srcMip;
                }
                t.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            renderer->EmitPoint(t.cl, pointRead);
        }
        c.EndCL(t);
    };
}

void TextureStreaming::Readout_(std::uint64_t frameNo)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReadout_ < std::chrono::seconds(5)) { return; }
    lastReadout_ = now;
    if (registered_ == 0 && swaps_.empty() && retire_.Count() == 0) { return; }
    std::size_t waitIo = 0, ready = 0, copied = 0;
    for (const auto& s : swaps_)
    {
        if (s->stage == Swap::Stage::WaitIo) { ++waitIo; }
        else if (s->stage == Swap::Stage::ReadyToCopy) { ++ready; }
        else { ++copied; }
    }
    const double avgMs = dtCount_ ? dtSumMs_ / dtCount_ : 0.0;
    const TextureStreamingManager::Stats& m = manager_.GetStats();
    LOG_INFO(logging::LogCategory::Render,
        "[texstream] frame {}: {} textures, swaps io {} / ready {} / copied {}, ring {}/{} KB, retired {} ({} KB), done {} failed {}, read {} MB | frame ms avg {:.2f} max quiet {:.2f} max swap {:.2f} ({} swap frames) | pool {} MB budget {} used {} wanted {} | cycle in {} ({} KB) out {} ({} KB) cancel {} refused {} | lag>2 max {} in {} cycles | fading {} | calc {:.3f} ms",
        frameNo, registered_, waitIo, ready, copied, ring_.BytesInUse() >> 10, ring_.Capacity() >> 10,
        retire_.Count(), retire_.Bytes() >> 10, swapsDone_, swapsFailed_, io_.BytesRead() >> 20,
        avgMs, dtMaxQuietMs_, dtMaxSwapMs_, swapFrames_,
        m.poolBytes >> 20, m.budgetBytes >> 20, m.usedBytes >> 20, m.wantedBytes >> 20,
        m.requestsIn, m.bytesInCycle >> 10, m.requestsOut, m.bytesOutCycle >> 10, m.cancels, m.refused,
        lagMax_, lagCycles_, fadingCount_, m.calcMs);
    dtSumMs_ = 0.0; dtCount_ = 0; dtMaxQuietMs_ = 0.0; dtMaxSwapMs_ = 0.0; swapFrames_ = 0;
    lagMax_ = 0; lagCycles_ = 0;
}

} // namespace streaming
