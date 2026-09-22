#include "rendering/streaming/TextureUploadRing.h"

#include "core/logging/Log.h"

namespace streaming {

bool TextureUploadRing::Init(ID3D12Device* device, UINT64 bytes)
{
    Shutdown();
    if (!device || bytes == 0) { return false; }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer_))))
    {
        LOG_ERROR(logging::LogCategory::Render, "texture upload ring: CreateCommittedResource({} bytes) failed", bytes);
        buffer_.Reset();
        return false;
    }
    buffer_->SetName(L"Streaming.UploadRing");
    D3D12_RANGE noRead{ 0, 0 };
    void* mapped = nullptr;
    if (FAILED(buffer_->Map(0, &noRead, &mapped)))
    {
        LOG_ERROR(logging::LogCategory::Render, "texture upload ring: Map failed");
        buffer_.Reset();
        return false;
    }
    cpu_ = static_cast<std::uint8_t*>(mapped);
    capacity_ = bytes;
    head_ = tail_ = inUse_ = 0;
    entries_.clear();
    return true;
}

void TextureUploadRing::Shutdown()
{
    if (buffer_ && cpu_) { buffer_->Unmap(0, nullptr); }
    buffer_.Reset();
    cpu_ = nullptr;
    capacity_ = head_ = tail_ = inUse_ = 0;
    entries_.clear();
}

// head_ is always the first live entry's offset; tail_ the next free byte. head_ < tail_ = one
// live range [head_, tail_); head_ >= tail_ with entries = wrapped ([head_, cap) + [0, tail_)),
// or full when equal.
bool TextureUploadRing::Alloc(UINT64 bytes, std::uint32_t& outEntry, UINT64& outOffset)
{
    if (!buffer_ || bytes == 0) { return false; }
    bytes = (bytes + kPlacementAlign - 1) & ~(kPlacementAlign - 1);
    if (bytes > capacity_) { return false; }
    UINT64 offset = 0;
    if (entries_.empty())
    {
        head_ = tail_ = 0;
    }
    else if (head_ < tail_)
    {
        if (tail_ + bytes <= capacity_) { offset = tail_; }
        else if (bytes <= head_) { offset = 0; } // wrap; the unused tail is wasted until the head moves
        else { return false; }
    }
    else
    {
        if (tail_ + bytes > head_) { return false; }
        offset = tail_;
    }
    Entry e{};
    e.id = nextId_++;
    e.offset = offset;
    e.bytes = bytes;
    entries_.push_back(e);
    tail_ = offset + bytes;
    inUse_ += bytes;
    outEntry = e.id;
    outOffset = offset;
    return true;
}

void TextureUploadRing::Stamp(std::uint32_t entry, std::uint64_t releaseFrame)
{
    for (Entry& e : entries_)
    {
        if (e.id == entry) { e.releaseFrame = releaseFrame; return; }
    }
}

void TextureUploadRing::ReleaseCompleted(std::uint64_t frameNo)
{
    while (!entries_.empty() && entries_.front().releaseFrame <= frameNo)
    {
        inUse_ -= entries_.front().bytes;
        entries_.pop_front();
    }
    if (entries_.empty()) { head_ = tail_ = 0; }
    else { head_ = entries_.front().offset; }
}

} // namespace streaming
