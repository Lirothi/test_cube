#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "materials/Texture2D.h"

// Off-thread image decoding for Texture2D.
//
// `Texture2D::CreateFromFile`'s WIC path is two very different jobs stapled together: decode the
// file to RGBA8 and build a CPU box-filter mip chain (pure CPU, no device), then upload and create
// the SRV (needs the device and an open command list). Only the second half has to run on the
// thread that owns the command list.
//
// The editor's thumbnail path made that split matter: a material pointing at unimported staging
// PNGs (measured: 25 MB + 21 MB + 12 MB for one rock) spent 2.1 SECONDS on the main thread decoding
// and mip-building them to draw a 256-pixel icon (logs/thumbnail_profile.log).
//
// So: a worker calls Prewarm() to do the CPU half ahead of time, and CreateFromFile then Take()s
// the result instead of decoding. A MISS IS ALWAYS SAFE — CreateFromFile falls back to decoding
// inline, which is exactly today's behaviour. That is deliberate: nothing about correctness depends
// on the prewarm having happened, only the stall does.
namespace texdecode {

struct DecodedImage
{
    std::vector<std::vector<std::uint8_t>> mips; // [0] = full size, then the box-filter chain
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool valid = false;
};

// The decode parameters are part of the identity: `usage`, `normalIsRG` and `alphaCoverageCutoff`
// all change the PIXELS, so two requests for the same file with different settings are different
// images and must not share an entry.
struct Key
{
    std::wstring path;
    Texture2D::Usage usage = Texture2D::Usage::LinearData;
    bool normalIsRG = false;
    float alphaCoverageCutoff = -1.0f;

    bool operator==(const Key& o) const
    {
        return path == o.path && usage == o.usage && normalIsRG == o.normalIsRG &&
               alphaCoverageCutoff == o.alphaCoverageCutoff;
    }
};

// What a non-blocking caller should do about this texture.
enum class Status
{
    Ready,      // decoded and waiting; Take() it
    Pending,    // a worker is decoding it — come back later, do NOT decode here
    NotNeeded,  // resolves to a DDS (or is unloadable): the normal path is already cheap
};

// Ask for a texture WITHOUT decoding it on this thread. Dispatches a worker on first request and
// returns Pending until it lands. This is the inversion that makes "never stall the main thread"
// airtight: the caller does not have to know in advance WHICH textures a material will pull in —
// it discovers them by asking, and bounces until they are all Ready.
Status Request(const Texture2D::CreateDesc& desc);

// Worker-safe. Decodes and stores the image unless an equal entry is already present. Does nothing
// for a path that resolves to a DDS (that path needs no decode) or on failure — a later Take()
// simply misses and the caller decodes inline.
void Prewarm(const Texture2D::CreateDesc& desc);

// Main thread. Moves the decoded image out of the cache; returns false when absent. Entries are
// consumed rather than retained, because a thumbnail texture is decoded once and uploaded once —
// keeping tens of megabytes of RGBA8 alive afterwards would trade a stall for a leak.
bool Take(const Texture2D::CreateDesc& desc, DecodedImage& out);

// Drop everything still pending (a browse cancelled mid-flight leaves prewarmed entries nobody
// will ever take).
void Clear();

// Take() hit/miss counters since the last ResetStats. A miss means CreateFromFile decoded on
// the CALLING thread — which is the main thread for thumbnails, i.e. exactly the stall this
// cache exists to remove. If misses are non-zero the prewarm is not landing in time.
void Stats(std::uint32_t& taken, std::uint32_t& missed);
void ResetStats();

// Bytes currently held. Diagnostics only.
std::size_t BytesResident();

} // namespace texdecode
