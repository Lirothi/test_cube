#include "rendering/streaming/TextureRetireBin.h"

#include <algorithm>

namespace streaming {

void TextureRetireBin::Retire(GpuResource&& res, UINT64 bytes, std::uint64_t retireFrame)
{
    Entry e;
    e.res = std::move(res);
    e.bytes = bytes;
    e.retireFrame = retireFrame;
    bytes_ += bytes;
    entries_.push_back(std::move(e));
}

void TextureRetireBin::Release(std::uint64_t frameNo)
{
    for (Entry& e : entries_)
    {
        if (e.retireFrame <= frameNo) { bytes_ -= e.bytes; }
    }
    std::erase_if(entries_, [frameNo](const Entry& e) { return e.retireFrame <= frameNo; });
}

void TextureRetireBin::ReleaseAll()
{
    entries_.clear();
    bytes_ = 0;
}

} // namespace streaming
