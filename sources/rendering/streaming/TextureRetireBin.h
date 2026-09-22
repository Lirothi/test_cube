#pragma once

#include <d3d12.h>

#include <cstdint>
#include <vector>

#include "rendering/core/ResourceDeclarations.h"

namespace streaming {

// Old texture resources after a swap, freed once every frame that could have read them has
// completed -- per-entry frame stamps, the rt.as.retired pattern (plan A2.3).
class TextureRetireBin
{
public:
    void Retire(GpuResource&& res, UINT64 bytes, std::uint64_t retireFrame);
    void Release(std::uint64_t frameNo);
    void ReleaseAll(); // GPU idle by contract of the caller

    std::size_t Count() const { return entries_.size(); }
    UINT64 Bytes() const { return bytes_; }

private:
    struct Entry
    {
        GpuResource res;
        UINT64 bytes = 0;
        std::uint64_t retireFrame = 0;
    };
    std::vector<Entry> entries_;
    UINT64 bytes_ = 0;
};

} // namespace streaming
