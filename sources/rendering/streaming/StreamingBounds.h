#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/math/Math.h"

class Frustum;
struct SceneFrameData;

namespace streaming {

class TextureStreaming;

// A texture one box references. `texelFactor` = uvDensity x scale / tiling: the world size of a
// unit-UV square on that slot (TextureStreamingTypes.h:151-157). While the set is built `texture`
// is the registry entry (with its `gen`); TakeResolved turns it into the snapshot's texture index.
struct BoundsRef
{
    static constexpr std::uint32_t kNone = ~0u; // its texture is not in this snapshot
    std::uint32_t texture = kNone;
    std::uint32_t gen = 0;
    float texelFactor = 0.0f;
};

// One placed object's box -- UE FBounds4 flattened (no SIMD). Its refs are a range of the set's
// one array: the chunks of a chunked mesh share the object's range.
struct BoundsEntry
{
    Math::float3 center{};
    Math::float3 halfExtents{};
    float lastSeenAge = 0.0f; // seconds since this box last intersected the camera frustum, at its slice
    bool terrain = false;     // RenderLayer::Terrain: UE terrain textures skip split requests and hidden scale
    std::uint32_t firstRef = 0, refCount = 0;
};

struct BoundsSet
{
    std::vector<BoundsEntry> entries;
    std::vector<BoundsRef> refs;
    void Clear() { entries.clear(); refs.clear(); }
};

// The snapshot's boxes, built a slice of the frame's object list at a time over the manager's
// idle stages (UE refreshes its instance views incrementally between full updates the same way):
// all of wind_test's 1499 objects in one frame cost the Debug build an 11 ms frame every five.
// A box per object that carries materials (statics and GPU-instanced groups alike, the group box
// being the union of its instances), visibility from a CPU frustum test against the non-jittered
// camera of the frame that built its slice. A new object list (its storage or length changed)
// restarts the set, so a build never mixes two lists.
class BoundsBuilder
{
public:
    // Adds up to `maxObjects` more objects (0 = the rest). True once every object is in.
    bool Step(const SceneFrameData& frame, const Frustum& frustum, float now, const TextureStreaming& ts, std::size_t maxObjects);
    // Hands the finished set over (swapped, so both keep their capacity) and starts the next one.
    // `textureSlot` maps a registry entry to the snapshot's texture index (-1 = not in it); a ref
    // to anything else, or to an entry re-registered since its slice, becomes BoundsRef::kNone.
    void TakeResolved(BoundsSet& out, const std::vector<int>& textureSlot, const TextureStreaming& ts);
    void Reset() { set_.Clear(); cursor_ = 0; list_ = nullptr; listSize_ = 0; }
    static std::size_t ObjectCount(const SceneFrameData& frame);
    // Forgets objects not seen for a while (level switches leave dead keys behind).
    void PruneLastSeen(float now);

private:
    BoundsSet set_;
    std::size_t cursor_ = 0;
    const void* list_ = nullptr;
    std::size_t listSize_ = 0;
    std::unordered_map<const void*, float> lastSeen_; // persists across sets, keyed by object (+ chunk) address
};

} // namespace streaming
