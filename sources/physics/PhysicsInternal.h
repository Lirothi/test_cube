#pragma once

// Jolt's headers, for sources/physics/*.cpp only: everything else talks to physics/PhysicsWorld.h. Those files are compiled with
// the library's defines and /arch:AVX2 (test_cube.vcxproj, third_party/jolt_defines.props).
#include "physics/PhysicsWorld.h" // PlayerSettings, PlayerInput: what PlayerBody takes

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

#include <functional>
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

    // The kind a request means for this motion, and the fallback where a kind cannot serve it. Auto on a body that does not move stays
    // Auto: the geometry's size decides (BuildShape).
    ShapeKind ResolveShapeKind(ShapeKind requested, bool dynamicBody);

    struct BuiltShape
    {
        JPH::RefConst<JPH::Shape> shape; // null = no body: nothing solid, or the build failed
        ShapeKind kind = ShapeKind::None; // what it became: Auto resolved, a hull that failed is a box
        std::uint32_t triangles = 0;     // what the geometry handed in
        std::uint32_t lodUsed = 0;
        std::uint32_t lodCount = 0;
        Math::float3 boundsMin;
        Math::float3 boundsMax;
        float smallestExtent = 0.0f;     // m, before the object's scale
        std::string error;               // empty when `shape` is null only because nothing is solid
    };
    BuiltShape BuildShape(const SolidGeometry& geometry, const CollisionSettings& settings, ShapeKind kind);

    // What Jolt does not have: a touching body loses speed at a constant rate, as to a torque that does not grow with its spin. Called after each step, on the velocities the
    // solver left. Only while something other than gravity moved it this step (`lastVelocityY` carries the vertical speed over): a spin in flight stays.
    void ApplyRollingResistance(JPH::BodyInterface& bodies, JPH::BodyID id, float resistance, float radius, float gravity, float dt, float& lastVelocityY);

    // ShapeKind::Auto: the rule. A mesh under this many metres at its largest takes a hull even when static: sticks, shells, candles and cups
    // carry thousands of triangles for nothing, and a hull is cheaper to touch. A mesh with leaves takes this LOD (its slots keep their
    // trunk; LOD 0 has 2 900-4 300 triangles of it for a palm and LOD 2 69-155).
    constexpr float kAutoHullBelow = 1.0f;
    constexpr int kAutoFoliageLod = 2;

    // The one road from an asset's settings to its shape: the world, the Mesh Editor's preview and the gates.
    BuiltShape BuildAssetShape(const std::string& geometryPath, const CollisionSettings& settings, bool dynamicBody);

    // JoltRuntime.cpp: what Jolt holds now, in bytes (mi_usable_size at each allocation and free).
    std::uint64_t JoltBytes();

    // The asset's shape under the object's scale. Non-uniform where the shape allows it, the nearest valid scale where not (and it says so).
    JPH::RefConst<JPH::Shape> ScaledInstance(const JPH::Shape& shape, const Math::float3& scale, std::string& error);

    // Custom primitives at a scale Jolt cannot apply to them: mirrored, or stretched. A box and a cylinder take it exactly along their own axes; turned against a stretch they stay upright
    // boxes round the nearest axes; a sphere takes the largest of the three, it has no other form.
    CollisionSettings BakeScale(const CollisionSettings& settings, const Math::float3& scale);

    // A body the water carries. Jolt's buoyancy drag is quadratic, so a small bob dies very slowly: 1 m of box at 1.1 was still moving
    // 0.4 m/s after 20 s on flat water with Jolt's defaults (gate `water`).
    constexpr float kFloatLinearDamping = 0.5f;
    constexpr float kFloatAngularDamping = 0.5f;

    // A kinematic body's mass acts on nothing (the solver reads it only for Dynamic), but Jolt asks the shape for one all the
    // same, and a MeshShape, what `Auto` gives a kinematic body, has none: 0, an assert in Debug and 1/0 in Release (gate `kinematic`).
    inline void GiveKinematicMass(JPH::BodyCreationSettings& creation)
    {
        creation.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        creation.mMassPropertiesOverride.mMass = 1.0f;
        creation.mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();
    }

    // PhysicsCharacter.cpp: a capsule on the ground, UE's walking mode. Not a body: collision queries only, pushing bodies by
    // impulses as it goes (Jolt's CharacterVirtual). `Feet` is its origin.
    // What the sea is doing under a boat, measured on the game's side, where the water can be read (MeasureVesselWater): the plane that fits the surface over the hull, which Jolt's buoyancy cuts the
    // hull with -- a wave shorter than the hull lifts it no more than a real one does --, and three stations along the hull for what one plane cannot say: a slope that differs at the bow and the
    // stern, a bow falling into a crest.
    struct VesselStation
    {
        float offset = 0.0f;      // m along the hull from the centre of mass: stern, middle, bow
        float height = 0.0f;      // the water's
        float speed = 0.0f;       // how fast it rises where the hull is, m/s, the hull's own way in it
        float slopeAlong = 0.0f;  // of the surface, rising toward the bow
        float slopeAcross = 0.0f; // rising toward starboard
    };
    struct VesselWater
    {
        bool valid = false;
        float height = 0.0f;      // of the plane, at the centre of mass
        float speed = 0.0f;       // m/s, as a station's
        JPH::Vec3 normal = JPH::Vec3::sAxisY();
        JPH::Vec3 spin = JPH::Vec3::sZero();     // how fast the plane turns, rad/s
        JPH::Vec3 forward = JPH::Vec3::sAxisX(); // the hull's, level
        JPH::Vec3 starboard = -JPH::Vec3::sAxisZ();
        float propellerHeight = 0.0f; // the water's, over the propeller: on a crest's back it comes out
        float calmAcross = 0.0f;  // the surface's rise toward starboard, slowly: what the form stability lies along. The chop passes under a hull faster than it can roll
        VesselStation stations[3];
    };
    // A boat (PhysicsVessel.cpp). Its shape: the hull with its weight low and over the water's lift, so it lies level at its draft; `floatFactor` is what Jolt's buoyancy takes for that. Null = it
    // cannot float (a triangle mesh has no volume, or the draft is under the hull), and `error` says why.
    JPH::RefConst<JPH::Shape> MakeVesselShape(const JPH::Shape& hull, const VesselSettings& settings, const Math::float3& scale, float& floatFactor, std::string& error);
    // What its body is made with, past the shape.
    void SetVesselBody(JPH::BodyCreationSettings& creation, const VesselSettings& settings);
    // `height` answers the water's height at (x, z); `elapsed` is the time since the last measure, 0 = a first look, no rates yet. `origin` is the mesh's, `centre` the centre of mass.
    void MeasureVesselWater(VesselWater& water, const std::function<float(float, float)>& height, JPH::RVec3Arg origin, JPH::RVec3Arg centre, JPH::QuatArg rotation,
        const VesselSettings& settings, const Math::float3& scale, float halfLength, float halfBeam, float elapsed);
    // One step on the water: its buoyancy, the helm, the hull and the waves.
    void PushVessel(JPH::Body& body, const VesselSettings& settings, const Math::float3& scale, VesselState& state, const VesselInput& input, const VesselWater& water, float buoyancy, JPH::Vec3Arg gravity, float dt);
    // The anchor's rode on the bow (`bowLocal`, the body's space), from `anchor` (world; its height not read): nothing within
    // anchorScope, past it the stretch's spring and the bow's run along it, never a push. One step's impulse.
    void PullAnchor(JPH::Body& body, const VesselSettings& settings, JPH::Vec3Arg bowLocal, JPH::Vec3Arg anchor, float dt);

    // A height no water reaches: no water here.
    inline constexpr float kNoWater = -1.0e30f;

    // The player's capsule and UE's character movement over it. Its origin is at the feet.
    class PlayerBody
    {
    public:
        PlayerBody(JPH::PhysicsSystem& system, JPH::Vec3Arg feet, const PlayerSettings& settings);
        ~PlayerBody();
        void Update(float dt, const PlayerInput& input, const PlayerSettings& settings, float waterHeight, JPH::TempAllocator& temp);
        JPH::Vec3 Feet() const;
        void SetFeet(JPH::Vec3Arg feet); // a teleport: the speed goes too
        JPH::Vec3 Eye(const PlayerSettings& settings) const;
        PlayerMode Mode() const;
        bool OnGround() const { return onGround_; }
        bool Crouched() const { return crouched_; }
        float HorizontalSpeed() const;

    private:
        JPH::PhysicsSystem& system_;
        JPH::Ref<JPH::CharacterVirtual> character_;
        JPH::RefConst<JPH::Shape> standing_;
        JPH::RefConst<JPH::Shape> crouching_;
        JPH::Vec3 horizontal_ = JPH::Vec3::sZero(); // along the ground, over whatever it stands on
        JPH::Vec3 lastGroundFlat_ = JPH::Vec3::sZero();
        float vertical_ = 0.0f;
        float sinceGround_ = 1.0f;
        float sinceJumpPress_ = 1.0f;
        float eyeFeetY_ = 0.0f;   // the feet as the eye follows them: a step up eases in, the capsule does not wait
        float crouchShare_ = 0.0f; // 0 standing, 1 crouched: the eye's
        bool onGround_ = false;
        bool crouched_ = false;
        bool swimming_ = false;
    };

    // The conversions. Numbers cross as they are, no change of handedness (docs/physics_plan.md 3.3, gate `pose`).
    inline JPH::Vec3 ToJolt(const Math::float3& v) { return JPH::Vec3(v.x, v.y, v.z); }
    inline Math::float3 FromJolt(JPH::Vec3Arg v) { return Math::float3(v.GetX(), v.GetY(), v.GetZ()); }
    inline JPH::Quat ToJolt(const Math::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
    inline Math::quat FromJolt(JPH::QuatArg q) { return Math::quat(q.GetX(), q.GetY(), q.GetZ(), q.GetW()); }
} // namespace physics
