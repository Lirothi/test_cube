#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "physics/PhysicsTypes.h"

class OceanRenderable;
class RenderableObject;
class RenderableObjectBase;

// The world's collision and rigid bodies, over Jolt (docs/physics_plan.md). This header holds no Jolt: the engine sees objects,
// points and normals. Everything here is called on the game thread; the render half takes the resulting matrix at the hand-over
// (Scene::Sync) through RenderableObject::SetRenderOffset.
//
// The step runs in the frame's idle window (physics.async): Tick counts the steps owed, KickStep starts them on a thread of their own once the
// render thread's frame is done, so they neither hold the game thread up nor share the cores with the frame's recording, and SyncPoses, at the
// hand-over, takes what they found. A step that is done by then gives the very pose the step in line gave; one that is not leaves the object
// where it stood for a frame, and the frame does not wait. Whatever reaches the world waits for the step first.
//
// What does not move does not pay: a body is created once, and without an active body or a moving boat PhysicsSystem::Update
// is not called at all. Objects arrive through the REGISTRY below, never a walk of the scene.
namespace physics
{
    // The objects: RenderableObject::SetPhysicsMotion registers, its destructor unregisters, its setters notify. Thread-safe
    // (a level load may build objects on workers): they only queue, and the world reads the object at its next Tick.
    void Register(RenderableObject* object);
    void Unregister(RenderableObject* object);
    void NotifyChanged(RenderableObjectBase* object);

    // --set / MCP physics.*. Not render knobs: only the game half reads them.
    struct Settings
    {
        bool enabled = true;     // physics.enabled: 0 = no world at all, the A/B
        bool simulate = true;    // physics.simulate: the shipped game runs; opening the editor turns it off (AppController)
        int debug = 0;           // physics.debug: 1 = the collision shapes as lines
        float debugRadius = 40.0f; // physics.debugRadius: triangles farther than this from the camera are not drawn
        float stepHz = 60.0f;    // physics.stepHz
        int maxSteps = 4;        // physics.maxSteps: a frame runs at most this many; the time past them is dropped
        int threads = 8;         // physics.threads: Jolt's pool, the calling thread helps on top. They sleep unless a step runs
        bool async = true;       // physics.async: 1 = the step on its own thread, the poses taken at the hand-over; 0 = in Scene::Tick, the A/B. Needs the render thread: on one thread there is no wait to run in
        bool kickInTick = false; // physics.kickInTick: 1 = the step starts in Tick, beside the render thread's frame, and not after it; the A/B
        float stepDelayMs = 0.0f; // physics.stepDelayMs: every step takes this much longer, to see what a heavy world does to a frame without building one
        float gravity = -9.81f;  // physics.gravity, m/s2 along y
        bool autoShapes = true;  // physics.autoShapes: 0 = every static asset's LOD 0 triangles, the A/B of ShapeKind::Auto's rule
        int forceLod = -1;       // physics.forceLod: every asset that does not name a LOD takes this one, but terrain. The plan's A/B
        // physics.hullTolerance: how far a mesh's point may stand outside its hull, as a share of the mesh's smallest size (1 mm at least, Jolt's own). A contact walks
        // the hull's faces, and at 1 mm a round thing takes Jolt's cap of 256 points: 0 = that.
        float hullTolerance = 0.04f; // 2000 coconuts landing: 508 faces at 1 mm 7.1 ms a step, 102 faces here 3.4 ms; 9 mm off a coconut, 92 % of its volume
        float killY = -200.0f;   // physics.killY: UE's KillZ, a body below this falls asleep and Keep leaves it
        float angularDamping = 1.0f; // physics.angularDamping, 1/s: a Dynamic body whose asset does not say
        float rollingResistance = 0.2f; // physics.rollingResistance: the sine of the slope a Dynamic body whose asset does not say still rests on (11.5 degrees)
        bool walk = false;       // physics.walk: the camera stands on a capsule on the ground
        float walkSpeed = 1.6f;  // physics.walkSpeed, m/s
        float walkSprint = 2.5f; // physics.walkSprint: Sprint's multiple
        float eyeHeight = 1.65f; // physics.eyeHeight, m over the feet
        float walkMaxDepth = 1.3f; // physics.walkMaxDepth: water deeper than this stops the walk, m
        float walkTestX = 0.0f;  // physics.walkTestX / Z, m/s: walks without a key, for a script (MCP has no keyboard)
        float walkTestZ = 0.0f;
    };
    Settings& GameSettings();

    struct RayHit
    {
        RenderableObject* object = nullptr;
        Math::float3 point;
        Math::float3 normal;
        float distance = 0.0f;
    };

    struct Stats
    {
        std::uint32_t objects = 0;      // registered
        std::uint32_t staticBodies = 0;
        std::uint32_t kinematicBodies = 0;
        std::uint32_t dynamicBodies = 0;
        std::uint32_t activeBodies = 0;
        std::uint32_t shapes = 0;       // unique, in the cache, with a body in them
        std::uint32_t meshShapes = 0;
        std::uint32_t hullShapes = 0;
        std::uint32_t primitiveShapes = 0; // box, sphere, capsule, custom
        std::uint64_t shapeTriangles = 0;  // of the mesh shapes
        std::uint64_t bytes = 0;           // what Jolt holds, the world and the shapes
        std::uint32_t stepsLastFrame = 0;
        std::uint64_t stepsTotal = 0;
        float stepMs = 0.0f;            // the last PhysicsSystem::Update
        float stepMsEma = 0.0f;
        float lastBuildMs = 0.0f;       // the last batch: shapes + bodies
        std::uint32_t floatingBodies = 0;
        std::uint32_t fellOut = 0;      // below physics.killY since the last Stop
        std::uint64_t handOversHeld = 0; // hand-overs that found the step still running: the frame showed the pose it had
        std::uint64_t stepWaits = 0;    // calls that had to wait for a step (a ray, the walk, an object added or gone, Keep)
        std::uint64_t stepErrors = 0;   // steps that ran out of room: contacts or pairs left out, bodies fall through
        bool simulating = false;
        bool worldExists = false;
        bool walking = false;
        bool walkOnGround = false;
    };

    // One simulated object's pose as authored values: what Keep writes.
    struct SimulatedPose
    {
        RenderableObject* object = nullptr;
        Math::float3 position;
        Math::float3 rotationEulerRad;
    };

    class World
    {
    public:
        World();
        ~World();
        World(const World&) = delete;
        World& operator=(const World&) = delete;

        // Once a frame from Scene::Tick: after the buoyancy (a boat's pose is this frame's), before the objects' PostTick.
        void Tick(float deltaTime);
        // Once a frame from the game loop, the moment the render thread's frame is done: starts the steps Tick counted.
        void KickStep();
        // Once a frame from Scene::Sync, before the objects' transforms cross: the poses of the step KickStep started, if it is done. Never waits.
        void SyncPoses();
        // Waits for the step in flight. What reaches the world through this class does so by itself; a test wants it to compare a frame.
        void FinishStep();
        // The end of a level load: the bodies of everything it built, in the load's profile instead of the first frame.
        void LoadFinished();
        // The eye for physics.debug and for the character.
        void SetCamera(const Math::float3& position) { camera_ = position; }
        // The water for the "buoyant" Dynamic bodies and the walk's depth. Null = none.
        void SetOcean(const OceanRenderable* ocean);
        // physics.walk: the input of this frame (m/s on the ground, a jump), and where the eye stands after this frame's Tick.
        // False = not walking, the camera is free.
        void SetWalkIntent(const Math::float3& velocity, bool jump);
        bool WalkEye(Math::float3& out) const;

        bool RayCast(const Math::float3& origin, const Math::float3& unitDirection, float maxDistance, RayHit& out) const;

        // The bodies that moved, as they stand (the editor's Keep).
        void CollectSimulatedPoses(std::vector<SimulatedPose>& out) const;

        Stats GetStats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        Math::float3 camera_;
    };

    // The shape an asset's settings give, without a world: for the Mesh Editor. `lines` are pairs of model-space points, thinned out
    // on a big mesh. `dynamicBody` is what Auto resolves against.
    struct CollisionPreview
    {
        std::vector<Math::float3> lines;
        std::uint32_t sourceTriangles = 0; // the solid triangles of the LOD
        std::uint32_t shapeTriangles = 0;  // what the shape answers with
        std::uint32_t lodUsed = 0;
        std::uint32_t lodCount = 0;
        ShapeKind kind = ShapeKind::None;
        float buildMs = 0.0f;
        std::string error;
    };
    void PreviewCollision(const std::string& geometryPath, const CollisionSettings& settings, bool dynamicBody, CollisionPreview& out);

    // The solid triangles' bounds, what the Mesh Editor's Fit measures.
    struct SolidBounds
    {
        Math::float3 min;
        Math::float3 max;
        bool valid = false;
    };
    SolidBounds MeasureSolid(const std::string& geometryPath, int lod, const std::vector<int>& excludeSlots);

    // The gates (docs/physics_plan.md section 5), no window and no scene: `name` empty = all of them. Each logs
    // `physics gate: <name> PASS|FAIL|SKIP ...`; returns the number that failed. --physics-gate and intent_regression run this.
    int RunGates(const std::string& name);
} // namespace physics
