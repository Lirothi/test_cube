#pragma once

// Jolt's headers, for sources/physics/*.cpp only: everything else talks to physics/PhysicsWorld.h. Those files are compiled with
// the library's defines and /arch:AVX2 (test_cube.vcxproj, third_party/jolt_defines.props).
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Renderer/DebugRendererSimple.h>

#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "physics/PhysicsTypes.h"

namespace physics
{
    // JoltRuntime.cpp: the allocator, Trace, AssertFailed, the factory and RegisterTypes. A process-wide reference count: the
    // first world brings it up and the last takes it down.
    void AcquireJolt();
    void ReleaseJolt();

    // The object layers: what cannot move, and everything that can. The broad phase has the same two.
    namespace layers
    {
        constexpr JPH::ObjectLayer kNonMoving = 0;
        constexpr JPH::ObjectLayer kMoving = 1;
        constexpr JPH::uint kCount = 2;
    }

    // The tables PhysicsSystem::Init keeps by reference, for as long as the system lives.
    struct LayerTables
    {
        LayerTables();
        JPH::BroadPhaseLayerInterfaceTable broadPhase;
        JPH::ObjectLayerPairFilterTable pairs;
        std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
    };

    // PhysicsShapes.cpp --------------------------------------------------------------------------------------------------

    // The solid triangles of a mesh on disk at a LOD, the excluded slots left out. Model space, the bake's metres.
    // Only the vertices the triangles use. False = unreadable.
    struct SolidGeometry
    {
        std::vector<Math::float3> positions;
        std::vector<std::uint32_t> indices;
        std::uint32_t lodUsed = 0;
        std::uint32_t lodCount = 0;
    };
    bool ReadSolidGeometry(const std::string& geometryPath, int lod, const std::vector<int>& excludeSlots, SolidGeometry& out);

    // The kind `Auto` means for this motion, and the fallback where a kind cannot serve it.
    ShapeKind ResolveShapeKind(ShapeKind requested, bool dynamicBody);

    struct BuiltShape
    {
        JPH::RefConst<JPH::Shape> shape; // null = no body: nothing solid, or the build failed
        std::uint32_t triangles = 0;     // what the geometry handed in
        Math::float3 boundsMin;
        Math::float3 boundsMax;
        float smallestExtent = 0.0f;     // m, before the object's scale
        std::string error;               // empty when `shape` is null only because nothing is solid
    };
    BuiltShape BuildShape(const SolidGeometry& geometry, const CollisionSettings& settings, ShapeKind kind);

    // The asset's shape under the object's scale. Non-uniform where the shape allows it, the nearest valid scale where not.
    JPH::RefConst<JPH::Shape> ScaledInstance(const JPH::Shape& shape, const Math::float3& scale, std::string& error);

    // A body the water carries. Jolt's buoyancy drag is quadratic, so a small bob dies very slowly: 1 m of box at 1.1 was still moving
    // 0.4 m/s after 20 s on flat water with Jolt's defaults (gate `water`).
    constexpr float kFloatLinearDamping = 0.5f;
    constexpr float kFloatAngularDamping = 0.5f;

    // PhysicsCharacter.cpp: a capsule on the ground, UE's walking mode. Not a body: collision queries only, pushing bodies by
    // impulses as it goes (Jolt's CharacterVirtual). `Feet` is its origin.
    class WalkingCharacter
    {
    public:
        WalkingCharacter(JPH::PhysicsSystem& system, JPH::Vec3Arg feet);
        ~WalkingCharacter();
        void Update(float dt, JPH::Vec3Arg desiredVelocity, bool jump, bool canPushBodies, JPH::TempAllocator& temp);
        JPH::Vec3 Feet() const;
        void SetFeet(JPH::Vec3Arg feet);
        bool OnGround() const;

    private:
        class Listener;
        JPH::PhysicsSystem& system_;
        std::unique_ptr<Listener> listener_;
        JPH::Ref<JPH::CharacterVirtual> character_;
    };

    // The conversions. Numbers cross as they are, no change of handedness (docs/physics_plan.md 3.3, gate `pose`).
    inline JPH::Vec3 ToJolt(const Math::float3& v) { return JPH::Vec3(v.x, v.y, v.z); }
    inline Math::float3 FromJolt(JPH::Vec3Arg v) { return Math::float3(v.GetX(), v.GetY(), v.GetZ()); }
    inline JPH::Quat ToJolt(const Math::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
    inline Math::quat FromJolt(JPH::QuatArg q) { return Math::quat(q.GetX(), q.GetY(), q.GetZ(), q.GetW()); }
} // namespace physics
