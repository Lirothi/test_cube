#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "core/threading/Knob.h"

class RenderableObject;

// The water kept out of a hull (2026-11-09, owner: "the water floods the inside of the boat; make a mask for marked meshes so
// the water is not drawn inside them"). A mesh asset with mesh.json "waterMask": true has the convex hull of its LOD 0 as
// a volume the ocean's surface is not drawn inside: the surface's pixel shader tests its point against the hull's planes
// in the model's space, through the pose the frame draws the object at (its render offset: the waves, the physics). The
// hull bridges the cockpit, so the water standing in it (a boat's floor under the sea's level, a wave over the transom) is
// gone wherever the camera is -- outside, at the helm inside it. Water outside the hull is never inside a convex volume
// that only wraps the mesh.
namespace ocean
{
// `--set ocean.waterMask:0` (transient): no water kept out, the A/B.
inline bool g_waterMask = true;
inline bool g_waterMask_game = true;
inline const bool g_waterMask_knob = ::knob::Register(g_waterMask_game, g_waterMask, "ocean::g_waterMask");

// A mesh's convex hull: the planes of its faces in the model's space (xyz the outward normal; inside where dot(n, p) + w
// <= 0) and their box.
struct WaterMaskHull
{
    std::vector<Math::float4> planes;
    Math::float3 boxMin = Math::float3(0.0f, 0.0f, 0.0f);
    Math::float3 boxMax = Math::float3(0.0f, 0.0f, 0.0f);
};

// One a frame: a hull and the world-to-model transform its object is drawn at.
struct WaterMaskInstance
{
    std::shared_ptr<const WaterMaskHull> hull;
    Math::mat4 worldToModel;
};

// What the surface's constant buffer holds (shaders/ocean_surface_surf_sim.hlsli WaterMaskCB): the hulls a frame and the
// planes every hull takes, at most.
constexpr std::uint32_t kWaterMaskMaxHulls = 4u;
constexpr std::uint32_t kWaterMaskMaxPlanes = 128u;

namespace watermask
{
    // RenderableObject::SetWaterMask: the registry the frame reads, never a scene walk. Register builds the hull of the
    // object's geometry the first time a geometry is met (one per path, kept for the process).
    void Register(RenderableObject* object);
    void Unregister(RenderableObject* object);
    // At Scene::Sync, after the physics poses: every registered object that is visible and has a hull, with the inverse of
    // the matrix the frame draws it at.
    void Collect(std::vector<WaterMaskInstance>& out);
}
} // namespace ocean
