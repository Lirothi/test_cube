#include "materials/TextureDecodeCache.h"

#include <mutex>
#include <unordered_map>

#include "core/task/TaskSystem.h"

namespace texdecode {
namespace {

struct KeyHash
{
    std::size_t operator()(const Key& k) const noexcept
    {
        std::size_t h = std::hash<std::wstring>{}(k.path);
        h = h * 1099511628211ull ^ static_cast<std::size_t>(k.usage);
        h = h * 1099511628211ull ^ static_cast<std::size_t>(k.normalIsRG);
        h = h * 1099511628211ull ^ std::hash<float>{}(k.alphaCoverageCutoff);
        return h;
    }
};

// Guards both maps. Held only around the map operations, NEVER across a decode — a decode of a
// 25 MB PNG takes ~2 s and two workers prewarming different textures must not serialise on it.
std::mutex gMutex;
std::unordered_map<Key, DecodedImage, KeyHash> gEntries;
// Keys a worker is decoding RIGHT NOW. Without this, two thumbnails sharing a material both see an
// empty cache and both decode the same file.
//
// The mapped bool is the entry's VALIDITY: EvictIf clears it for a file a re-import just rewrote,
// and the worker throws its result away instead of publishing bytes that describe the previous
// import. A decode takes seconds, so "started before the import, lands after it" is not a
// theoretical window.
std::unordered_map<Key, bool, KeyHash> gInFlight;
// Concurrent decodes. Deliberately small: these run on the SHARED worker pool.
constexpr std::size_t kMaxDecodesInFlight = 2;
std::size_t gBytes = 0;
// Take() outcomes. A miss means the caller decoded inline — on the main thread, for thumbnails.
std::uint32_t gTaken = 0;
std::uint32_t gMissed = 0;

Key MakeKey(const Texture2D::CreateDesc& desc)
{
    // The RESOLVED path, so a request for the .png and one that lands on its .dds sibling agree —
    // and so a prewarm of an imported texture is correctly a no-op rather than a phantom entry.
    Key k;
    k.path = Texture2D::ResolveSourcePath(desc.path);
    k.usage = desc.usage;
    k.normalIsRG = desc.normalIsRG;
    k.alphaCoverageCutoff = desc.alphaCoverageCutoff;
    return k;
}

std::size_t ImageBytes(const DecodedImage& img)
{
    std::size_t n = 0;
    for (const auto& m : img.mips) { n += m.size(); }
    return n;
}

// Retire an in-flight claim and say whether its result is still wanted. Called under gMutex by
// whoever finished the decode. False = the file was rewritten while we were decoding it.
bool FinishInFlight(const Key& key)
{
    const auto it = gInFlight.find(key);
    if (it == gInFlight.end()) { return false; } // cannot happen: only the decoder erases its claim
    const bool stillValid = it->second;
    gInFlight.erase(it);
    return stillValid;
}

} // namespace

Status Request(const Texture2D::CreateDesc& desc)
{
    const Key key = MakeKey(desc);
    // A DDS needs no decode at all — the caller's normal path is a read plus an upload.
    if (key.path.size() >= 4 &&
        (key.path.compare(key.path.size() - 4, 4, L".dds") == 0 ||
         key.path.compare(key.path.size() - 4, 4, L".DDS") == 0))
    {
        return Status::NotNeeded;
    }

    {
        std::lock_guard<std::mutex> lk(gMutex);
        if (gEntries.find(key) != gEntries.end()) { return Status::Ready; }
        if (gInFlight.find(key) != gInFlight.end()) { return Status::Pending; }
        // The worker pool is shared with the render graph's parallel recording. A folder of
        // thirteen meshes is ~39 textures; dispatching them all at once hands every worker a
        // multi-second decode and starves the frame. The caller retries anyway, so REFUSING to
        // dispatch is the throttle. (Same reasoning as kMaxPreflightJobsInFlight.)
        if (gInFlight.size() >= kMaxDecodesInFlight) { return Status::Pending; }
        gInFlight[key] = true; // claimed here so a retry next frame does not dispatch again
    }

    // SubmitDetach, NOT DispatchTrack. The main loop calls WaitForTrackedAsyncTasks() at the top
    // of EVERY frame (App.cpp:493), so a TRACKED task is joined by the next frame — the frame
    // would wait for a two-second PNG decode and reproduce the exact stall this exists to remove,
    // one layer down. Every other long job in this path (preview init, preflight, PNG encode)
    // already uses SubmitDetach; this was the one that did not.
    TaskSystem::Get().SubmitDetach([desc, key]() mutable
    {
        DecodedImage img;
        UINT w = 0, h = 0;
        img.valid = Texture2D::DecodeToMips(desc, img.mips, w, h);
        img.width = w;
        img.height = h;

        std::lock_guard<std::mutex> lk(gMutex);
        const bool wanted = FinishInFlight(key);
        if (!img.valid || !wanted) { return; }
        gBytes += ImageBytes(img);
        gEntries.emplace(key, std::move(img));
    });
    return Status::Pending;
}

void Prewarm(const Texture2D::CreateDesc& desc)
{
    const Key key = MakeKey(desc);

    {
        std::lock_guard<std::mutex> lk(gMutex);
        if (gEntries.find(key) != gEntries.end()) { return; }   // already decoded
        if (gInFlight.find(key) != gInFlight.end()) { return; } // another worker is on it
        gInFlight[key] = true;
    }

    DecodedImage img;
    UINT w = 0, h = 0;
    img.valid = Texture2D::DecodeToMips(desc, img.mips, w, h);
    img.width = w;
    img.height = h;

    std::lock_guard<std::mutex> lk(gMutex);
    const bool wanted = FinishInFlight(key);
    if (!img.valid || !wanted) { return; } // a DDS, a failed decode, or a re-import landed while we
                                           // decoded: leave the cache empty and let CreateFromFile
                                           // take its normal path
    gBytes += ImageBytes(img);
    gEntries.emplace(key, std::move(img));
}

bool Take(const Texture2D::CreateDesc& desc, DecodedImage& out)
{
    const Key key = MakeKey(desc);
    std::lock_guard<std::mutex> lk(gMutex);
    const auto it = gEntries.find(key);
    if (it == gEntries.end()) { ++gMissed; return false; }
    ++gTaken;
    out = std::move(it->second);
    gBytes -= ImageBytes(out);
    gEntries.erase(it);
    return true;
}

void Clear()
{
    std::lock_guard<std::mutex> lk(gMutex);
    gEntries.clear();
    gBytes = 0;
    // gInFlight is deliberately NOT cleared: a worker is still writing those keys and will erase
    // its own entry when it finishes.
}

std::size_t EvictIf(const std::function<bool(const std::wstring&)>& pred)
{
    if (!pred) { return 0; }
    std::lock_guard<std::mutex> lk(gMutex);

    std::size_t dropped = 0;
    for (auto it = gEntries.begin(); it != gEntries.end(); )
    {
        if (pred(it->first.path))
        {
            gBytes -= ImageBytes(it->second);
            it = gEntries.erase(it);
            ++dropped;
        }
        else
        {
            ++it;
        }
    }
    // Same file, decode still running: mark the claim invalid so the worker discards its result
    // rather than replacing what we just dropped with the pre-import image.
    for (auto& [key, valid] : gInFlight)
    {
        if (valid && pred(key.path)) { valid = false; }
    }
    return dropped;
}

void Stats(std::uint32_t& taken, std::uint32_t& missed)
{
    std::lock_guard<std::mutex> lk(gMutex);
    taken = gTaken;
    missed = gMissed;
}

void ResetStats()
{
    std::lock_guard<std::mutex> lk(gMutex);
    gTaken = 0;
    gMissed = 0;
}

std::size_t BytesResident()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gBytes;
}

} // namespace texdecode
