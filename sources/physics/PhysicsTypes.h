#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math/Math.h"

// What a level object and an asset say about physics (docs/physics_plan.md section 3.4). No Jolt here: this header reaches
// RenderableObject.h, and through it most of the engine. Jolt stays in sources/physics/*.cpp.
namespace physics
{
    // The object's "physics" key. Absent = Static: UE's BlockAll, the world collides with what it draws.
    // A "buoyant" object that is not Dynamic is a kinematic body: its pose is the buoyancy's.
    enum class Motion : std::uint8_t
    {
        None,    // no body
        Static,
        Dynamic,
    };

    enum class ShapeKind : std::uint8_t
    {
        Auto,    // Dynamic = Hull. Otherwise by the solid triangles' size: under 1 m Hull, above it Mesh (LOD 2 if the asset has leaves, else LOD 0)
        Mesh,    // triangles of the LOD; a Dynamic object cannot use it (Jolt: no mesh against mesh) and takes Hull
        Hull,
        Box,     // of the solid triangles' bounds
        Sphere,
        Capsule,
        Custom,  // CollisionSettings::primitives, UE's simple collision
        None,
    };

    // One hand-placed part of a Custom shape, in the mesh's model space (the bake's metres).
    enum class PrimitiveKind : std::uint8_t { Box, Sphere, Capsule, Cylinder };
    struct CollisionPrimitive
    {
        PrimitiveKind kind = PrimitiveKind::Box;
        Math::float3 center;
        Math::float3 rotationDeg;     // Math::mat4::RotationFromEulerXYZRad's order, as a level object's
        Math::float3 halfExtents = Math::float3(0.5f, 0.5f, 0.5f); // Box
        float radius = 0.5f;          // Sphere, Capsule, Cylinder
        float halfHeight = 0.5f;      // Along its own y. Capsule: the straight part, the caps on top; Cylinder: the whole of it
    };

    // mesh.json "vessel": the asset is a powered boat (physics/PhysicsVessel.cpp, docs/play_mode_plan.md). A placed copy is a Dynamic body on the water that a helm can steer. Points are in the mesh's
    // model space at scale 1. The numbers are a 10 m, 5.5 t runabout's, and every one of them is the asset's.
    struct VesselSettings
    {
        Math::float3 forward = Math::float3(1.0f, 0.0f, 0.0f); // the bow's way
        Math::float3 propeller = Math::float3(-4.6f, 0.1f, 0.0f); // where the thrust pushes and the rudder turns
        Math::float3 helmEye = Math::float3(-2.0f, 1.85f, -0.55f); // the helmsman's eyes
        float mass = 5500.0f;       // kg
        float centerOfMassY = 0.7f; // m over the mesh's origin: low, or it rolls over. Along and across it stands over the water's lift, so the boat lies level
        float draft = 0.38f;        // m over the mesh's origin: where still water stands on it. What sets Jolt's buoyancy factor
        float reserveLift = 2.0f;   // of the weight, the most its volume lifts: the collision hull is a convex lump with the cabin in it, 9 times the weight of the yacht, and a real hull ends at its deck. Past it a bow driven into a crest flies off
        float thrust = 22000.0f;    // N, full ahead
        float reverseShare = 0.4f;  // of the thrust, full astern
        float throttleRate = 1.0f;  // 1/s
        float rudderDeg = 30.0f;    // hard over
        float rudderRate = 70.0f;   // deg/s
        float rudderForce = 120.0f; // N across the stern per (m/s)2 of water past it, per sine of the rudder's angle
        float wash = 6.0f;          // m/s past the rudder from the propeller at full throttle: what steers a boat that hardly moves
        float surgeDrag = 110.0f;   // N per (m/s)2 along the hull
        float swayDrag = 1800.0f;   // N per (m/s)2 across it
        float swayLinear = 800.0f;  // N per m/s
        float swayLever = 1.2f;     // m aft of the centre of mass, where that force acts: the keel's. A hull then turns into its own drift and holds a course on waves that would slew it
        float yawDrag = 430000.0f;  // N m per (rad/s)2
        float yawLinear = 20000.0f; // N m per rad/s
        // The hull's grip across its way, as a tyre's mu N: the most the water holds it on a curve is this share of the weight
        // the water carries (x its share in the water), the net force across its way -- the hull's and the rudder's -- held to
        // it; past it the boat slides wide. On the plane less of the hull bites: down to sideGripPlane of it (2026-11-12, owner:
        // "the side grip falls with the speed" -- until then a boat's circle hardly changed with its speed, a held throttle took
        // every bend, and braking for one bought nothing). 0 = no limit (before).
        float sideGrip = 0.6f;      // of the weight (g), at displacement speeds
        float sideGripPlane = 0.45f; // of that, on the plane: 0.27 g, 2.65 m/s2 across its way flat out
        float heaveDamping = 3.0f;  // 1/s, against the water's own rise and fall
        float pitchDamping = 4.0f;  // 1/s, against the surface's own tilting: a long hull's ends push through the water. Under-damped it resonates with a sea met at speed and stands on its stern
        float rollDamping = 3.0f;   // 1/s
        float rollStiffness = 50000.0f; // N m per radian off the surface: the form stability of hard chines and a wide transom, which a convex lump of hull cut by a plane does not have
        float bank = 2.2f;          // degrees of lean into a turn per m/s2 of the turn
        float maxBank = 22.0f;      // degrees
        // Over the hump and onto the plane.
        float humpSpeed = 5.0f;     // m/s: the bow's highest and the drag's bump. A Froude number of 0.5 for a 10 m hull
        float planeSpeed = 10.0f;   // m/s: it is up
        float humpDrag = 6000.0f;   // N, on top of surgeDrag at the hump: too little throttle stays under it
        float humpTrim = 100000.0f; // N m of bow up at the hump
        float planeTrim = 40000.0f; // N m of bow up on the plane
        float planingLift = 0.3f;   // how much of its weight the speed carries on the plane, against what the volume carries: the hull rises, and has less of itself in the water to drag
        // The waves.
        float waveForce = 1.0f;     // the surface's slope pushes the hull down it (Froude-Krylov): 1 = physics, 0 = none. A swell surfs it, and a slope that differs at the bow and the stern yaws it
        float slam = 1500.0f;       // N per (m/s)2 of a part of the hull falling into the water faster than it rises
        float slamMax = 0.4f;       // of the weight, the most a part of the hull takes
        // The anchor (Play's: X at the helm drops it under the bow, again weighs it, Stop weighs it). The rode lies slack within
        // anchorScope of the anchor; past it it pulls the bow back, a spring with damping -- so the boat swings round the
        // anchor, bow to it, and full ahead stretches it thrust / stiffness further.
        float anchorScope = 8.0f;       // m of slack round the anchor
        float anchorStiffness = 8000.0f; // N per m past the scope
        float anchorDamping = 6000.0f;  // N per m/s of the bow running out along the rode
        // The spray's size against small_yacht's, whose hull the presets were made for (vfx/VesselSpray): the drops' and
        // the mist's sprites times it, their throw times its root (a model's heights go with its size). A 6.7 m runabout
        // under 10 m sheets was a boat in a cloud.
        float sprayScale = 1.0f;
        float cameraDistance = 15.0f; // m
        float cameraHeight = 2.5f;  // m over the mesh's origin
        // m/s: faster than this the hull leaves the GI probes' rays. They stand in the world, and it flies through them: a probe is inside it, out of it and behind it from one update to the next, and
        // the cockpit's bounce jumped 25-37 % a frame at 16 m/s. 0 = never leaves. Below it the hull lights itself as anything else does.
        float giProbeSpeed = 2.0f;
    };

    // What the helm says this frame: -1 full astern .. 1 full ahead, -1 hard to port .. 1 hard to starboard.
    struct VesselInput
    {
        float throttle = 0.0f;
        float rudder = 0.0f;
    };

    // Where the helm and the hull have got to.
    struct VesselState
    {
        float throttle = 0.0f;
        float rudder = 0.0f;
        float speed = 0.0f;     // m/s along the hull
        float immersion = 0.0f; // 1 = as deep as it floats at rest
        float bite = 0.0f;      // the propeller's share in the water: 0 = out of it, the engine races
        float impact = 0.0f;    // the last slam, as a share of the weight, fading
        float waterHeight = 0.0f; // of the surface under it
        float buoyancyLead = 0.0f; // m the centre of buoyancy stands ahead of the centre of mass along the hull: aft of it (< 0) the water lifts the stern, the bow comes down
        float trimTorque = 0.0f;   // N m of bow up the hump and the plane ask for this step
    };

    // mesh.json / level object "collision" block (physics/CollisionJson.h). Every field has an automatic value, so an asset
    // nobody wrote anything for still collides. It reaches the object through ResolveMeshAsset, as "buoyancy" does.
    struct CollisionSettings
    {
        ShapeKind shape = ShapeKind::Auto;
        int lod = -1; // < 0 = the rule in ShapeKind::Auto
        // Material slots left out. Absent in the JSON = the slots with "windFoliage" > 0 (a leaf does not collide): the factory
        // resolves that, so what reaches the object is the final list.
        std::vector<int> excludeSlots;
        float density = 1000.0f; // kg/m3, Dynamic only
        float mass = 0.0f;       // kg; > 0 wins over the density
        float friction = 0.5f;
        float restitution = 0.1f;
        float convexRadius = -1.0f; // m; < 0 = automatic (Jolt's default, less on a thin object)
        float floatFactor = 1.1f;   // Jolt's buoyancy: 1 = neutral, < 1 sinks, > 1 floats; a "buoyant" Dynamic object only
        float angularDamping = -1.0f; // 1/s; < 0 = physics.angularDamping
        float rollingResistance = -1.0f; // the sine of the slope it still rests on; < 0 = physics.rollingResistance. Jolt has none (ApplyRollingResistance)
        // A triangle collides on its front only (Jolt ignores back faces in the simulation). A cloth sheet walked into from
        // behind wants both sides: its triangles are added again the other way round.
        bool twoSided = false;
        std::vector<CollisionPrimitive> primitives; // ShapeKind::Custom
    };

    inline const char* MotionName(Motion motion)
    {
        switch (motion)
        {
        case Motion::None: return "none";
        case Motion::Dynamic: return "dynamic";
        case Motion::Static: break;
        }
        return "static";
    }

    inline bool MotionFromName(const std::string& name, Motion& out)
    {
        if (name == "none") { out = Motion::None; return true; }
        if (name == "static") { out = Motion::Static; return true; }
        if (name == "dynamic") { out = Motion::Dynamic; return true; }
        return false;
    }

    inline const char* ShapeKindName(ShapeKind kind)
    {
        switch (kind)
        {
        case ShapeKind::Mesh: return "mesh";
        case ShapeKind::Hull: return "hull";
        case ShapeKind::Box: return "box";
        case ShapeKind::Sphere: return "sphere";
        case ShapeKind::Capsule: return "capsule";
        case ShapeKind::Custom: return "custom";
        case ShapeKind::None: return "none";
        case ShapeKind::Auto: break;
        }
        return "auto";
    }

    inline bool ShapeKindFromName(const std::string& name, ShapeKind& out)
    {
        for (const ShapeKind kind : { ShapeKind::Auto, ShapeKind::Mesh, ShapeKind::Hull, ShapeKind::Box, ShapeKind::Sphere,
                 ShapeKind::Capsule, ShapeKind::Custom, ShapeKind::None })
        {
            if (name == ShapeKindName(kind)) { out = kind; return true; }
        }
        return false;
    }
} // namespace physics
