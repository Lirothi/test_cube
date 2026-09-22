#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/math/Math.h"

class Frustum;
struct SceneFrameData;

namespace streaming {

class TextureStreaming;

// One placed object's box with the textures it references -- UE FBounds4 flattened (a few
// hundred entries here, no SIMD). `texelFactor` = uvDensity x scale / tiling: the world size of
// a unit-UV square on that slot (TextureStreamingTypes.h:151-157).
struct BoundsEntry
{
    Math::float3 center{};
    Math::float3 halfExtents{};
    float lastSeenAge = 0.0f; // seconds since this box last intersected the camera frustum
    bool terrain = false;     // RenderLayer::Terrain: UE terrain textures skip split requests and hidden scale
    struct Ref { std::uint32_t texture; float texelFactor; }; // texture = index into the snapshot list
    std::vector<Ref> refs;
};

// Rebuilt every snapshot from the frame's object list: a box per object that carries materials
// (statics and GPU-instanced groups alike, the group box being the union of its instances),
// visibility from a CPU frustum test against the non-jittered camera. `lastSeen` persists across
// frames keyed by object address; `textureSlot` maps a registry entry to the snapshot's texture
// index (-1 = not in this snapshot). Returns the entries.
void BuildBounds(const SceneFrameData& frame, const Frustum& frustum, float now,
                 std::unordered_map<const void*, float>& lastSeen,
                 const std::vector<int>& textureSlot,
                 std::vector<BoundsEntry>& out);

} // namespace streaming
