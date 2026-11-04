#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
        Auto,    // Static / kinematic = Mesh, Dynamic = Hull
        Mesh,    // triangles of the LOD; a Dynamic object cannot use it (Jolt: no mesh against mesh) and takes Hull
        Hull,
        Box,     // of the solid triangles' bounds
        Sphere,
        Capsule,
        None,
    };

    // mesh.json / level object "collision" block (physics/CollisionJson.h). Every field has an automatic value, so an asset
    // nobody wrote anything for still collides. It reaches the object through ResolveMeshAsset, as "buoyancy" does.
    struct CollisionSettings
    {
        ShapeKind shape = ShapeKind::Auto;
        int lod = 0;
        // Material slots left out. Absent in the JSON = the slots with "windFoliage" > 0 (a leaf does not collide): the factory
        // resolves that, so what reaches the object is the final list.
        std::vector<int> excludeSlots;
        float density = 1000.0f; // kg/m3, Dynamic only
        float mass = 0.0f;       // kg; > 0 wins over the density
        float friction = 0.5f;
        float restitution = 0.1f;
        float convexRadius = -1.0f; // m; < 0 = automatic (Jolt's default, less on a thin object)
        float floatFactor = 1.1f;   // Jolt's buoyancy: 1 = neutral, < 1 sinks, > 1 floats; a "buoyant" Dynamic object only
        float angularDamping = -1.0f; // 1/s; < 0 = physics.angularDamping. Stands in for rolling resistance, which Jolt does not have
        // A triangle collides on its front only (Jolt ignores back faces in the simulation). A cloth sheet walked into from
        // behind wants both sides: its triangles are added again the other way round.
        bool twoSided = false;
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
        case ShapeKind::None: return "none";
        case ShapeKind::Auto: break;
        }
        return "auto";
    }

    inline bool ShapeKindFromName(const std::string& name, ShapeKind& out)
    {
        for (const ShapeKind kind : { ShapeKind::Auto, ShapeKind::Mesh, ShapeKind::Hull, ShapeKind::Box, ShapeKind::Sphere,
                 ShapeKind::Capsule, ShapeKind::None })
        {
            if (name == ShapeKindName(kind)) { out = kind; return true; }
        }
        return false;
    }
} // namespace physics
