#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>

namespace streaming {

// Persistent-mapped UPLOAD ring for streamed mips (plan A2.1). Allocation and release on the main
// thread only; the IO worker just writes bytes into Cpu() + offset. Entries release in allocation
// order once nothing on the GPU can read them any more -- an entry a copy was recorded from goes
// through render::deferred (released once the GPU has finished that frame NUMBER; until the second
// review of wave 1, item 3, it was kFrameCount frames counted here) -- so one slow read at the head
// holds the ring: sized (50 MB) well above a frame's traffic on purpose.
class TextureUploadRing
{
public:
    static constexpr UINT64 kPlacementAlign = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; // 512

    bool Init(ID3D12Device* device, UINT64 bytes);
    void Shutdown();
    bool Ready() const { return buffer_ != nullptr; }

    // Reserve `bytes` (rounded up to 512) contiguous bytes; false when the ring has no room yet.
    // Returns an entry id and the byte offset from the buffer start. Until one of the two calls below,
    // the entry blocks the head.
    bool Alloc(UINT64 bytes, std::uint32_t& outEntry, UINT64& outOffset);
    // Nothing on the GPU read the entry (its swap was dropped before a copy): free at the next ReleaseCompleted.
    void ReleaseUnread(std::uint32_t entry);
    // A copy from the entry was recorded in the frame being built: free once the GPU has finished it (the service's
    // release step marks it; ReleaseCompleted then takes it off the head).
    void RetireAfterCopy(std::uint32_t entry);
    // Frees the head's entries that are done, in allocation order.
    void ReleaseCompleted();

    ID3D12Resource* Buffer() const { return buffer_.Get(); }
    std::uint8_t* Cpu() const { return cpu_; }
    UINT64 Capacity() const { return capacity_; }
    UINT64 BytesInUse() const { return inUse_; }
    std::size_t LiveEntries() const { return entries_.size(); }

private:
    struct Entry
    {
        std::uint32_t id = 0;
        UINT64 offset = 0;
        UINT64 bytes = 0;
        // Null while in use; set by ReleaseUnread (already true) or RetireAfterCopy (true once the service's step ran,
        // on the thread of its boundary). Shared with that step: a step after Shutdown finds its flag still there.
        std::shared_ptr<std::atomic<bool>> done;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    std::uint8_t* cpu_ = nullptr;
    UINT64 capacity_ = 0;
    UINT64 head_ = 0;   // oldest live byte
    UINT64 tail_ = 0;   // next free byte
    UINT64 inUse_ = 0;
    std::uint32_t nextId_ = 1;
    std::deque<Entry> entries_; // allocation order
};

} // namespace streaming
