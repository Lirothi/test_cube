#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/Math.h"

class Renderer;
class Scene;
class ParticleEmitterObject;
namespace physics { struct VesselHull; }
namespace buoyancy { struct HullFootprint; }

// A planing hull's spray (docs/boat_spray_plan.md): each boat the physics world has gets its own lit particle emitters --
// a sheet of drops and a mist on each side -- born along the forward part of its waterline and thrown outward and up as
// it goes. Scene::Sync hands them the hulls as the frame draws them (the same list the wake takes); the emitters are
// runtime objects of the scene (no editor identity, never saved), made the first frame a boat is seen and kept until the
// level goes. A boat at rest throws nothing.
namespace vfx
{
// The knobs, the ocean preset's (OceanRenderConfig::spray*: the Ocean window, the Inspector's "Boat spray", `--set` /
// MCP `spray.*`), copied in at every Scene::Sync.
struct SprayParams
{
    bool enabled = true;
    float rate = 0.7f;            // of what the presets can keep alive
    float mist = 1.0f;            // the mist's share of that
    float out = 0.4f;             // a drop's speed outward, upward, forward: shares of the hull's (the earth's frame:
    float up = 0.2f;              // the water a hull pushes aside, not the water it passes)
    float forward = 0.1f;
    float slam = 1.0f;            // the burst a slam into a wave throws (1 = the presets' numbers)
    Math::float3 color = Math::float3(0.95f, 0.95f, 0.95f); // the drops' albedo
    float forwardScatter = 0.25f; // the sun's share in the forward lobe
    float opacity = 1.0f;         // times the presets' alpha keys (each at most 1)
    float foamLight = 1.0f;       // lit as a level sheet of the water's foam (1) or as spheres (0): EmitterDesc::litFlat
    float sideFoam = 1.0f;        // the white water along the hull's sides, foam a texel it passes
};

class VesselSpray
{
public:
    // At the hand-over (Scene::Sync), before the objects' SyncSceneState: places and drives every boat's emitters,
    // making a new boat's on its first frame. The drops leave and die at the water's surface under each hull.
    void Sync(Scene& scene, Renderer* renderer, const std::vector<physics::VesselHull>& hulls, const SprayParams& params);
    // The white water a boat lays along its sides this frame (the wake's foam), a line a side (x0, z0, x1, z1) and the
    // foam a second on each: false = none.
    bool SideFoamOf(const void* vessel, Math::float4 (&lines)[2], float& rate) const;
    // The level is going: the emitters go with its objects.
    void Clear() { sets_.clear(); }

    // Readouts: boats with spray, particles a second being thrown now, bursts thrown since the level came.
    size_t Boats() const { return sets_.size(); }
    float Rate() const { return rate_; }
    float SheetRate() const { return sheetRate_; } // the sheets' alone
    int Bursts() const { return bursts_; }
    // Particles alive (a few frames ago) and the pools' capacity, every boat's sheets and mists.
    uint32_t SheetAlive() const { return sheetAlive_; }
    uint32_t SheetCapacity() const { return sheetCapacity_; }
    uint32_t MistAlive() const { return mistAlive_; }
    uint32_t MistCapacity() const { return mistCapacity_; }
    // Drops (sheets and mists) that have entered a hull since their emitters were made (EmitterDesc::collideHulls): a
    // running count -- its rise a second is what would cross a boat without the collision (particles.collideHulls:0).
    uint64_t HullEntries() const { return hullEntries_; }

private:
    // One side of a hull's waterline, in the model's metres (u along its forward, v across it): the spray's root on its
    // forward part, and how far aft the side runs.
    struct Root
    {
        float u0 = 0.0f, v0 = 0.0f; // the root's end at the bow
        float u1 = 0.0f, v1 = 0.0f; // the root's end aft
        float u2 = 0.0f, v2 = 0.0f; // the side's end at the stern
        bool valid = false;
    };
    struct Set
    {
        const void* vessel = nullptr; // the boat's object: a key, never followed
        const buoyancy::HullFootprint* footprint = nullptr;
        Root side[2];                 // [0] port (+v), [1] starboard (-v)
        ParticleEmitterObject* sheet[2] = {};
        ParticleEmitterObject* mist[2] = {};
        float lastImpact = 0.0f;      // the slam the last frame showed: a burst on its rise
        float sheetAlpha[4] = {};     // the presets' alpha keys, as made (the opacity scales them every frame)
        float mistAlpha[4] = {};
        Math::float4 foamLine[2] = {}; // along its sides, outside the waterline (x0, z0, x1, z1)
        float foamRate = 0.0f;        // foam a second laid there
        bool seen = false;
    };
    static Root FindRoot(const buoyancy::HullFootprint& footprint, int side);
    bool Create(Scene& scene, Renderer* renderer, Set& set);

    std::vector<Set> sets_;
    float rate_ = 0.0f;
    float sheetRate_ = 0.0f;
    int bursts_ = 0;
    uint32_t sheetAlive_ = 0, sheetCapacity_ = 0, mistAlive_ = 0, mistCapacity_ = 0;
    uint64_t hullEntries_ = 0;
};
} // namespace vfx
