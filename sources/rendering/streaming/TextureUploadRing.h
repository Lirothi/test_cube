#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>

namespace streaming {

// Persistent-mapped UPLOAD ring for streamed mips (plan A2.1). Allocation and release on the main
// thread only; the IO worker just writes bytes into Cpu() + offset. Entries release in allocation
// order once their frame stamp has passed (the copy's frame + kFrameCount), so one slow read at the
// head holds the ring -- sized (50 MB) well above a frame's traffic on purpose.
class TextureUploadRing
{
public:
    static constexpr UINT64 kPlacementAlign = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; // 512
    static constexpr UINT64 kUnstamped = ~0ull;

    bool Init(ID3D12Device* device, UINT64 bytes);
    void Shutdown();
    bool Ready() const { return buffer_ != nullptr; }

    // Reserve `bytes` (rounded up to 512) contiguous bytes; false when the ring has no room yet.
    // Returns an entry id and the byte offset from the buffer start.
    bool Alloc(UINT64 bytes, std::uint32_t& outEntry, UINT64& outOffset);
    // The frame after which the entry may be reused; kUnstamped entries block the head.
    void Stamp(std::uint32_t entry, std::uint64_t releaseFrame);
    void ReleaseCompleted(std::uint64_t frameNo);

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
        std::uint64_t releaseFrame = kUnstamped;
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
