#pragma once

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>
#include <vector>

// Texture streaming, step A1 (docs/texture_streaming_vt_plan.md). A DDS file is the streaming
// container as it is: every mip is tight-packed right after the previous one, so the byte range of
// any mip follows from the header alone. This table is that arithmetic, done once at load, so a
// later stream-in can `ReadFile` exactly the tail it wants without re-parsing anything.
//
// UE keeps the same contract on its side (FTexture2DResource::WarnRequiresTightPackedMip,
// Texture2DResource.cpp:261-270): a streamable mip MUST be tight-packed, and a pitch that disagrees
// with the format's own row size is a warning, not something the runtime accommodates.
namespace streaming {

// How many of the SMALLEST mips are always resident and never streamed. UE's
// NUM_INLINE_DERIVED_MIPS (TextureDerivedDataTask.h:31) == GMinTextureResidentMipCount
// (TextureDerivedData.cpp:4369): the cook inlines this many, the runtime never drops below it. A1
// only declares the number; A3's manager is the first thing that stops short of the full chain.
inline constexpr UINT kNonStreamingMips = 7;

struct DdsMipTable
{
    struct Mip
    {
        UINT64 fileOffset = 0;    // absolute byte offset of this mip's first byte in the .dds
        UINT width = 0;           // texels
        UINT height = 0;
        UINT rowPitchBytes = 0;   // one tight row: BC = a row of 4x4 blocks, linear = width x bpp
        UINT sliceBytes = 0;      // rowPitchBytes x rows (BC: block rows) == the mip's byte size
    };

    std::vector<Mip> mips;        // mips[0] = the largest level IN THE FILE (file mip order)
    UINT mipCount = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // the file's own format, sRGB tag included
    UINT64 headerBytes = 0;       // 128 (legacy) or 148 (DX10 header); mips[0].fileOffset == this

    // Fill from the header fields. `bytesPerBlockOrPixel` and `isBC` come from the loader's own
    // format table (Texture2D.cpp FormatPair), so the two can never disagree about a format.
    void Build(UINT width, UINT height, UINT count, DXGI_FORMAT fileFormat, UINT64 header,
               bool isBC, UINT bytesPerBlockOrPixel)
    {
        mips.clear();
        mips.reserve(count);
        mipCount = count;
        format = fileFormat;
        headerBytes = header;
        UINT64 offset = header;
        UINT w = width, h = height;
        for (UINT m = 0; m < count; ++m)
        {
            Mip mip{};
            mip.fileOffset = offset;
            mip.width = w;
            mip.height = h;
            if (isBC)
            {
                const UINT bw = (w + 3u) / 4u > 0u ? (w + 3u) / 4u : 1u;
                const UINT bh = (h + 3u) / 4u > 0u ? (h + 3u) / 4u : 1u;
                mip.rowPitchBytes = bw * bytesPerBlockOrPixel;
                mip.sliceBytes = mip.rowPitchBytes * bh;
            }
            else
            {
                mip.rowPitchBytes = w * bytesPerBlockOrPixel;
                mip.sliceBytes = mip.rowPitchBytes * h;
            }
            offset += mip.sliceBytes;
            mips.push_back(mip);
            w = w > 1u ? w >> 1 : 1u;
            h = h > 1u ? h >> 1 : 1u;
        }
    }

    // Sum of every mip's bytes == what the file must hold after its header.
    UINT64 DataBytes() const
    {
        UINT64 total = 0;
        for (const Mip& m : mips) { total += m.sliceBytes; }
        return total;
    }

    // Bytes of the tail [firstMip, mipCount): what a partial (streamable) load reads.
    UINT64 TailBytes(UINT firstMip) const
    {
        UINT64 total = 0;
        for (UINT m = firstMip; m < mipCount; ++m) { total += mips[m].sliceBytes; }
        return total;
    }
};

} // namespace streaming
