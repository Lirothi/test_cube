#include "rendering/core/FrameResource.h"
#include "core/Helpers.h"
#include "core/logging/Log.h"
#include "rendering/core/GpuAlloc.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace {

std::atomic<std::uint64_t> g_ringChecks{ 0 };
std::atomic<std::uint64_t> g_ringBytesMoved{ 0 };
std::atomic<std::uint64_t> g_ringLateAllocs{ 0 };
std::atomic<std::uint64_t> g_ringLateTables{ 0 };

void AppendCodeAddress(std::string& line, void* address)
{
    if (address == nullptr)
    {
        return;
    }
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) && module)
    {
        char full[MAX_PATH] = "";
        if (GetModuleFileNameA(module, full, MAX_PATH) != 0)
        {
            const char* slash = std::strrchr(full, '\\');
            strncpy_s(name, slash ? slash + 1 : full, _TRUNCATE);
        }
    }
    char text[MAX_PATH + 32];
    std::snprintf(text, sizeof(text), " %s+0x%llx", name,
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) -
                                                  reinterpret_cast<std::uintptr_t>(module)));
    line += text;
}

} // namespace

void render::ReportLateDescriptorStage(std::uint32_t count, std::uint32_t used, std::uint64_t frame)
{
    // The slot's descriptor ring is the frame's tables; one staged after the seal lands where a list already handed to
    // the queue may read (its frame's tables), or is wiped by the next ResetPerFrame under its own list.
    g_ringLateTables.fetch_add(1u, std::memory_order_relaxed);
    void* callers[8] = {};
    CaptureStackBackTrace(1, 8, callers, nullptr);
    std::string by;
    for (void* c : callers)
    {
        AppendCodeAddress(by, c);
    }
    LOG_ERROR_THROTTLED(std::chrono::seconds(2), logging::LogCategory::RenderRhi,
                        "upload ring: {} descriptors staged at ring entry {} after frame {} sealed this slot, by{}",
                        count, used, frame, by);
}

void render::ShutdownUploadRingCheck()
{
    if (g_uploadRingCheckShutdownHook)
    {
        g_uploadRingCheckShutdownHook();
        g_uploadRingCheckShutdownHook = nullptr;
    }
    if (!g_uploadRingCheck)
    {
        return;
    }
    const std::uint64_t moved = g_ringBytesMoved.load(std::memory_order_relaxed);
    const std::uint64_t late = g_ringLateAllocs.load(std::memory_order_relaxed);
    const std::uint64_t lateTables = g_ringLateTables.load(std::memory_order_relaxed);
    const std::string text = "upload ring check: " + std::to_string(g_ringChecks.load(std::memory_order_relaxed)) +
                             " checks, " + std::to_string(moved) + " bytes moved, " + std::to_string(late) +
                             " allocations and " + std::to_string(lateTables) + " descriptor tables after a seal";
    if (moved != 0u || late != 0u || lateTables != 0u)
    {
        LOG_ERROR(logging::LogCategory::RenderRhi, "{}", text);
    }
    else
    {
        LOG_INFO(logging::LogCategory::RenderRhi, "{}", text);
    }
}

void FrameResource::NoteAlloc_(UINT region, UINT offset, UINT size)
{
    CheckAlloc_ a;
    a.region = region;
    a.offset = offset;
    a.size = size;
    CaptureStackBackTrace(2, kCheckCallers_, a.callers, nullptr); // past NoteAlloc_ and AllocDynamic
    std::lock_guard<std::mutex> lk(checkMtx_);
    checkAllocs_.push_back(a);
    if (checkSealed_)
    {
        // The slot's frame has ended its recording and the slot is not reset yet: whatever records into this block
        // now is either late for that frame (the list went to the queue without it) or early for the next one, whose
        // BeginFrame resets the ring under it and hands the same bytes out again.
        g_ringLateAllocs.fetch_add(1u, std::memory_order_relaxed);
        std::string by;
        for (void* c : a.callers)
        {
            AppendCodeAddress(by, c);
        }
        LOG_ERROR_THROTTLED(std::chrono::seconds(2), logging::LogCategory::RenderRhi,
                            "upload ring: [{}, +{}) of region {} allocated after frame {} sealed this slot, by{}",
                            offset, size, region, checkFrame_, by);
    }
}

void FrameResource::NoteFilled(const DynamicAlloc& a)
{
    if (!render::g_uploadRingCheck || a.cpu == nullptr || a.size == 0u)
    {
        return;
    }
    CheckFilled_ f;
    f.cpu = static_cast<const std::uint8_t*>(a.cpu);
    f.bytes.resize(a.size);
    std::memcpy(f.bytes.data(), a.cpu, a.size);
    CaptureStackBackTrace(1, kCheckCallers_, f.callers, nullptr);
    std::lock_guard<std::mutex> lk(checkMtx_);
    checkFilled_.push_back(std::move(f));
}

void FrameResource::SealUpload(std::uint64_t frame)
{
    // The self-test's canaries, allocated like any constant: the first is filled and then written before the seal (the
    // fill check must report it), the second is sealed with the rest and written behind the seal's back (the next
    // VerifyUpload must). Nobody reads them, so the writes harm nothing.
    std::uint32_t* canary = nullptr;
    if (render::g_uploadRingCheckSelftest && frame == 3u)
    {
        const DynamicAlloc filled = AllocDynamic(256u, 256u);
        if (filled.cpu)
        {
            std::memset(filled.cpu, 0xA5, 256u);
            NoteFilled(filled);
            static_cast<std::uint32_t*>(filled.cpu)[1] = 0x5A5A5A5Au;
        }
        const DynamicAlloc a = AllocDynamic(256u, 256u);
        canary = static_cast<std::uint32_t*>(a.cpu);
        if (canary)
        {
            std::memset(canary, 0xA5, 256u);
        }
    }

    {
        std::lock_guard<std::mutex> grow(uploadGrowMtx_);
        std::lock_guard<std::mutex> lk(checkMtx_);
        for (const CheckFilled_& f : checkFilled_)
        {
            const std::size_t size = f.bytes.size();
            std::vector<std::uint8_t> now(size);
            std::memcpy(now.data(), f.cpu, size);
            if (std::memcmp(now.data(), f.bytes.data(), size) == 0)
            {
                continue;
            }
            std::size_t movedBytes = 0u;
            std::string words;
            int shown = 0;
            for (std::size_t w = 0; w + 4u <= size; w += 4u)
            {
                if (std::memcmp(now.data() + w, f.bytes.data() + w, 4u) == 0)
                {
                    continue;
                }
                movedBytes += 4u;
                if (shown++ < 8)
                {
                    std::uint32_t was = 0u;
                    std::uint32_t is = 0u;
                    std::memcpy(&was, f.bytes.data() + w, 4u);
                    std::memcpy(&is, now.data() + w, 4u);
                    char text[64];
                    std::snprintf(text, sizeof(text), " +%zu:%08x->%08x", w, was, is);
                    words += text;
                }
            }
            const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(f.cpu);
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(uploadCPU_);
            const long long offset = (at >= base && at < base + uploadSize_) ? static_cast<long long>(at - base) : -1;
            std::string by;
            for (void* c : f.callers)
            {
                AppendCodeAddress(by, c);
            }
            g_ringBytesMoved.fetch_add(movedBytes, std::memory_order_relaxed);
            LOG_ERROR(logging::LogCategory::RenderRhi,
                      "upload ring: frame {}'s block at {} (+{} bytes; -1 = an overflow chunk), filled by{}, moved "
                      "before the recording ended (~{} bytes). Words:{}",
                      frame, offset, size, by, movedBytes, words);
        }

        checkSnap_.clear();
        checkSnap_.resize(1u + extraUploads_.size());
        const UINT used = std::min(uploadOffset_.load(std::memory_order_acquire), uploadSize_);
        checkSnap_[0].resize(used);
        if (used != 0u)
        {
            std::memcpy(checkSnap_[0].data(), uploadCPU_, used);
        }
        for (std::size_t i = 0; i < extraUploads_.size(); ++i)
        {
            const UINT chunkUsed = std::min(extraUploads_[i].offset.load(std::memory_order_acquire), extraUploads_[i].size);
            checkSnap_[i + 1u].resize(chunkUsed);
            if (chunkUsed != 0u)
            {
                std::memcpy(checkSnap_[i + 1u].data(), extraUploads_[i].cpu, chunkUsed);
            }
        }
        checkFrame_ = frame;
        checkSealed_ = true;
    }
    descAlloc_.SealForCheck(frame); // the frame's descriptor tables are final too
    if (render::g_uploadRingCheckSelftest && frame == 3u)
    {
        descAlloc_.Alloc(1u); // one ring entry nobody reads, taken after the seal: must be reported
    }

    if (canary)
    {
        canary[0] = 0x5A5A5A5Au;
        LOG_INFO(logging::LogCategory::RenderRhi,
                 "upload ring self-test: wrote into a filled and a sealed canary of frame {} and took a descriptor after "
                 "the seal; the check must report all three",
                 frame);
    }
}

std::size_t FrameResource::VerifyUpload(const char* when)
{
    std::lock_guard<std::mutex> grow(uploadGrowMtx_);
    std::lock_guard<std::mutex> lk(checkMtx_);
    if (!checkSealed_)
    {
        return 0u;
    }
    g_ringChecks.fetch_add(1u, std::memory_order_relaxed);

    const auto describe = [](const std::vector<CheckAlloc_>& allocs, UINT region, UINT at) -> std::string
    {
        for (const CheckAlloc_& a : allocs)
        {
            if (a.region == region && at >= a.offset && at < a.offset + a.size)
            {
                std::string s = "[" + std::to_string(a.offset) + ", +" + std::to_string(a.size) + ") by";
                for (void* c : a.callers)
                {
                    AppendCodeAddress(s, c);
                }
                return s;
            }
        }
        return "none";
    };

    std::size_t moved = 0u;
    int runsLogged = 0;
    int runs = 0;
    std::vector<std::uint8_t> now;
    for (std::size_t r = 0; r < checkSnap_.size(); ++r)
    {
        std::vector<std::uint8_t>& snap = checkSnap_[r];
        const UINT size = static_cast<UINT>(snap.size());
        if (size == 0u)
        {
            continue;
        }
        const std::uint8_t* src = nullptr;
        if (r == 0u)
        {
            src = static_cast<const std::uint8_t*>(uploadCPU_);
        }
        else if (r - 1u < extraUploads_.size() && extraUploads_[r - 1u].size >= size)
        {
            src = static_cast<const std::uint8_t*>(extraUploads_[r - 1u].cpu);
        }
        if (src == nullptr)
        {
            continue;
        }
        now.resize(size);
        std::memcpy(now.data(), src, size); // one sequential read of write-combined memory
        if (std::memcmp(now.data(), snap.data(), size) == 0)
        {
            continue;
        }

        UINT i = 0u;
        while (i < size)
        {
            if (now[i] == snap[i])
            {
                ++i;
                continue;
            }
            // A run: differing bytes closer than 64 apart are one write.
            const UINT begin = i;
            UINT end = i + 1u;
            UINT gap = 0u;
            for (UINT j = end; j < size && gap < 64u; ++j)
            {
                if (now[j] != snap[j]) { end = j + 1u; gap = 0u; }
                else { ++gap; }
            }
            UINT runBytes = 0u;
            for (UINT j = begin; j < end; ++j)
            {
                runBytes += now[j] != snap[j] ? 1u : 0u;
            }
            moved += runBytes;
            ++runs;
            if (runsLogged < 8)
            {
                ++runsLogged;
                std::string words;
                const UINT w0 = begin & ~3u;
                for (UINT w = w0; w < end && w < w0 + 32u && w + 4u <= size; w += 4u)
                {
                    std::uint32_t was = 0u;
                    std::uint32_t is = 0u;
                    std::memcpy(&was, snap.data() + w, 4u);
                    std::memcpy(&is, now.data() + w, 4u);
                    char text[64];
                    std::snprintf(text, sizeof(text), " +%u:%08x->%08x", w, was, is);
                    words += text;
                }
                LOG_ERROR(logging::LogCategory::RenderRhi,
                          "upload ring: frame {} region {} bytes [{}, {}) moved {} ({} bytes). Allocation: {}. "
                          "The slot's allocation there a frame before: {}. Words:{}",
                          checkFrame_, r, begin, end, when, runBytes,
                          describe(checkAllocs_, static_cast<UINT>(r), begin),
                          describe(checkAllocsPrev_, static_cast<UINT>(r), begin), words);
            }
            i = end;
        }
        snap = now; // the next check reports only what moves after this one
    }

    if (moved != 0u)
    {
        g_ringBytesMoved.fetch_add(moved, std::memory_order_relaxed);
        LOG_ERROR(logging::LogCategory::RenderRhi, "upload ring: frame {} {}: {} bytes moved in {} runs",
                  checkFrame_, when, moved, runs);
    }
    LOG_INFO_THROTTLED(std::chrono::seconds(10), logging::LogCategory::RenderRhi,
                       "upload ring check: {} checks, {} bytes moved so far",
                       g_ringChecks.load(std::memory_order_relaxed), g_ringBytesMoved.load(std::memory_order_relaxed));
    return moved;
}

void FrameResource::InitUpload(ID3D12Device* dev, UINT bytes) {
    if (bytes == 0u) {
        bytes = 1u << 20; // 1 MB by default
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc = { 1, 0 };
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> tmp;
    ThrowIfFailed(gpu::CreateCommittedResource(dev,
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        MemTag::FrameUpload, L"FrameUpload.Ring",
        IID_PPV_ARGS(tmp.ReleaseAndGetAddressOf())));

    upload_ = tmp;
    uploadSize_ = bytes;
    uploadGPU_ = upload_->GetGPUVirtualAddress();
    uploadOffset_.store(0u, std::memory_order_release);

    // Persistent map
    D3D12_RANGE rge{ 0,0 };
    void* p = nullptr;
    ThrowIfFailed(upload_->Map(0, &rge, &p));
    uploadCPU_ = p;

    // Clear fallback chunks
    extraUploads_.clear();
}

void FrameResource::ResetUpload() {
    uploadOffset_.store(0u, std::memory_order_release);
    // Release fallback chunks from the previous frame
    extraUploads_.clear();
    if (render::g_uploadRingCheck) {
        std::lock_guard<std::mutex> lk(checkMtx_);
        checkAllocsPrev_.swap(checkAllocs_);
        checkAllocs_.clear();
        checkSnap_.clear();
        checkFilled_.clear();
        checkSealed_ = false;
    }
}

FrameResource::DynamicAlloc FrameResource::AllocDynamic(UINT size, UINT align) {
    DynamicAlloc out{};
    if (size == 0u) {
        return out;
    }
    if (align == 0u) {
        align = 16u;
    }
    
    auto AlignUp_ = [](UINT v, UINT a) -> UINT {
        const UINT m = a - 1u;
        return (v + m) & ~m;
        };

    // Try the main buffer first
    for (;;) {
        UINT old = uploadOffset_.load(std::memory_order_relaxed);
        UINT aligned = AlignUp_(old, align);
        if (aligned + size <= uploadSize_) {
            if (uploadOffset_.compare_exchange_weak(old, aligned + size,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
                out.cpu = static_cast<uint8_t*>(uploadCPU_) + aligned;
                out.gpu = uploadGPU_ + aligned;
                out.size = size;
                out.offset = aligned;
                out.resource = upload_.Get();
                if (render::g_uploadRingCheck) {
                    NoteAlloc_(0u, aligned, size);
                }
                return out;
            }
            else {
                continue;
            }
        }
        break;
    }

    // Overflow: take or create a fallback chunk and allocate from it.
    {
        std::lock_guard<std::mutex> lk(uploadGrowMtx_);

        // Try existing chunks
        for (std::size_t chunkIndex = 0; chunkIndex < extraUploads_.size(); ++chunkIndex) {
            auto& ch = extraUploads_[chunkIndex];
            for (;;) {
                UINT old = ch.offset.load(std::memory_order_relaxed);
                UINT aligned = AlignUp_(old, align);
                if (aligned + size <= ch.size) {
                    if (ch.offset.compare_exchange_weak(old, aligned + size,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        out.cpu = static_cast<uint8_t*>(ch.cpu) + aligned;
                        out.gpu = ch.gpu + aligned;
                        out.size = size;
                        out.offset = aligned;
                        out.resource = ch.res.Get();
                        if (render::g_uploadRingCheck) {
                            NoteAlloc_(static_cast<UINT>(chunkIndex + 1u), aligned, size);
                        }
                        return out;
                    }
                    else {
                        continue;
                    }
                }
                break;
            }
        }

        // Create a new chunk (grow multiplicatively)
        UploadChunk_ ch{};
        ch.size = std::max<UINT>(AlignUp_(size, 65536u), uploadSize_); // no smaller than the base buffer

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        // For single-GPU we can specify the node mask explicitly
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = ch.size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc = { 1, 0 };
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;

        // Obtain the device from the existing upload_ resource (it is the same ID3D12Device)
        Microsoft::WRL::ComPtr<ID3D12Device> dev;
        ThrowIfFailed(upload_->GetDevice(IID_PPV_ARGS(dev.GetAddressOf())));

        ThrowIfFailed(gpu::CreateCommittedResource(dev.Get(),
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            MemTag::FrameUpload, L"FrameUpload.Overflow",
            IID_PPV_ARGS(ch.res.ReleaseAndGetAddressOf())));

        // persistent map
        D3D12_RANGE rge{ 0, 0 };
        void* p = nullptr;
        ThrowIfFailed(ch.res->Map(0, &rge, &p));
        ch.cpu = p;
        ch.gpu = ch.res->GetGPUVirtualAddress();
        ch.offset.store(0u, std::memory_order_release);

        // Immediately carve out space from it
        const UINT aligned = AlignUp_(0u, align);
        ch.offset.store(aligned + size, std::memory_order_release);

        out.cpu = static_cast<uint8_t*>(ch.cpu) + aligned;
        out.gpu = ch.gpu + aligned;
        out.size = size;
        out.offset = aligned;
        out.resource = ch.res.Get(); // the object stays where it is when the ComPtr moves into the vector

        extraUploads_.push_back(std::move(ch));
        if (render::g_uploadRingCheck) {
            NoteAlloc_(static_cast<UINT>(extraUploads_.size()), aligned, size);
        }
        return out;
    }
}