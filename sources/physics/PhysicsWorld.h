#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "physics/PhysicsTypes.h"

class DebugDrawSystem;
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
namespace JPH { class PhysicsSystem; }

namespace physics
{
    class WaterSurface; // physics/WaterSurface.h
    class StepBody;     // physics/PhysicsBody.h

    // A body as game code names it to the fixed step (R14.2): its id, taken on the game thread (World::BodyOf). kNoBody = none.
    using BodyKey = std::uint32_t;
    inline constexpr BodyKey kNoBody = 0xFFFFFFFFu;

    // One fixed step as a game system's FixedTick sees it (World::AddFixedTick): its length, gravity, and the bodies one at a time.
    class FixedStep
    {
    public:
        FixedStep(JPH::PhysicsSystem& system, float dt, const Math::float3& gravity) : system_(&system), dt_(dt), gravity_(gravity) {}
        float Dt() const { return dt_; }
        const Math::float3& Gravity() const { return gravity_; }
        // `use` with the body, woken and under its lock. False = no such body any more (its object gone), or one that cannot
        // move (static): `use` is not called.
        bool With(BodyKey key, const std::function<void(StepBody&)>& use) const;

    private:
        JPH::PhysicsSystem* system_;
        float dt_;
        Math::float3 gravity_;
    };

    // The objects: RenderableObject::SetPhysicsMotion registers, its destructor unregisters, its setters notify. Thread-safe
    // (a level load may build objects on workers): they only queue, and the world reads the object at its next Tick.
    void Register(RenderableObject* object);
    void Unregister(RenderableObject* object);
    void NotifyChanged(RenderableObjectBase* object);

    // The player's body and how it moves: UE's UCharacterMovementComponent, its formulas and numbers in metres (docs/play_mode_plan.md). --set / MCP player.*, a level's
    // Player Start may say its own (app/game/PlayerStart.h). height, crouchHeight and radius are read when the body is made, the rest every frame.
    struct PlayerSettings
    {
        float walkSpeed = 4.0f;       // m/s
        float sprintSpeed = 6.4f;     // m/s, Shift
        float crouchSpeed = 2.0f;     // m/s, UE's half of the walk
        float swimSpeed = 2.0f;       // m/s
        float acceleration = 20.48f;  // m/s2, UE's MaxAcceleration 2048 cm/s2
        float braking = 20.48f;       // m/s2, UE's BrakingDecelerationWalking
        float friction = 8.0f;        // 1/s, UE's GroundFriction
        float airControl = 0.35f;     // the share of the acceleration in the air: UE's first person template; its component's default is 0.05
        float jumpSpeed = 4.2f;       // m/s, UE's JumpZVelocity 420 cm/s: 0.9 m up
        float height = 1.8f;          // m, the capsule
        float crouchHeight = 1.1f;    // m
        float radius = 0.3f;          // m
        float eyeHeight = 1.65f;      // m over the feet
        float crouchEyeHeight = 0.95f; // m over the feet
        float stepHeight = 0.4f;      // m: a ledge this high is walked up
        float maxSlopeDeg = 50.0f;    // a slope steeper than this is a wall
        float swimDepth = 1.3f;       // water deeper than this over the feet floats the body: the eye just over the surface, no more walking on the bottom
        // A script's keys (--set player.test*) are not here: they go through InputManager's channel (architecture plan
        // R12.3), and the player's input comes in by PlayerSystem::SetInput (app/scene) as the keys' does.
    };

    // What the keys say this frame.
    struct PlayerInput
    {
        Math::float3 move;   // on the ground, in the world's axes, length up to 1
        bool jump = false;   // pressed this frame
        bool crouch = false; // held
        bool sprint = false; // held
    };

    enum class PlayerMode : std::uint8_t { Walking, Falling, Swimming };
    const char* PlayerModeName(PlayerMode mode);

    // A walking capsule as game code names it (World::AddCharacter, R14.6). kNoCharacter = none.
    using CharacterKey = std::uint32_t;
    inline constexpr CharacterKey kNoCharacter = 0;
    // A height no water reaches: no water here (World::MoveCharacter).
    inline constexpr float kNoWater = -1.0e30f;
    // Where a capsule stands and how it goes (World::GetCharacter).
    struct CharacterState
    {
        Math::float3 feet;        // its origin
        Math::float3 eye;         // the settings' eye over the feet, as it eases up a step and down to a crouch
        PlayerMode mode = PlayerMode::Falling;
        bool onGround = false;
        bool crouched = false;
        float speed = 0.0f;       // m/s, along the ground
    };

    // The boat at the helm as the frame shows it (VesselSystem::GetHelm, app/scene).
    struct HelmState
    {
        bool exists = false;
        RenderableObject* object = nullptr;
        const VesselSettings* settings = nullptr;
        Math::float3 position;
        Math::quat rotation;
        Math::float3 forward;
        Math::float3 scale;
        float starboardY = 0.0f; // how far its starboard side has risen
        float upY = 1.0f;        // of its up: with starboardY, its roll whatever its pitch
        float halfLength = 0.0f;
        float halfBeam = 0.0f;
        bool anchored = false;   // its anchor is down (VesselSystem::ToggleAnchor)
        VesselState vessel;
    };

    // A boat as the frame draws it (VesselSystem::CollectHulls): what its wake on the water starts from.
    struct VesselHull
    {
        RenderableObject* object = nullptr;
        const VesselSettings* settings = nullptr;
        Math::float3 position;  // the mesh's origin
        Math::float3 forward;   // the bow's way, world, unit
        Math::float3 up;        // the deck's up, world, unit
        Math::float3 scale;
        float speed = 0.0f;     // m/s along the hull, as the frame shows it
        Math::float3 velocity;        // the origin's, m/s, world: the last step's motion (a turn's sway and drift in it)
        Math::float3 angularVelocity; // rad/s, world: the last step's turn (a point r off the origin moves at v + w x r)
        float impact = 0.0f;    // its last slam into a wave, a share of its weight, fading (VesselState::impact)
        float immersion = 0.0f; // how deep it sits, 1 = as deep as at rest, 0 = in the air (VesselState::immersion)
        float waterHeight = 0.0f; // the surface under it (VesselState::waterHeight)
        bool atHelm = false;
        bool anchored = false;
        bool piloted = false;     // a pilot other than the helm orders it (VesselSystem::SetPilot)
        float halfLength = 0.0f;  // of the hull's box along its way, m
        float halfBeam = 0.0f;
        float rudderLever = 0.0f; // m the propeller -- where the rudder pushes -- lies aft of the centre of mass
        float throttle = 0.0f;    // where its engine and rudder have got to (VesselState)
        float rudder = 0.0f;
    };

    // The player as the game reads it (app/scene/PlayerSystem::State).
    struct PlayerState
    {
        bool exists = false;
        bool possessed = false;   // false: F8, the body stands and the camera is free
        bool onGround = false;
        bool crouched = false;
        PlayerMode mode = PlayerMode::Falling;
        float speed = 0.0f;       // m/s, along the ground
        Math::float3 feet;
    };

    // --set / MCP physics.*. Not render knobs: only the game half reads them.
    struct Settings
    {
        bool enabled = true;     // physics.enabled: 0 = no world at all, the A/B
        bool simulate = true;    // The world runs. NOT a knob: the play session says, every frame (AppController); the editor's world stands, a build without the editor always runs
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
        // A boat nobody steers -- no helm, no bot, its own anchor up -- stands on a hard mooring once it has slowed to
        // mooringSettle m/s: its place along the water and its heading held (HoldMooring), the waves' heave, pitch and roll its
        // own. Owner, 2026-11-13: "when we board one, the others must stand on a hard anchor".
        float mooring = 2.5f;      // physics.mooring, rad/s: the hold's own frequency along the water; 0 = none, a boat drifts (before)
        float mooringTurn = 2.0f;  // physics.mooringTurn, rad/s: and about the vertical
        float mooringSettle = 0.6f; // physics.mooringSettle, m/s
        PlayerSettings player;   // player.*
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
    };

    // One simulated object's pose as authored values: what Keep writes.
    struct SimulatedPose
    {
        RenderableObject* object = nullptr;
        Math::float3 position;
        Math::float3 rotationEulerRad;
    };

    // A boat's body as the physics made it (R14.3b): what app/scene/VesselSystem runs it by.
    struct VesselBody
    {
        RenderableObject* object = nullptr;
        BodyKey key = kNoBody;
        VesselSettings asset;     // mesh.json "vessel", as the body was made by it
        Math::float3 scale;
        float floatFactor = 1.0f; // what Jolt's buoyancy takes for its shape (MakeVesselShape)
        float halfLength = 0.0f;  // of the hull's box along its way, m
        float halfBeam = 0.0f;
        float rudderLever = 0.0f; // m the propeller lies aft of the centre of mass, along the hull
        Math::float3 bowLocal;    // the stem of the hull's box at the draft, in the body's space
    };
    // Where the frame shows a body (World::ShowBody).
    struct ShownBody
    {
        Math::float3 position;
        Math::quat rotation;
        Math::float3 forward; // the asked local forward, turned and normalised
        Math::float3 up;
        Math::float3 velocity; // the last step's motion of the origin, m/s
        Math::float3 angular;  // and its turn, rad/s
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
        // Objects that left the scene just now (a race's spawned boats, Scene::Sync): their entries go at once, after any step in
        // flight -- before VesselSystem::CollectHulls or anything else hands out a pointer to them.
        void ForgetRemoved();
        // Once a frame from Scene::Sync, before the objects' transforms cross: the poses of the step KickStep started, if it is done. Never waits.
        void SyncPoses();
        // Waits for the step in flight. What reaches the world through this class does so by itself; a test wants it to compare a frame.
        void FinishStep();
        // The end of a level load: the bodies of everything it built, in the load's profile instead of the first frame.
        void LoadFinished();
        // The eye for physics.debug and for the character.
        void SetCamera(const Math::float3& position) { camera_ = position; }
        void SetDebugDraw(DebugDrawSystem* draw) { debugDraw_ = draw; } // physics.debug lines go here
        // The water for the "buoyant" Dynamic bodies, the boats and the player's swim (R14.1: the physics names no ocean). Null = none.
        // Read in Tick only; the caller keeps it alive till the next SetWater.
        void SetWater(const WaterSurface* water);
        // R14.6: walking capsules -- UE's character movement over Jolt's CharacterVirtual (PhysicsCharacter.cpp). Not bodies:
        // collision queries, and bodies pushed by impulses as one goes. Game code moves one once a frame on the frame's clock
        // (StepHooks::frame); the player -- where it comes in, its keys, physics.killY, its eye -- is app/scene/PlayerSystem's.
        // A world gone (physics.enabled:0) takes them with it: a key then names nothing. Game thread.
        // A capsule standing at `feet` (its height, crouchHeight and radius are read here). kNoCharacter: no world. Waits for a
        // step in flight.
        CharacterKey AddCharacter(const Math::float3& feet, const PlayerSettings& settings);
        void RemoveCharacter(CharacterKey key);
        bool HasCharacter(CharacterKey key) const;
        // One frame of its movement: `dt` as given, the keys, the tuning, the water's height under its feet (kNoWater: none).
        // Waits for a step in flight: its queries and pushes must not meet one. False = no such capsule.
        bool MoveCharacter(CharacterKey key, float dt, const PlayerInput& input, const PlayerSettings& settings, float waterHeight);
        // A teleport: the speed goes too. Waits for a step in flight.
        bool PlaceCharacter(CharacterKey key, const Math::float3& feet);
        // Where it stands and how it goes; `settings` for its eye. False = no such capsule.
        bool GetCharacter(CharacterKey key, const PlayerSettings& settings, CharacterState& out) const;
        // The boats -- the helm, the pilots, the anchor, the mooring, where each is shown -- are app/scene/VesselSystem's (R14.3b),
        // over the bodies, queries and hooks below.

        bool RayCast(const Math::float3& origin, const Math::float3& unitDirection, float maxDistance, RayHit& out) const;

        // R14.2 / R14.3b / R14.6: game code around the steps (app/scene/VesselSystem: the boats; app/scene/PlayerSystem: the
        // player). Every hook may be empty.
        struct StepHooks
        {
            // Game thread, once a frame in Tick on the frame's own clock, before the steps are counted and whether or not the
            // world simulates: the frame's time and the water (null = none). Not while there is no world (physics.enabled:0).
            // The player's (R14.6): it is the camera, and a camera moves every frame -- under `fixed`, at 300 frames a second
            // its eye would stand four frames of five. A step still in flight runs on: what reaches the world waits for it.
            std::function<void(float deltaTime, const WaterSurface* water)> frame;
            // Game thread, where the steps owed are counted and no step runs: the water (null = none), and the time the steps cover.
            std::function<void(float elapsed, const WaterSurface* water)> measure;
            // Game thread, as the steps start (KickStep, or in line): what the game ordered, taken for them.
            std::function<void()> start;
            // Before every step, on the step's thread (the game's when the world steps in line), with that step. It reaches
            // bodies only through the FixedStep, and never an object (one may be gone by then), nor anything the game thread
            // writes meanwhile.
            std::function<void(const FixedStep&)> fixed;
            // Game thread, where the frame's poses are shown (the hand-over, or the end of an in-line Tick): `alpha`, where the
            // frame stands between the last two steps.
            std::function<void(float alpha)> shown;
            // Game thread, after Stop put every body back where the level has it.
            std::function<void()> stopped;
        };
        // Waits for a step in flight. The id is RemoveStepHooks'.
        int AddStepHooks(StepHooks hooks);
        void RemoveStepHooks(int id);
        // A FixedTick alone (AddStepHooks with `fixed` only): a gate's. The id is RemoveFixedTick's.
        int AddFixedTick(std::function<void(const FixedStep&)> tick);
        void RemoveFixedTick(int id);
        // The object's body as a FixedTick names it; kNoBody until the world built one (the Tick after it was registered).
        BodyKey BodyOf(const RenderableObject* object) const;
        // The boats' bodies (mesh.json "vessel"), and a number that moves whenever they change: what VesselSystem runs.
        std::uint64_t VesselsRevision() const;
        void CollectVesselBodies(std::vector<VesselBody>& out) const;
        // A body's pose: its origin, centre of mass and rotation. Waits for a step in flight. False = no such body, or one
        // fallen out of the world.
        bool BodyPose(BodyKey key, Math::float3& origin, Math::float3& centre, Math::quat& rotation) const;
        // A point in the body's own space, in the world's. Waits for a step in flight.
        bool BodyToWorld(BodyKey key, const Math::float3& local, Math::float3& out) const;
        // Where the frame shows a body between the last two steps (`alpha`), its `localForward` in the world, and the last
        // step's motion -- the physics' own sums. Game thread with no step in flight (a hook, or after FinishStep).
        bool ShowBody(BodyKey key, float alpha, const Math::float3& localForward, ShownBody& out) const;
        // Puts a body somewhere at rest, woken: no speed, both last poses there (a race's grid). Waits for a step in flight.
        // The session's: Stop puts it back where the level has it.
        bool PlaceBody(BodyKey key, const Math::float3& position, const Math::quat& rotation);

        // The bodies that moved, as they stand (the editor's Keep).
        void CollectSimulatedPoses(std::vector<SimulatedPose>& out) const;

        Stats GetStats() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        Math::float3 camera_;
        DebugDrawSystem* debugDraw_ = nullptr;
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

    // A box turned about the vertical to lie along the mesh: the smallest rectangle round it on the ground. A door standing at an angle in its own space gets a box along its leaf, where the
    // axes' bounds are a block round it. A mesh along its axes keeps them exactly. The yaw is as a level's or a custom primitive's rotationDeg.y, in (-45, 45]. What Shape: Box and Fit box use.
    struct SolidBox
    {
        Math::float3 centre;
        Math::float3 half;
        float yawDeg = 0.0f;
        bool valid = false;
    };
    SolidBox FitSolidBox(const std::vector<Math::float3>& points);
    SolidBox MeasureSolidBox(const std::string& geometryPath, int lod, const std::vector<int>& excludeSlots);

    // A mesh asset's LOD 0 triangles (every slot) in the model's space, only the vertices they use. The water mask's columns
    // (ocean/WaterMask). False if the geometry cannot be read.
    bool ReadSolidTriangles(const std::string& geometryPath, std::vector<Math::float3>& positions, std::vector<std::uint32_t>& indices);
    // Both from one read of the file: every slot's triangles, and the listed material slots' alone (the water mask's lid: the
    // glass that closes a basin; empty slots = none).
    bool ReadSolidAndSlotTriangles(const std::string& geometryPath, const std::vector<int>& slots, std::vector<Math::float3>& positions,
                                   std::vector<std::uint32_t>& indices, std::vector<Math::float3>& slotPositions,
                                   std::vector<std::uint32_t>& slotIndices);

    // A gate of a layer above the physics (R14.3a: the boats', app/scene/VesselGates), run by RunGates beside the physics' own:
    // `run` fills `detail` and answers 0 = PASS, 1 = FAIL, 2 = SKIP.
    struct ExtraGate
    {
        const char* name;
        std::function<int(std::string& detail)> run;
    };
    // The gates (docs/physics_plan.md section 5), no window and no scene: `name` empty = all of them, the physics' and then
    // `extra`. Each logs `physics gate: <name> PASS|FAIL|SKIP ...`; returns the number that failed. --physics-gate and
    // intent_regression run this.
    int RunGates(const std::string& name, const std::vector<ExtraGate>& extra = {});
} // namespace physics
