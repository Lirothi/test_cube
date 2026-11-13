#include "physics/PhysicsInternal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/TransformedShape.h>

#include "app/Systems.h"
#include "core/diagnostics/BootProfile.h"
#include "core/logging/Log.h"
#include "core/memory/MemScope.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/task/TaskSystem.h"
#include "core/threading/RenderThread.h"
#include "core/threading/ThreadRole.h"
#include "ocean/OceanReadback.h"
#include "ocean/OceanRenderable.h"
#include "physics/CollisionJson.h"
#include "physics/PhysicsWorld.h"
#include "rendering/core/Renderer.h"
#include "rendering/debug/DebugDraw.h"
#include "rendering/renderables/RenderableObject.h"

namespace physics
{
    namespace
    {
        // ---- The registry ---------------------------------------------------------------------------------------------
        // Only a queue: an object is read at the world's next Tick, on the game thread, where its transform is the game's.
        enum class EventKind : std::uint8_t { Added, Removed, Changed };
        struct Event
        {
            EventKind kind;
            RenderableObjectBase* object; // a key, never dereferenced here: the object may be gone by the time this is read
            RenderableObject* renderable; // Added only
        };
        struct Registry
        {
            std::mutex mutex;
            std::vector<Event> events;
        };
        Registry& TheRegistry()
        {
            static Registry* registry = new Registry(); // immortal: objects outlive statics at exit
            return *registry;
        }
        void Post(EventKind kind, RenderableObjectBase* object, RenderableObject* renderable = nullptr)
        {
            Registry& registry = TheRegistry();
            std::lock_guard<std::mutex> lock(registry.mutex);
            registry.events.push_back(Event{ kind, object, renderable });
        }

        // The rigid part of a model matrix as Jolt's position and rotation.
        struct Pose
        {
            JPH::Vec3 position;
            JPH::Quat rotation;
        };

        Math::quat QuatFromMatrix(const Math::mat4& m)
        {
            return Math::quat::FromXM(DirectX::XMQuaternionRotationMatrix(m.xm())).Normalized();
        }

        Pose AuthoredPose(const RenderableObject& object)
        {
            return Pose{ ToJolt(object.GetPosition()), ToJolt(QuatFromMatrix(object.GetOrientationMatrix())) };
        }

        Math::mat4 RigidMatrix(JPH::Vec3Arg position, JPH::QuatArg rotation)
        {
            return Math::mat4::FromQuaternion(FromJolt(rotation)) * Math::mat4::Translation(FromJolt(position));
        }

        std::string ShapeKeyOf(const std::string& geometry, const std::string& geometryStamp, const CollisionSettings& s, ShapeKind kind)
        {
            std::string key = geometry;
            key += '|';
            key += geometryStamp;
            key += '|';
            key += ShapeKindName(kind);
            key += '|';
            key += std::to_string(s.lod);
            // The rule's knobs: a change of either is a new shape (Impl::Drain rebuilds).
            key += GameSettings().autoShapes ? "|a" : "|m";
            key += std::to_string(GameSettings().forceLod);
            key += 'h';
            key += std::to_string(GameSettings().hullTolerance);
            for (const CollisionPrimitive& p : s.primitives)
            {
                key += "|p" + std::to_string(static_cast<int>(p.kind));
                for (const float v : { p.center.x, p.center.y, p.center.z, p.rotationDeg.x, p.rotationDeg.y, p.rotationDeg.z, p.halfExtents.x, p.halfExtents.y, p.halfExtents.z, p.radius, p.halfHeight })
                {
                    key += ',';
                    key += std::to_string(v);
                }
            }
            for (const int slot : s.excludeSlots)
            {
                key += ',';
                key += std::to_string(slot);
            }
            if (kind != ShapeKind::Mesh)
            {
                key += "|d";
                key += std::to_string(s.density);
                key += "|r";
                key += std::to_string(s.convexRadius);
            }
            if (s.twoSided) { key += "|2"; }
            return key;
        }

        // The collision shapes as lines, through the same road as the editor's zones. Only what lies near the camera: the
        // island alone is 83 thousand triangles.
        class LineRenderer final : public JPH::DebugRendererSimple
        {
        public:
            void Begin(DebugDrawSystem* lines, const Math::float3& eye, float radius)
            {
                lines_ = lines;
                eye_ = ToJolt(eye);
                radiusSq_ = radius * radius;
                SetCameraPos(eye_);
            }
            void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override
            {
                if (!lines_) { return; }
                lines_->AddLine(FromJolt(from), FromJolt(to), Math::float4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f));
            }
            void DrawTriangle(JPH::RVec3Arg a, JPH::RVec3Arg b, JPH::RVec3Arg c, JPH::ColorArg color, ECastShadow) override
            {
                if ((a - eye_).LengthSq() > radiusSq_ && (b - eye_).LengthSq() > radiusSq_ && (c - eye_).LengthSq() > radiusSq_) { return; }
                DrawLine(a, b, color);
                DrawLine(b, c, color);
                DrawLine(c, a, color);
            }
            void DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) override {}

        private:
            DebugDrawSystem* lines_ = nullptr;
            JPH::Vec3 eye_ = JPH::Vec3::sZero();
            float radiusSq_ = 0.0f;
        };

        class NearCameraFilter final : public JPH::BodyDrawFilter
        {
        public:
            NearCameraFilter(const Math::float3& eye, float radius) : eye_(ToJolt(eye)), radiusSq_(radius * radius) {}
            bool ShouldDraw(const JPH::Body& body) const override
            {
                return body.GetWorldSpaceBounds().GetSqDistanceTo(eye_) <= radiusSq_;
            }

        private:
            JPH::Vec3 eye_;
            float radiusSq_;
        };

        // The thread PhysicsSystem::Update is called on (physics.async). Its own, not a worker of the task system: a step blocks its caller on Jolt's barrier, and a render task
        // must not queue behind that. Jolt's pool still does the work; this one helps it, as the game thread did.
        class StepThread
        {
        public:
            ~StepThread() { Stop(); }

            // Game thread, nothing in flight.
            void Kick(std::function<void()> work)
            {
                if (!thread_.joinable())
                {
                    stop_ = false;
                    thread_ = std::thread(&StepThread::Main, this);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    work_ = std::move(work);
                    done_.store(false, std::memory_order_release);
                }
                wake_.notify_one();
            }
            bool Done() const { return done_.load(std::memory_order_acquire); }
            void Wait()
            {
                std::unique_lock<std::mutex> lock(mutex_);
                finished_.wait(lock, [this]() { return done_.load(std::memory_order_relaxed); });
            }
            void Stop()
            {
                if (!thread_.joinable()) { return; }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stop_ = true;
                }
                wake_.notify_one();
                thread_.join();
            }

        private:
            void Main()
            {
                logging::SetCurrentThreadName("Physics");
                Profiler::Get().SetThreadName("Physics Step"); // its row in a --trace
                Profiler::MarkThreadCrossFrame();
                std::unique_lock<std::mutex> lock(mutex_);
                for (;;)
                {
                    wake_.wait(lock, [this]() { return stop_ || work_ != nullptr; });
                    if (work_ == nullptr) { return; } // stopping, and nothing was handed over
                    std::function<void()> work = std::move(work_);
                    work_ = nullptr;
                    lock.unlock();
                    work();
                    lock.lock();
                    done_.store(true, std::memory_order_release);
                    finished_.notify_all();
                }
            }

            std::thread thread_;
            std::mutex mutex_;
            std::condition_variable wake_;
            std::condition_variable finished_;
            std::function<void()> work_;
            std::atomic<bool> done_{ true };
            bool stop_ = false;
        };
    }

    Settings& GameSettings()
    {
        static Settings settings;
        return settings;
    }

    void Register(RenderableObject* object) { Post(EventKind::Added, object, object); }
    void Unregister(RenderableObject* object) { Post(EventKind::Removed, object); }
    void NotifyChanged(RenderableObjectBase* object) { Post(EventKind::Changed, object); }

    struct World::Impl
    {
        struct Entry
        {
            RenderableObject* object = nullptr;
            JPH::BodyID body;          // invalid = no body
            Motion motion = Motion::None;
            bool kinematic = false;
            std::string shapeKey;
            Math::float3 scale;
            std::uint32_t revision = 0;

            Pose rest{ JPH::Vec3::sZero(), JPH::Quat::sIdentity() };
            Math::mat4 restInverse;    // of the rest pose, for the render offset
            Pose previous{ JPH::Vec3::sZero(), JPH::Quat::sIdentity() };
            Pose current{ JPH::Vec3::sZero(), JPH::Quat::sIdentity() };
            std::uint64_t activeStep = 0; // the last step the body was awake in
            bool inMotion = false;     // on the list of the awake
            std::uint32_t awakeIndex = 0; // where on it: a level leaving with a heap awake takes them off one by one
            bool hasOffset = false;    // this world has written its render offset
            Math::mat4 lastKinematicOffset; // what the buoyancy said at the last step
            bool kinematicValid = false;
            bool kinematicMoving = false; // told to move at the last step: it keeps that speed until told otherwise
            bool fellOut = false;      // below physics.killY: asleep where it is, and Keep leaves it alone
            bool floats = false;       // Dynamic and "buoyant": the water pushes it
            float floatFactor = 1.1f;
            float floatSpan = 0.5f;    // how far apart the water's height is sampled for its tilt, m
            float rolling = -1.0f;     // collision.rollingResistance; < 0 = the knob
            float rollingRadius = 0.1f;
            float velocityY = 0.0f;    // ApplyRollingResistance's memory
            std::unique_ptr<VesselSettings> vessel; // mesh.json "vessel": a powered boat, its own copy for --set vessel.*
            VesselState vesselState;   // the step's while one runs
            VesselWater water;         // the step's while one runs
            float halfLength = 0.0f;
            float halfBeam = 0.0f;
            JPH::Vec3 shownPosition = JPH::Vec3::sZero(); // as the last Display drew it, and the game's alone: who is near a boat asks every frame and must not wait for a step
            JPH::Vec3 shownForward = JPH::Vec3::sAxisX();
            JPH::Vec3 shownUp = JPH::Vec3::sAxisY();
            float shownSpeed = 0.0f; // vesselState's speed as Display saw it: the game's alone
            float shownThrottle = 0.0f; // and its engine and rudder
            float shownRudder = 0.0f;
            float shownImpact = 0.0f; // and its last slam (a share of the weight, fading): the spray's burst
            float shownImmersion = 0.0f; // how deep it sits (1 = at rest): no spray off a hull in the air
            float shownWaterHeight = 0.0f; // the surface under it: where its spray leaves and falls back to
            JPH::Vec3 shownVelocity = JPH::Vec3::sZero(); // the last step's motion of the origin and its turn: the spray's root moves
            JPH::Vec3 shownAngular = JPH::Vec3::sZero();  // with them, and a drop the side pushes leaves at least as fast
            // The anchor (ToggleAnchor, after a Join; the step reads them): its rode on the bow, the stem of the hull's box at
            // the draft in the body's space.
            bool anchored = false;
            JPH::Vec3 anchorPoint = JPH::Vec3::sZero();
            JPH::Vec3 bowLocal = JPH::Vec3::sZero();
            // A pilot other than the helm (World::SetPilot): pilotInput is the game's, stepPilotInput taken for the steps
            // where they start, as the helm's.
            float rudderLever = 0.0f; // m the propeller lies aft of the centre of mass, along the hull
            bool piloted = false;
            VesselInput pilotInput;
            VesselInput stepPilotInput;
            bool stepPiloted = false;  // piloted, as it stood where the step started
            // The hard mooring (HoldMooring): the step's. Moored once nobody steers it and it has slowed down, where it was then.
            bool moored = false;
            JPH::Vec3 mooredAt = JPH::Vec3::sZero();
            float mooredHeading = 0.0f;
        };

        std::unordered_map<RenderableObjectBase*, Entry> entries;
        std::unordered_map<std::string, BuiltShape> shapes;
        std::vector<Entry*> byBodyIndex;
        std::vector<Entry*> awake;       // Dynamic bodies between waking and their last pose
        std::vector<Entry*> kinematics;
        std::vector<Entry*> floaters;
        std::vector<Entry*> vessels;
        Entry* helm = nullptr;
        VesselInput helmInput;      // the game's
        VesselInput stepHelmInput;  // taken for the steps where they start: the step's thread owns this
        float stepMooring = 0.0f;   // physics.mooring* as they stood where the step started
        float stepMooringTurn = 0.0f;
        float stepMooringSettle = 0.0f;
        HelmState helmShown;        // as the last Display drew it: nothing here is the step's
        const OceanRenderable* ocean = nullptr;
        std::unique_ptr<PlayerBody> player;
        bool playerWanted = false;
        bool playerPossessed = true;
        Math::float3 playerSpawn;
        PlayerInput playerInput;
        Math::float3 playerEye;
        bool playerEyeValid = false;
        std::unordered_set<RenderableObjectBase*> dirty;
        std::vector<Event> eventScratch;
        JPH::BodyIDVector activeScratch;
        std::vector<JPH::BodyID> removeScratch;
        std::chrono::steady_clock::time_point lastAwakeLog{};

        std::unique_ptr<LayerTables> tables;
        std::unique_ptr<JPH::PhysicsSystem> system;
        std::unique_ptr<JPH::TempAllocator> temp;
        std::unique_ptr<JPH::JobSystemThreadPool> jobs;
        std::unique_ptr<LineRenderer> lines;
        bool joltHeld = false;
        bool worldBuiltForEnabled = false;
        bool simulating = false;
        bool layoutChanged = false;      // something new since the last OptimizeBroadPhase
        int appliedShapeRule = 1 | (static_cast<int>(Settings{}.hullTolerance * 10000.0f) << 4); // the defaults: autoShapes on, forceLod off
        float appliedGravity = 0.0f;
        double accumulator = 0.0;
        std::uint64_t stepIndex = 0;
        Stats stats;

        // ---- The step beside the frame (physics.async) ------------------------------------------------------------------------
        // While a step is in flight the step's thread owns the system, the entries' poses and steps, `awake`, `stepIndex` and `outcome`, and never touches an object: one may be
        // gone by now. The game thread touches none of them until Collect or Join says it is back; only the registry's queue and the settings go on.
        struct StepBatch
        {
            int count = 0;
            float step = 0.0f;
            float rollingResistance = 0.0f;
            float killY = 0.0f;
            float delayMs = 0.0f;
        };
        struct StepOutcome
        {
            std::uint32_t steps = 0;
            std::uint32_t fellOut = 0;
            std::uint32_t errors = 0;
            float stepMs = 0.0f;
        };
        // What the water says under a body. Read on the game thread (OceanReadback is its), pushed where the step runs.
        struct WaterSample
        {
            JPH::BodyID body;
            JPH::Vec3 normal;
            float height;
            float factor;
        };
        // Where a boat was sent: MoveKinematic is a speed and Jolt keeps it, so the batch's later steps send it to the same place, which stops it.
        struct KinematicTarget
        {
            JPH::BodyID body;
            JPH::Vec3 position;
            JPH::Quat rotation;
        };
        StepThread stepThread;
        StepBatch batch;
        StepOutcome outcome;
        std::vector<WaterSample> water;
        std::vector<KinematicTarget> kinematicTargets;
        std::unordered_map<std::string, std::string> geometryStamps; // BuildDirty's: one look at each file
        bool stepInFlight = false;
        bool poseOwed = false;           // a step was handed over: the frame's poses come from it, at the hand-over
        int stepsOwed = 0;               // Tick counted them, KickOwed starts them
        float stepOwedSeconds = 0.0f;

        ~Impl()
        {
            DestroySystem();
            if (joltHeld) { ReleaseJolt(); }
        }

        // The step's stats, on the game thread.
        void MergeOutcome()
        {
            if (outcome.steps == 0) { return; }
            stats.stepMs = outcome.stepMs;
            stats.stepMsEma = stats.stepsTotal == 0 ? outcome.stepMs : stats.stepMsEma + 0.05f * (outcome.stepMs - stats.stepMsEma);
            stats.stepsTotal += outcome.steps;
            stats.stepsLastFrame += outcome.steps;
            stats.fellOut += outcome.fellOut;
            stats.stepErrors += outcome.errors;
            outcome = StepOutcome{};
        }

        // A step that is done belongs to the game again. False = still running.
        bool Collect()
        {
            if (!stepInFlight) { return true; }
            if (!stepThread.Done()) { return false; }
            stepInFlight = false;
            MergeOutcome();
            return true;
        }

        // Whatever reaches the system, or an entry's pose, calls this first.
        void Join()
        {
            if (!stepInFlight) { return; }
            if (!stepThread.Done())
            {
                CPU_SCOPE(ProfilerScopes::kPhysicsWaitStep);
                ++stats.stepWaits;
                stepThread.Wait();
            }
            Collect();
        }

        void EnsureSystem()
        {
            if (system) { return; }
            MEM_SCOPE(MemTag::Physics);
            if (!joltHeld)
            {
                AcquireJolt();
                joltHeld = true;
            }
            tables = std::make_unique<LayerTables>();
            system = std::make_unique<JPH::PhysicsSystem>();
            // Orders of magnitude from Jolt's HelloWorld for a real project; bodies fit wind_test 40 times over.
            constexpr JPH::uint kMaxBodies = 65536;
            constexpr JPH::uint kBodyMutexes = 0; // its default
            constexpr JPH::uint kMaxBodyPairs = 65536;
            // A heap of 5000 bodies ran out at Jolt's sample's 10240: the contacts past it are simply not there, and the bodies fall through the island (5271 of 10000 left the world).
            constexpr JPH::uint kMaxContactConstraints = 65536;
            system->Init(kMaxBodies, kBodyMutexes, kMaxBodyPairs, kMaxContactConstraints, tables->broadPhase, *tables->objectVsBroadPhase, tables->pairs);
            appliedGravity = GameSettings().gravity;
            system->SetGravity(JPH::Vec3(0.0f, appliedGravity, 0.0f));
            // A step takes the whole contact table out of this (~1 KB each), and touches what it uses: reserved, not resident. Past it, malloc: the plain allocator aborts there.
            temp = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(128u * 1024u * 1024u);
            stats.worldExists = true;
            LOG_INFO(logging::LogCategory::Physics, "physics: world up, {} bodies at most", kMaxBodies);
        }

        void DestroySystem()
        {
            Join();
            stepThread.Stop();
            poseOwed = false;
            stepsOwed = 0;
            lines.reset();
            player.reset(); // playerWanted stays: the world's return brings it back
            playerEyeValid = false;
            floaters.clear();
            for (auto& [key, entry] : entries)
            {
                entry.body = JPH::BodyID();
                entry.inMotion = false;
            }
            awake.clear();
            kinematics.clear();
            byBodyIndex.clear();
            jobs.reset();
            temp.reset();
            system.reset();
            tables.reset();
            shapes.clear();
            stats = Stats{};
        }

        void EnsureJobs(int threads)
        {
            if (!jobs)
            {
                MEM_SCOPE(MemTag::Physics);
                jobs = std::make_unique<JPH::JobSystemThreadPool>();
                jobs->SetThreadInitFunction([](int index)
                {
                    logging::SetCurrentThreadName("Physics");
                    Profiler::Get().SetThreadName("Physics " + std::to_string(index)); // its row in a --trace
                    Profiler::MarkThreadCrossFrame(); // a step lies across the frames beside it; dropped otherwise, the longest ones first
                });
                jobs->Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
            }
            else if (jobs->GetMaxConcurrency() != threads + 1)
            {
                jobs->SetNumThreads(threads);
            }
        }

        // What this object wants of the world right now.
        static bool WantsBody(const RenderableObject& object)
        {
            return object.GetPhysicsMotion() != Motion::None && object.IsVisibleGame() &&
                object.GetCollisionSettings().shape != ShapeKind::None && !object.GetGeometryPath().empty();
        }

        void Forget(Entry& entry)
        {
            if (entry.body.IsInvalid()) { return; }
            const JPH::uint index = entry.body.GetIndex();
            if (index < byBodyIndex.size()) { byBodyIndex[index] = nullptr; }
            if (entry.inMotion)
            {
                awake[entry.awakeIndex] = awake.back();
                awake[entry.awakeIndex]->awakeIndex = entry.awakeIndex;
                awake.pop_back();
                entry.inMotion = false;
            }
            if (entry.floats)
            {
                floaters.erase(std::remove(floaters.begin(), floaters.end(), &entry), floaters.end());
                entry.floats = false;
            }
            if (entry.vessel)
            {
                vessels.erase(std::remove(vessels.begin(), vessels.end(), &entry), vessels.end());
                if (helm == &entry)
                {
                    helm = nullptr;
                    helmShown = HelmState{};
                }
                entry.vessel.reset();
            }
            if (entry.kinematic)
            {
                kinematics.erase(std::remove(kinematics.begin(), kinematics.end(), &entry), kinematics.end());
                if (stats.kinematicBodies > 0) { --stats.kinematicBodies; }
            }
            else if (entry.motion == Motion::Dynamic)
            {
                if (stats.dynamicBodies > 0) { --stats.dynamicBodies; }
            }
            else if (stats.staticBodies > 0)
            {
                --stats.staticBodies;
            }
            removeScratch.push_back(entry.body);
            entry.body = JPH::BodyID();
        }

        void FlushRemovals()
        {
            if (removeScratch.empty() || !system) { removeScratch.clear(); return; }
            std::sort(removeScratch.begin(), removeScratch.end()); // the free list hands these numbers out again
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            bodies.RemoveBodies(removeScratch.data(), static_cast<int>(removeScratch.size()));
            bodies.DestroyBodies(removeScratch.data(), static_cast<int>(removeScratch.size()));
            removeScratch.clear();
        }

        // Back where the level put it. The history is reset at the hand-over: a jump of metres in one frame would be read as a
        // motion vector, and DLSS would smear the object.
        void ClearOffset(Entry& entry)
        {
            if (!entry.hasOffset || !entry.object) { return; }
            entry.object->ClearRenderOffset();
            entry.object->RequestMotionHistoryReset();
            entry.hasOffset = false;
        }

        void Drain()
        {
            eventScratch.clear();
            {
                Registry& registry = TheRegistry();
                std::lock_guard<std::mutex> lock(registry.mutex);
                eventScratch.swap(registry.events);
            }
            if (!eventScratch.empty() || !dirty.empty()) { Join(); }
            for (const Event& event : eventScratch)
            {
                switch (event.kind)
                {
                case EventKind::Added:
                {
                    Entry entry;
                    entry.object = event.renderable;
                    entries[event.object] = std::move(entry);
                    dirty.insert(event.object);
                    break;
                }
                case EventKind::Removed:
                {
                    const auto it = entries.find(event.object);
                    if (it != entries.end())
                    {
                        it->second.object = nullptr; // gone: its offset is nobody's
                        Forget(it->second);
                        entries.erase(it);
                    }
                    dirty.erase(event.object);
                    break;
                }
                case EventKind::Changed:
                    if (entries.count(event.object) != 0) { dirty.insert(event.object); }
                    break;
                }
            }
            FlushRemovals();
            if (!GameSettings().enabled)
            {
                dirty.clear();
                return;
            }
            if (!worldBuiltForEnabled)
            {
                // Switched back on: every registered object is owed its body.
                Join();
                worldBuiltForEnabled = true;
                for (const auto& [key, entry] : entries) { dirty.insert(key); }
            }
            // physics.autoShapes / forceLod / hullTolerance: every shape's key is another now.
            const int shapeRule = (GameSettings().autoShapes ? 1 : 0) | ((GameSettings().forceLod + 1) << 1) | (static_cast<int>(GameSettings().hullTolerance * 10000.0f) << 4);
            const bool ruleChanged = shapeRule != appliedShapeRule;
            if (ruleChanged)
            {
                Join();
                appliedShapeRule = shapeRule;
                for (const auto& [key, entry] : entries) { dirty.insert(key); }
            }
            if (!dirty.empty()) { BuildDirty(); }
            if (ruleChanged && system)
            {
                EvictUnusedShapes();
                system->OptimizeBroadPhase();
                layoutChanged = false;
            }
        }

        struct Wanted
        {
            Entry* entry;
            ShapeKind kind;
            std::string key;
            bool dynamicBody;
        };

        // When the file was written, in the shape's key: a re-bake is another shape, and a reload would otherwise take the old one back out of the cache.
        const std::string& GeometryStamp(const std::string& path)
        {
            const auto it = geometryStamps.find(path);
            if (it != geometryStamps.end()) { return it->second; }
            std::error_code error;
            const auto written = std::filesystem::last_write_time(path, error);
            return geometryStamps.emplace(path, error ? std::string() : std::to_string(written.time_since_epoch().count())).first->second;
        }

        // A compound takes a uniform scale and a sphere nothing else: on those Jolt would quietly use the nearest it can, a palm mirrored or stretched lying on its side or a third
        // too short. So such a copy's primitives are built at that scale, one shape per scale; everything Jolt can scale as asked shares the asset's shape.
        JPH::RefConst<JPH::Shape> InstanceOf(const Wanted& wanted, const BuiltShape& built, const Math::float3& scale, std::string& error)
        {
            if (wanted.kind != ShapeKind::Custom || built.shape->IsValidScale(ToJolt(scale))) { return ScaledInstance(*built.shape, scale, error); }
            const std::string key = wanted.key + "|s" + std::to_string(scale.x) + "," + std::to_string(scale.y) + "," + std::to_string(scale.z);
            auto it = shapes.find(key);
            if (it == shapes.end())
            {
                const RenderableObject& object = *wanted.entry->object;
                it = shapes.emplace(key, BuildAssetShape(object.GetGeometryPath(), BakeScale(object.GetCollisionSettings(), scale), wanted.dynamicBody)).first;
                RecountShapes();
            }
            if (!it->second.shape) { error = it->second.error; }
            return it->second.shape;
        }

        // The bodies of what changed. New shapes in parallel: most of a level's cost lies there.
        void BuildDirty()
        {
            MEM_SCOPE(MemTag::Physics);
            const auto start = std::chrono::steady_clock::now();
            EnsureSystem();
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            geometryStamps.clear();

            std::vector<Wanted> wanted;
            wanted.reserve(dirty.size());
            for (RenderableObjectBase* key : dirty)
            {
                const auto it = entries.find(key);
                if (it == entries.end()) { continue; }
                Entry& entry = it->second;
                RenderableObject& object = *entry.object;
                if (!WantsBody(object))
                {
                    Forget(entry);
                    ClearOffset(entry);
                    entry.motion = object.GetPhysicsMotion();
                    continue;
                }
                const bool dynamicBody = object.GetPhysicsMotion() == Motion::Dynamic;
                const bool kinematicBody = !dynamicBody && object.IsBuoyant();
                const ShapeKind kind = ResolveShapeKind(object.GetCollisionSettings().shape, dynamicBody);
                std::string shapeKey = ShapeKeyOf(object.GetGeometryPath(), kind == ShapeKind::Custom ? std::string() : GeometryStamp(object.GetGeometryPath()), object.GetCollisionSettings(), kind);

                const bool floats = dynamicBody && object.IsBuoyant();
                const bool same = !entry.body.IsInvalid() && entry.shapeKey == shapeKey && entry.motion == object.GetPhysicsMotion() &&
                    entry.kinematic == kinematicBody && entry.floats == floats && entry.revision == object.GetBodyRevision() &&
                    entry.scale.x == object.GetScale().x && entry.scale.y == object.GetScale().y && entry.scale.z == object.GetScale().z;
                if (same)
                {
                    Teleport(entry);
                    continue;
                }
                Forget(entry);
                ClearOffset(entry);
                wanted.push_back(Wanted{ &entry, kind, std::move(shapeKey), dynamicBody });
            }
            FlushRemovals();
            dirty.clear();
            if (wanted.empty()) { return; }
            BOOT_SCOPE("Physics bodies"); // past the return: an entry a call, kept for good, and an object that turns every frame comes through here every frame
            // Jolt's answer depends on the order the bodies came in, and `dirty` walks by the objects' addresses: the same level gave another pile from run to run (gate `async`'s
            // control). By the bytes of the position: any order that is the same every time will do, and a NaN must not break the sort.
            std::sort(wanted.begin(), wanted.end(), [](const Wanted& a, const Wanted& b)
            {
                if (a.dynamicBody != b.dynamicBody) { return !a.dynamicBody; } // the world first: past the bodies' limit it is what falls away, and then everything falls through
                const Math::float3 pa = a.entry->object->GetPosition();
                const Math::float3 pb = b.entry->object->GetPosition();
                const int order = std::memcmp(&pa, &pb, sizeof(pa));
                return order != 0 ? order < 0 : a.key < b.key;
            });

            // The shapes not in the cache yet, once each.
            std::vector<const Wanted*> toBuild;
            {
                std::unordered_set<std::string> seen;
                for (const Wanted& w : wanted)
                {
                    if (shapes.find(w.key) == shapes.end() && seen.insert(w.key).second) { toBuild.push_back(&w); }
                }
            }
            std::vector<BuiltShape> built(toBuild.size());
            TaskSystem::ParallelFor(toBuild.size(),
                [&toBuild, &built](size_t i)
                {
                    MEM_SCOPE(MemTag::Physics);
                    const Wanted& w = *toBuild[i];
                    const RenderableObject& object = *w.entry->object;
                    built[i] = BuildAssetShape(object.GetGeometryPath(), object.GetCollisionSettings(), w.dynamicBody);
                }, 1);
            for (size_t i = 0; i < toBuild.size(); ++i)
            {
                if (!built[i].error.empty())
                {
                    LOG_WARNING(logging::LogCategory::Physics, "physics: shape of '{}' ({}): {}",
                        toBuild[i]->entry->object->GetGeometryPath(), ShapeKindName(toBuild[i]->kind), built[i].error);
                }
                shapes.emplace(toBuild[i]->key, std::move(built[i]));
            }
            RecountShapes();

            // The bodies, as batches: a batch reaches the broad phase at once.
            std::vector<JPH::BodyID> staticIds;
            std::vector<JPH::BodyID> movingIds;
            staticIds.reserve(wanted.size());
            for (const Wanted& w : wanted)
            {
                Entry& entry = *w.entry;
                RenderableObject& object = *entry.object;
                const BuiltShape& built_ = shapes.find(w.key)->second;
                const VesselSettings* vesselSettings = object.GetPhysicsMotion() == Motion::Dynamic && object.IsBuoyant() ? object.GetVesselSettings() : nullptr;
                entry.motion = object.GetPhysicsMotion();
                entry.shapeKey = w.key;
                entry.scale = object.GetScale();
                entry.revision = object.GetBodyRevision();
                entry.kinematic = entry.motion != Motion::Dynamic && object.IsBuoyant();
                if (!built_.shape) { continue; } // nothing solid in it: only leaves

                std::string error;
                JPH::RefConst<JPH::Shape> scaled = InstanceOf(w, built_, entry.scale, error);
                if (!scaled)
                {
                    LOG_WARNING(logging::LogCategory::Physics, "physics: scale of '{}': {}", object.GetGeometryPath(), error);
                    continue;
                }
                float vesselFactor = 1.0f;
                if (vesselSettings)
                {
                    const JPH::RefConst<JPH::Shape> boat = MakeVesselShape(*scaled, *vesselSettings, entry.scale, vesselFactor, error);
                    if (boat) { scaled = boat; }
                    else
                    {
                        LOG_WARNING(logging::LogCategory::Physics, "physics: '{}' is no boat: {}", object.GetGeometryPath(), error);
                        vesselSettings = nullptr;
                    }
                }
                const Pose pose = AuthoredPose(object);
                entry.rest = pose;
                entry.previous = pose;
                entry.current = pose;
                entry.restInverse = Math::mat4::Inverse(RigidMatrix(pose.position, pose.rotation));
                entry.kinematicValid = false;
                entry.kinematicMoving = false;
                entry.fellOut = false; // a new body, where the level has it

                const bool dynamicBody = entry.motion == Motion::Dynamic;
                const CollisionSettings& settings = object.GetCollisionSettings();
                const JPH::EMotionType motionType = dynamicBody ? JPH::EMotionType::Dynamic
                    : entry.kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static;
                JPH::BodyCreationSettings creation(scaled.GetPtr(), pose.position, pose.rotation, motionType,
                    motionType == JPH::EMotionType::Static ? layers::kNonMoving : layers::kMoving);
                creation.mUserData = reinterpret_cast<JPH::uint64>(static_cast<RenderableObjectBase*>(&object));
                creation.mFriction = settings.friction;
                creation.mRestitution = settings.restitution;
                if (motionType == JPH::EMotionType::Kinematic) { GiveKinematicMass(creation); }
                if (dynamicBody)
                {
                    if (settings.mass > 0.0f)
                    {
                        creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                        creation.mMassPropertiesOverride.mMass = settings.mass;
                    }
                    // A triangle mesh has no thickness: a body that moves past a triangle within one step is through it. A coconut dropped
                    // 14 m lands at 16 m/s, 27 cm a step against its 13 cm radius, and 6 of 30 went on falling for ever. Jolt only casts
                    // when a body moves past 75 % of its inner radius in a step, so a slow one pays nothing for this.
                    creation.mMotionQuality = JPH::EMotionQuality::LinearCast;
                    creation.mAngularDamping = settings.angularDamping >= 0.0f ? settings.angularDamping : GameSettings().angularDamping;
                    entry.rolling = settings.rollingResistance;
                    entry.rollingRadius = scaled->GetInnerRadius();
                    entry.velocityY = 0.0f;
                    if (object.IsBuoyant())
                    {
                        // Waves move under it: it must not fall asleep on them.
                        creation.mAllowSleeping = false;
                        creation.mLinearDamping = kFloatLinearDamping;
                        creation.mAngularDamping = kFloatAngularDamping;
                        entry.floats = true;
                        entry.floatFactor = settings.floatFactor;
                        const float largest = std::max({ built_.boundsMax.x - built_.boundsMin.x, built_.boundsMax.z - built_.boundsMin.z, 0.05f });
                        entry.floatSpan = 0.5f * largest * std::max(std::fabs(entry.scale.x), std::fabs(entry.scale.z));
                    }
                    if (vesselSettings) { SetVesselBody(creation, *vesselSettings); }
                }
                JPH::Body* body = bodies.CreateBody(creation);
                if (!body)
                {
                    LOG_WARNING_THROTTLED(std::chrono::seconds(5), logging::LogCategory::Physics, "physics: out of bodies");
                    continue;
                }
                entry.body = body->GetID();
                if (vesselSettings)
                {
                    entry.vessel = std::make_unique<VesselSettings>(*vesselSettings);
                    entry.vesselState = VesselState{};
                    entry.water = VesselWater{};
                    entry.floatFactor = vesselFactor;
                    const JPH::Vec3 along = ToJolt(vesselSettings->forward).Normalized();
                    const JPH::Vec3 half = scaled->GetLocalBounds().GetExtent(); // the shape's own: a custom shape's bounds above are its parts' spheres
                    entry.halfLength = std::fabs(along.GetX()) * half.GetX() + std::fabs(along.GetZ()) * half.GetZ();
                    entry.halfBeam = std::fabs(along.GetZ()) * half.GetX() + std::fabs(along.GetX()) * half.GetZ();
                    const JPH::Vec3 middle = scaled->GetLocalBounds().GetCenter();
                    entry.bowLocal = JPH::Vec3(middle.GetX(), vesselSettings->draft * entry.scale.y, middle.GetZ()) + JPH::Vec3(along.GetX(), 0.0f, along.GetZ()) * entry.halfLength;
                    const JPH::Vec3 propeller(vesselSettings->propeller.x * entry.scale.x, vesselSettings->propeller.y * entry.scale.y, vesselSettings->propeller.z * entry.scale.z);
                    entry.rudderLever = (scaled->GetCenterOfMass() - propeller).Dot(along);
                    entry.anchored = false;
                    entry.shownPosition = pose.position;
                    entry.shownForward = pose.rotation * along;
                    entry.shownUp = pose.rotation * JPH::Vec3::sAxisY();
                    vessels.push_back(&entry);
                    LOG_INFO(logging::LogCategory::Physics, "physics: '{}' is a boat: {:.0f} kg, hull volume {:.1f} m3, {:.2f} m3 under still water (buoyancy factor {:.2f}), {:.1f} x {:.1f} m",
                        object.GetGeometryPath(), vesselSettings->mass, scaled->GetVolume(), scaled->GetVolume() / vesselFactor, vesselFactor, 2.0f * entry.halfLength, 2.0f * entry.halfBeam);
                }
                else if (entry.floats) { floaters.push_back(&entry); }
                const JPH::uint index = entry.body.GetIndex();
                if (index >= byBodyIndex.size()) { byBodyIndex.resize(index + 1, nullptr); }
                byBodyIndex[index] = &entry;
                if (motionType == JPH::EMotionType::Static)
                {
                    staticIds.push_back(entry.body);
                    ++stats.staticBodies;
                }
                else
                {
                    movingIds.push_back(entry.body);
                    if (entry.kinematic)
                    {
                        kinematics.push_back(&entry);
                        ++stats.kinematicBodies;
                    }
                    else
                    {
                        ++stats.dynamicBodies;
                    }
                }
            }
            if (!staticIds.empty())
            {
                const JPH::BodyInterface::AddState state = bodies.AddBodiesPrepare(staticIds.data(), static_cast<int>(staticIds.size()));
                bodies.AddBodiesFinalize(staticIds.data(), static_cast<int>(staticIds.size()), state, JPH::EActivation::DontActivate);
                WakeAround(staticIds);
            }
            if (!movingIds.empty())
            {
                const JPH::BodyInterface::AddState state = bodies.AddBodiesPrepare(movingIds.data(), static_cast<int>(movingIds.size()));
                bodies.AddBodiesFinalize(movingIds.data(), static_cast<int>(movingIds.size()), state,
                    simulating ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            }
            layoutChanged = true;
            stats.lastBuildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            LOG_DEBUG(logging::LogCategory::Physics, "physics: {} bodies, {} new shapes in {:.1f} ms ({} in the cache)",
                wanted.size(), toBuild.size(), stats.lastBuildMs, shapes.size());
        }

        void RecountShapes()
        {
            stats.shapes = stats.meshShapes = stats.hullShapes = stats.primitiveShapes = 0;
            stats.shapeTriangles = 0;
            for (const auto& [key, built] : shapes)
            {
                if (!built.shape) { continue; }
                ++stats.shapes;
                if (built.kind == ShapeKind::Mesh)
                {
                    ++stats.meshShapes;
                    stats.shapeTriangles += built.triangles;
                }
                else if (built.kind == ShapeKind::Hull) { ++stats.hullShapes; }
                else { ++stats.primitiveShapes; }
            }
        }

        // The shapes no body holds any more. An empty entry (only leaves) stays: it costs nothing, and without it the next copy reads the
        // file to find that out again.
        void EvictUnusedShapes()
        {
            for (auto it = shapes.begin(); it != shapes.end();)
            {
                if (!it->second.shape || it->second.shape->GetRefCount() > 1) { ++it; }
                else { it = shapes.erase(it); }
            }
            RecountShapes();
        }

        // A sleeping body does not notice the floor that appeared or went away under it.
        void WakeAround(const std::vector<JPH::BodyID>& ids)
        {
            if (!simulating || stats.dynamicBodies == 0) { return; }
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            for (const JPH::BodyID id : ids)
            {
                bodies.ActivateBodiesInAABox(bodies.GetTransformedShape(id).GetWorldSpaceBounds(), {}, {});
            }
        }

        // The authored transform moved (a gizmo, undo, Keep): the body stands there now, at rest.
        void Teleport(Entry& entry)
        {
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            RenderableObject& object = *entry.object;
            const Pose pose = AuthoredPose(object);
            const bool samePlace = (pose.position - entry.rest.position).LengthSq() == 0.0f && pose.rotation == entry.rest.rotation;
            if (samePlace) { return; }
            entry.rest = pose;
            entry.previous = pose;
            entry.current = pose;
            entry.restInverse = Math::mat4::Inverse(RigidMatrix(pose.position, pose.rotation));
            const JPH::AABox before = bodies.GetTransformedShape(entry.body).GetWorldSpaceBounds();
            bodies.SetPositionAndRotation(entry.body, pose.position, pose.rotation, JPH::EActivation::DontActivate);
            if (entry.motion == Motion::Dynamic)
            {
                bodies.SetLinearAndAngularVelocity(entry.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                entry.fellOut = false;
                ClearOffset(entry);
            }
            if (simulating && stats.dynamicBodies > 0)
            {
                bodies.ActivateBodiesInAABox(before, {}, {});
                bodies.ActivateBodiesInAABox(bodies.GetTransformedShape(entry.body).GetWorldSpaceBounds(), {}, {});
            }
        }

        // Every Dynamic body. In the bodies' own order: `entries` walks by the objects' addresses, and the order a body wakes in is the order it is solved in.
        void WakeAllDynamic()
        {
            std::vector<JPH::BodyID> ids;
            ids.reserve(stats.dynamicBodies);
            for (const auto& [key, entry] : entries)
            {
                if (!entry.body.IsInvalid() && entry.motion == Motion::Dynamic && !entry.fellOut) { ids.push_back(entry.body); }
            }
            if (ids.empty()) { return; }
            std::sort(ids.begin(), ids.end());
            system->GetBodyInterface().ActivateBodies(ids.data(), static_cast<int>(ids.size()));
        }

        void SetSimulating(bool on)
        {
            if (on == simulating) { return; }
            Join();
            poseOwed = false;
            stepsOwed = 0;
            simulating = on;
            accumulator = 0.0;
            if (!system) { return; }
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            if (on)
            {
                WakeAllDynamic();
                LOG_INFO(logging::LogCategory::Physics, "physics: simulating, {} dynamic and {} kinematic bodies",
                    stats.dynamicBodies, stats.kinematicBodies);
                return;
            }
            // Stop: everything back where the level put it.
            for (Entry* entry : awake) { entry->inMotion = false; }
            awake.clear();
            for (auto& [key, entry] : entries)
            {
                if (entry.body.IsInvalid() || (entry.motion != Motion::Dynamic && !entry.kinematic)) { continue; }
                bodies.DeactivateBody(entry.body);
                bodies.SetPositionAndRotation(entry.body, entry.rest.position, entry.rest.rotation, JPH::EActivation::DontActivate);
                bodies.SetLinearAndAngularVelocity(entry.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                entry.previous = entry.rest;
                entry.current = entry.rest;
                entry.kinematicValid = false;
                entry.fellOut = false;
                ClearOffset(entry);
                if (entry.vessel)
                {
                    entry.object->SetLeavesGiProbes(false);
                    entry.piloted = false;
                    entry.pilotInput = VesselInput{};
                    entry.stepPilotInput = VesselInput{};
                    entry.vesselState = VesselState{};
                    entry.water = VesselWater{};
                    entry.shownPosition = entry.rest.position;
                    entry.shownForward = entry.rest.rotation * ToJolt(entry.vessel->forward).Normalized();
                    entry.shownUp = entry.rest.rotation * JPH::Vec3::sAxisY();
                    entry.shownSpeed = 0.0f;
                    entry.shownVelocity = JPH::Vec3::sZero();
                    entry.shownAngular = JPH::Vec3::sZero();
                    entry.anchored = false; // the session's, weighed with it
                    entry.moored = false;   // and the mooring: the next session moors it where it is then
                    entry.shownImpact = 0.0f;
                    entry.shownImmersion = 0.0f;
                    entry.shownWaterHeight = 0.0f;
                }
            }
            stats.fellOut = 0;
            LOG_INFO(logging::LogCategory::Physics, "physics: stopped, the bodies are back where the level put them");
        }

        // The boats: their pose is the buoyancy's, the world follows so what they hit moves.
        bool MoveKinematics(float step)
        {
            bool moved = false;
            kinematicTargets.clear();
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            for (Entry* entry : kinematics)
            {
                const RenderableObject& object = *entry->object;
                const Math::mat4 offset = object.HasRenderOffset() ? object.GetRenderOffset() : Math::mat4::Identity();
                const bool same = entry->kinematicValid && std::memcmp(&offset.m, &entry->lastKinematicOffset.m, sizeof(offset.m)) == 0;
                if (same && !entry->kinematicMoving) { continue; }
                entry->lastKinematicOffset = offset;
                entry->kinematicValid = true;
                entry->kinematicMoving = !same; // sent to where it already is: it stops, and the next step owes it nothing
                const Math::mat4 world = RigidMatrix(entry->rest.position, entry->rest.rotation) * offset;
                const JPH::Vec3 position(world.m._41, world.m._42, world.m._43);
                const JPH::Quat rotation = ToJolt(QuatFromMatrix(world));
                bodies.MoveKinematic(entry->body, position, rotation, step);
                kinematicTargets.push_back(KinematicTarget{ entry->body, position, rotation });
                moved = true;
            }
            return moved;
        }

        // The bodies in the water: Jolt's impulse model over the surface under each, the plane through three heights so a slope tilts
        // it. The same surface the boats and the renderer's vertices read (OceanReadback).
        void SampleWater(float elapsed)
        {
            SampleVesselWater(elapsed);
            water.clear();
            if (floaters.empty() || !ocean || !ocean->IsVisibleGame()) { return; }
            const OceanReadback& readback = ocean->GetReadback();
            const bool waves = readback.HasSurface();
            const float level = ocean->GetWaterLevel();
            const auto height = [&](float x, float z) { return waves ? readback.SampleHeight(x, z) : level; };
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            for (const Entry* entry : floaters)
            {
                if (entry->fellOut) { continue; } // a push wakes it, and under the world it would sink for ever
                const JPH::Vec3 centre = bodies.GetCenterOfMassPosition(entry->body);
                const float span = entry->floatSpan;
                const float h0 = height(centre.GetX(), centre.GetZ());
                const float hx = height(centre.GetX() + span, centre.GetZ());
                const float hz = height(centre.GetX(), centre.GetZ() + span);
                water.push_back(WaterSample{ entry->body, JPH::Vec3(-(hx - h0) / span, 1.0f, -(hz - h0) / span).Normalized(), h0, entry->floatFactor });
            }
        }

        void SampleVesselWater(float elapsed)
        {
            if (vessels.empty()) { return; }
            const bool sea = ocean && ocean->IsVisibleGame();
            const OceanReadback* readback = sea ? &ocean->GetReadback() : nullptr;
            const bool waves = sea && readback->HasSurface();
            const float level = sea ? ocean->GetWaterLevel() : 0.0f;
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            for (Entry* entry : vessels)
            {
                if (!sea || entry->fellOut)
                {
                    entry->water.valid = false; // a push wakes it, and under the world it would sink for ever
                    continue;
                }
                // The boats' wake (OceanWake), as the free height: a boat lying still does not float in its own hollow and
                // rocks in another's wake. Under way it leaves the wake out -- its own bow wave and stern trough are what the
                // hump, the planing and their trim already stand for, and taken twice they would trim it twice -- and so
                // another boat's too, the price of one field for all of them. Read here, between the steps (no step runs).
                const float speed = std::fabs(entry->vesselState.speed);
                const float t = std::clamp((speed - 1.0f) / 2.0f, 0.0f, 1.0f);
                const float wakeShare = 1.0f - t * t * (3.0f - 2.0f * t);
                const std::function<float(float, float)> height = [readback, waves, level, wakeShare](float x, float z)
                    { return waves ? readback->SampleHeight(x, z, wakeShare) : level; };
                MeasureVesselWater(entry->water, height, bodies.GetPosition(entry->body), bodies.GetCenterOfMassPosition(entry->body), bodies.GetRotation(entry->body),
                    *entry->vessel, entry->scale, entry->halfLength, entry->halfBeam, elapsed);
            }
        }

        // The orders as they stand where a step starts: the step's thread owns the copies.
        void TakeHelmInputs()
        {
            stepHelmInput = helmInput;
            for (Entry* entry : vessels)
            {
                entry->stepPilotInput = entry->piloted ? entry->pilotInput : VesselInput{};
                entry->stepPiloted = entry->piloted;
            }
            const Settings& settings = GameSettings();
            stepMooring = settings.mooring;
            stepMooringTurn = settings.mooringTurn;
            stepMooringSettle = settings.mooringSettle;
        }

        void PushVessels(float step)
        {
            if (vessels.empty()) { return; }
            const JPH::BodyLockInterface& locks = system->GetBodyLockInterface();
            const JPH::Vec3 gravity = system->GetGravity();
            for (Entry* entry : vessels)
            {
                if (!entry->water.valid) { continue; }
                system->GetBodyInterface().ActivateBody(entry->body); // Body's own impulses wake nothing
                JPH::BodyLockWrite lock(locks, entry->body);
                if (!lock.Succeeded()) { continue; }
                PushVessel(lock.GetBody(), *entry->vessel, entry->scale, entry->vesselState, entry == helm ? stepHelmInput : entry->stepPilotInput, entry->water, entry->floatFactor, gravity, step);
                if (entry->anchored) { PullAnchor(lock.GetBody(), *entry->vessel, entry->bowLocal, entry->anchorPoint, step); }
                // Nobody steers it and its own anchor is up: the hard mooring, once it has slowed down, where it was then.
                JPH::Body& body = lock.GetBody();
                if (entry == helm || entry->stepPiloted || entry->anchored || (stepMooring <= 0.0f && stepMooringTurn <= 0.0f))
                {
                    entry->moored = false;
                    continue;
                }
                if (!entry->moored)
                {
                    const JPH::Vec3 velocity = body.GetLinearVelocity();
                    if (velocity.GetX() * velocity.GetX() + velocity.GetZ() * velocity.GetZ() > stepMooringSettle * stepMooringSettle) { continue; }
                    const JPH::RVec3 position = body.GetPosition();
                    const JPH::Vec3 bow = body.GetRotation() * ToJolt(entry->vessel->forward).Normalized();
                    entry->moored = true;
                    entry->mooredAt = JPH::Vec3(static_cast<float>(position.GetX()), 0.0f, static_cast<float>(position.GetZ()));
                    entry->mooredHeading = std::atan2(bow.GetX(), bow.GetZ());
                }
                HoldMooring(body, *entry->vessel, entry->mooredAt, entry->mooredHeading, stepMooring, stepMooringTurn, step);
            }
        }

        // Where the frame draws the boats, for whoever asks about them on the game's side.
        void ShowVessels(float alpha)
        {
            for (Entry* entry : vessels)
            {
                entry->shownPosition = entry->previous.position + (entry->current.position - entry->previous.position) * alpha;
                const JPH::Quat rotation = entry->previous.rotation.SLERP(entry->current.rotation, alpha).Normalized();
                const JPH::Vec3 forward = (rotation * ToJolt(entry->vessel->forward)).Normalized();
                entry->shownForward = forward;
                entry->shownUp = rotation * JPH::Vec3::sAxisY();
                entry->shownSpeed = entry->vesselState.speed;
                entry->shownThrottle = entry->vesselState.throttle;
                entry->shownRudder = entry->vesselState.rudder;
                // The last step's motion, from its two poses (the step's own velocities are the step thread's).
                const float step = static_cast<float>(StepSeconds());
                entry->shownVelocity = (entry->current.position - entry->previous.position) / step;
                JPH::Quat turn = (entry->current.rotation * entry->previous.rotation.Conjugated()).Normalized();
                if (turn.GetW() < 0.0f) { turn = -turn; }
                JPH::Vec3 axis = JPH::Vec3::sZero();
                float angle = 0.0f;
                turn.GetAxisAngle(axis, angle);
                entry->shownAngular = angle > 1e-7f ? axis * (angle / step) : JPH::Vec3::sZero();
                entry->shownImpact = entry->vesselState.impact;
                entry->shownImmersion = entry->vesselState.immersion;
                entry->shownWaterHeight = entry->vesselState.waterHeight;
                // With a margin, so a boat crawling at the limit does not go in and out of them.
                const float probeSpeed = entry->vessel->giProbeSpeed;
                const float speed = std::fabs(entry->vesselState.speed);
                if (probeSpeed > 0.0f && speed > probeSpeed) { entry->object->SetLeavesGiProbes(true); }
                else if (probeSpeed <= 0.0f || speed < 0.7f * probeSpeed) { entry->object->SetLeavesGiProbes(false); }
                if (entry != helm) { continue; }
                helmShown.exists = true;
                helmShown.object = entry->object;
                helmShown.settings = entry->vessel.get();
                helmShown.position = FromJolt(entry->shownPosition);
                helmShown.rotation = FromJolt(rotation);
                helmShown.forward = FromJolt(forward);
                helmShown.scale = entry->scale;
                const JPH::Vec3 up = rotation * JPH::Vec3::sAxisY();
                helmShown.starboardY = up.Cross(forward).GetY();
                helmShown.upY = up.GetY();
                helmShown.halfLength = entry->halfLength;
                helmShown.halfBeam = entry->halfBeam;
                helmShown.anchored = entry->anchored;
                helmShown.vessel = entry->vesselState;
            }
        }

        void PushWater(float step)
        {
            if (water.empty()) { return; }
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            const JPH::Vec3 gravity = system->GetGravity();
            for (const WaterSample& sample : water)
            {
                const JPH::Vec3 centre = bodies.GetCenterOfMassPosition(sample.body);
                bodies.ApplyBuoyancyImpulse(sample.body, JPH::Vec3(centre.GetX(), sample.height, centre.GetZ()), sample.normal, sample.factor,
                    0.5f, 0.05f, JPH::Vec3::sZero(), gravity, step); // Jolt's sample's drags
            }
        }

        // The player. On the frame's own clock, not the fixed step: it is the camera, and a camera moves every frame.
        void TickPlayer(float deltaTime)
        {
            if (!playerWanted && !player) { return; }
            Join(); // a character's queries and pushes must not meet a step
            if (!playerWanted)
            {
                player.reset();
                playerEyeValid = false;
                return;
            }
            Settings& settings = GameSettings();
            PlayerSettings& tuning = settings.player;
            if (!player)
            {
                MEM_SCOPE(MemTag::Physics);
                player = std::make_unique<PlayerBody>(*system, ToJolt(playerSpawn), tuning);
                LOG_INFO(logging::LogCategory::Physics, "player: a body at {:.2f} {:.2f} {:.2f}", playerSpawn.x, playerSpawn.y, playerSpawn.z);
            }
            PlayerInput input = playerPossessed ? playerInput : PlayerInput{};
            playerInput.jump = false;
            input.move = input.move + Math::float3(tuning.testMoveX, 0.0f, tuning.testMoveZ);
            input.jump = input.jump || tuning.testJump;
            input.crouch = input.crouch || tuning.testCrouch;
            input.sprint = input.sprint || tuning.testSprint;
            tuning.testJump = false;

            float water = kNoWater;
            if (ocean && ocean->IsVisibleGame())
            {
                const OceanReadback& readback = ocean->GetReadback();
                const JPH::Vec3 feet = player->Feet();
                water = readback.HasSurface() ? readback.SampleHeight(feet.GetX(), feet.GetZ()) : ocean->GetWaterLevel();
            }
            player->Update(std::clamp(deltaTime, 1.0e-4f, 1.0f / 30.0f), input, tuning, water, *temp);
            // UE's KillZ, for the one thing nothing else stops: no ground and no water under it.
            if (player->Feet().GetY() < settings.killY)
            {
                LOG_WARNING(logging::LogCategory::Physics, "player: fell out of the world at {:.1f} {:.1f}, back at the start", player->Feet().GetX(), player->Feet().GetZ());
                player->SetFeet(ToJolt(playerSpawn));
            }
            playerEye = FromJolt(player->Eye(tuning));
            playerEyeValid = playerPossessed;
        }

        bool Cast(const Math::float3& origin, const Math::float3& unitDirection, float maxDistance, RayHit& out)
        {
            if (!system) { return false; }
            Join();
            const JPH::RRayCast ray(ToJolt(origin), ToJolt(unitDirection) * maxDistance);
            JPH::RayCastResult hit;
            if (!system->GetNarrowPhaseQuery().CastRay(ray, hit)) { return false; }
            const JPH::Vec3 point = ray.GetPointOnRay(hit.mFraction);
            out.distance = hit.mFraction * maxDistance;
            out.point = FromJolt(point);
            JPH::BodyLockRead lock(system->GetBodyLockInterface(), hit.mBodyID);
            if (!lock.Succeeded()) { return false; }
            const JPH::Body& body = lock.GetBody();
            out.normal = FromJolt(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
            auto* base = reinterpret_cast<RenderableObjectBase*>(body.GetUserData());
            const auto it = entries.find(base);
            out.object = it != entries.end() ? it->second.object : nullptr;
            return true;
        }

        void AfterStep()
        {
            ++stepIndex;
            JPH::BodyInterface& bodies = system->GetBodyInterfaceNoLock();
            activeScratch.clear();
            system->GetActiveBodies(JPH::EBodyType::RigidBody, activeScratch);
            for (const JPH::BodyID id : activeScratch)
            {
                const JPH::uint index = id.GetIndex();
                Entry* entry = index < byBodyIndex.size() ? byBodyIndex[index] : nullptr;
                if (!entry || entry->motion != Motion::Dynamic) { continue; }
                entry->previous = entry->current;
                bodies.GetPositionAndRotation(id, entry->current.position, entry->current.rotation);
                if (!entry->floats)
                {
                    const float resistance = entry->rolling >= 0.0f ? entry->rolling : batch.rollingResistance;
                    if (resistance > 0.0f) { ApplyRollingResistance(bodies, id, resistance, entry->rollingRadius, appliedGravity, batch.step, entry->velocityY); }
                }
                entry->activeStep = stepIndex;
                // UE's KillZ: nothing brings a body back from under the world, and awake it would redraw shadows and steps for ever.
                if (entry->current.position.GetY() < batch.killY)
                {
                    bodies.DeactivateBody(id);
                    if (!entry->fellOut)
                    {
                        entry->fellOut = true;
                        ++outcome.fellOut;
                        const Math::float3 p = FromJolt(entry->current.position);
                        LOG_WARNING_THROTTLED(std::chrono::seconds(2), logging::LogCategory::Physics, // the shape's key starts with the geometry: the object may be gone
                            "physics: '{}' fell out of the world at {:.1f} {:.1f}", entry->shapeKey.substr(0, entry->shapeKey.find('|')), p.x, p.z);
                    }
                }
                if (!entry->inMotion)
                {
                    entry->inMotion = true;
                    entry->awakeIndex = static_cast<std::uint32_t>(awake.size());
                    awake.push_back(entry);
                }
            }
        }

        // Why a world stays awake: every 2 s, from the list the step already read.
        void LogAwake()
        {
            if (!logging::ShouldLog(logging::LogLevel::Debug, logging::LogCategory::Physics)) { return; } // a Release session drops the line: not worth a sort of every speed
            const auto now = std::chrono::steady_clock::now();
            if (now - lastAwakeLog < std::chrono::seconds(2) || activeScratch.empty()) { return; }
            lastAwakeLog = now;
            std::vector<float> speeds;
            float spin = 0.0f;
            JPH::BodyInterface& bodies = system->GetBodyInterfaceNoLock();
            for (const JPH::BodyID id : activeScratch)
            {
                const JPH::uint index = id.GetIndex();
                const Entry* entry = index < byBodyIndex.size() ? byBodyIndex[index] : nullptr;
                if (!entry || entry->motion != Motion::Dynamic) { continue; }
                speeds.push_back(bodies.GetLinearVelocity(id).Length());
                spin = std::max(spin, bodies.GetAngularVelocity(id).Length());
            }
            if (speeds.empty()) { return; }
            std::sort(speeds.begin(), speeds.end());
            LOG_DEBUG(logging::LogCategory::Physics, "physics: {} dynamic bodies awake, speed median {:.3f} max {:.3f} m/s, spin max {:.3f} rad/s",
                speeds.size(), speeds[speeds.size() / 2], speeds.back(), spin);
        }

        void WriteOffset(Entry& entry, JPH::Vec3Arg position, JPH::QuatArg rotation)
        {
            entry.object->SetRenderOffset(entry.restInverse * RigidMatrix(position, rotation));
            entry.hasOffset = true;
        }

        static double StepSeconds() { return 1.0 / std::clamp(static_cast<double>(GameSettings().stepHz), 10.0, 480.0); }

        void PrepareBatch(int count, float step)
        {
            const Settings& settings = GameSettings();
            EnsureJobs(std::clamp(settings.threads, 0, 16));
            batch.count = count;
            batch.step = step;
            batch.rollingResistance = settings.rollingResistance;
            batch.killY = settings.killY;
            batch.delayMs = settings.stepDelayMs;
        }

        // The steps. On the step's thread, or in line under physics.async 0.
        void RunBatch()
        {
            MEM_SCOPE(MemTag::Physics);
            for (int i = 0; i < batch.count; ++i)
            {
                if (i > 0)
                {
                    PushWater(batch.step); // the first step's went in where the steps were started: it may wake a body
                    PushVessels(batch.step);
                    if (i == 1)
                    {
                        // The first step took the boats there. Left alone they would go on at that speed, N times as far: at 20 fps they left the world.
                        JPH::BodyInterface& bodies = system->GetBodyInterface();
                        for (const KinematicTarget& target : kinematicTargets) { bodies.MoveKinematic(target.body, target.position, target.rotation, batch.step); }
                    }
                }
                const auto start = std::chrono::steady_clock::now();
                {
                    CPU_SCOPE(ProfilerScopes::kPhysicsStep);
                    if (batch.delayMs > 0.0f) { std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(batch.delayMs * 1000.0f))); } // in floats it adds to the clock in floats, and returns at once
                    const JPH::EPhysicsUpdateError error = system->Update(batch.step, 1, temp.get(), jobs.get());
                    if (error != JPH::EPhysicsUpdateError::None)
                    {
                        ++outcome.errors;
                        LOG_ERROR_THROTTLED(std::chrono::seconds(5), logging::LogCategory::Physics,
                            "physics: the step ran out of room (error {}: 1 manifolds, 2 body pairs, 4 contacts): what did not fit does not collide, bodies will fall through the world", static_cast<int>(error));
                    }
                }
                outcome.stepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
                ++outcome.steps;
                {
                    CPU_SCOPE(ProfilerScopes::kPhysicsAfterStep);
                    AfterStep();
                }
                LogAwake();
            }
        }

        // The frame stands between the last two steps: without this a body moves in jumps of 16 ms at hundreds of frames a
        // second. A body that stopped being awake takes its exact pose, once, and goes back to being free.
        void Display(float alpha)
        {
            poseOwed = false;
            ShowVessels(alpha);
            if (awake.empty()) { return; }
            for (size_t i = 0; i < awake.size();)
            {
                Entry& entry = *awake[i];
                if (entry.activeStep != stepIndex || entry.fellOut) // one under the world woke up in this very step, and no step may come to say it has stopped
                {
                    WriteOffset(entry, entry.current.position, entry.current.rotation);
                    entry.inMotion = false;
                    awake[i] = awake.back();
                    awake[i]->awakeIndex = static_cast<std::uint32_t>(i);
                    awake.pop_back();
                    continue;
                }
                ++i;
            }
            const auto write = [this, alpha](size_t i)
            {
                Entry& entry = *awake[i];
                const JPH::Vec3 position = entry.previous.position + (entry.current.position - entry.previous.position) * alpha;
                WriteOffset(entry, position, entry.previous.rotation.SLERP(entry.current.rotation, alpha).Normalized());
            };
            // At the hand-over this is what the render half waits for. An object only writes itself; 3000 of them are 0.19 ms on one thread.
            constexpr size_t kBatch = 256;
            if (awake.size() < 2 * kBatch)
            {
                for (size_t i = 0; i < awake.size(); ++i) { write(i); }
                return;
            }
            TaskSystem::ParallelFor(awake.size(), write, kBatch);
        }

        // The steps Tick counted, on their thread. What went since the Tick goes first (a level load runs between them); the water was read in the Tick, and `ocean` may be gone now. A
        // body gone since then fails its lock and takes no push. No step that moves nothing starts: the pose owed then is the one the hand-over works out.
        void KickOwed()
        {
            if (stepsOwed == 0 || stepInFlight) { return; }
            const int steps = stepsOwed;
            const float step = stepOwedSeconds;
            stepsOwed = 0;
            Drain();
            if (!system || !simulating) { return; }
            const bool kinematicMoved = MoveKinematics(step);
            TakeHelmInputs();
            PushWater(step);
            PushVessels(step);
            if (!kinematicMoved && system->GetNumActiveBodies(JPH::EBodyType::RigidBody) == 0) { return; }
            PrepareBatch(steps, step);
            stepInFlight = true;
            stepThread.Kick([this]() { RunBatch(); });
        }

        void Simulate(float deltaTime)
        {
            const Settings& settings = GameSettings();
            const double stepSeconds = StepSeconds();
            const float step = static_cast<float>(stepSeconds);
            if (settings.gravity != appliedGravity)
            {
                Join();
                appliedGravity = settings.gravity;
                system->SetGravity(JPH::Vec3(0.0f, appliedGravity, 0.0f));
                WakeAllDynamic();
            }

            accumulator += deltaTime;
            if (stepsOwed > 0) { KickOwed(); } // a loop that never called KickStep
            if (stepInFlight) { return; } // longer than a frame: the time waits for it, the hand-over looks again
            int steps = static_cast<int>(accumulator / stepSeconds);
            const int maxSteps = std::max(settings.maxSteps, 1);
            if (steps > maxSteps)
            {
                LOG_DEBUG_THROTTLED(std::chrono::seconds(5), logging::LogCategory::Physics,
                    "physics: {} steps owed in one frame, {} taken, the rest of the time is dropped", steps, maxSteps);
                accumulator = maxSteps * stepSeconds;
                steps = maxSteps;
            }

            const bool beside = settings.async && threading::g_renderThreadEnabled;
            if (beside && steps > 0)
            {
                // What landed and nobody showed yet: its end, while the entries are still the game's.
                if (poseOwed) { Display(1.0f); }
                for (int i = 0; i < steps; ++i) { accumulator -= stepSeconds; } // as the step in line does: steps * stepSeconds rounds otherwise, and the frame's alpha with it
                stepsOwed = steps;
                stepOwedSeconds = step;
                poseOwed = true;
                SampleWater(static_cast<float>(steps) * step);
                if (settings.kickInTick) { KickOwed(); }
                return;
            }
            else
            {
                for (int i = 0; i < steps; ++i)
                {
                    accumulator -= stepSeconds;
                    const bool kinematicMoved = MoveKinematics(step);
                    SampleWater(step);
                    TakeHelmInputs();
                    PushWater(step);
                    PushVessels(step);
                    if (!kinematicMoved && system->GetNumActiveBodies(JPH::EBodyType::RigidBody) == 0) { continue; }
                    PrepareBatch(1, step);
                    RunBatch();
                    MergeOutcome();
                }
            }
            Display(static_cast<float>(std::clamp(accumulator / stepSeconds, 0.0, 1.0)));
        }

        void DrawDebug(const Math::float3& eye)
        {
            const Settings& settings = GameSettings();
            if (settings.debug == 0)
            {
                lines.reset();
                return;
            }
            Join();
            MEM_SCOPE(MemTag::Physics);
            if (!lines) { lines = std::make_unique<LineRenderer>(); }
            lines->Begin(Systems::GetRenderer().GetDebugDrawSystem(), eye, settings.debugRadius);
            JPH::BodyManager::DrawSettings draw;
            draw.mDrawShape = true;
            draw.mDrawShapeWireframe = true;
            NearCameraFilter filter(eye, settings.debugRadius);
            system->DrawBodies(draw, lines.get(), &filter);
        }
    };

    World::World() : impl_(std::make_unique<Impl>()) {}
    World::~World() = default;

    void World::Tick(float deltaTime)
    {
        TC_CHECK_GAME("physics::World::Tick");
        CPU_SCOPE(ProfilerScopes::kSceneTickPhysics);
        Impl& m = *impl_;
        const Settings& settings = GameSettings();
        m.stats.stepsLastFrame = 0;
        m.Collect();
        if (!settings.enabled && m.system)
        {
            // physics.enabled:0: no world at all, every object back where the level put it.
            m.SetSimulating(false);
            m.DestroySystem();
            m.worldBuiltForEnabled = false;
        }
        m.Drain();
        if (!settings.enabled || !m.system) { return; }
        m.SetSimulating(settings.simulate);
        // These two read the world, and a step about to start would make them wait for all of it.
        m.TickPlayer(deltaTime);
        m.DrawDebug(camera_);
        if (m.simulating) { m.Simulate(deltaTime); }
    }

    void World::SyncPoses()
    {
        Impl& m = *impl_;
        if (!m.poseOwed) { return; }
        CPU_SCOPE(ProfilerScopes::kPhysicsSyncPoses);
        m.KickOwed(); // a loop that never called KickStep (the scene-stress harness): the step runs beside the next frame instead, and this one holds
        if (!m.Collect())
        {
            ++m.stats.handOversHeld;
            return;
        }
        m.Drain(); // an object that went since the Tick: its entry goes before anything writes to it
        m.Display(static_cast<float>(std::clamp(m.accumulator / Impl::StepSeconds(), 0.0, 1.0)));
    }

    void World::KickStep() { impl_->KickOwed(); }

    void World::ForgetRemoved()
    {
        Impl& m = *impl_;
        m.Join();
        m.Drain();
    }

    void World::FinishStep()
    {
        impl_->KickOwed();
        impl_->Join();
    }

    void World::LoadFinished()
    {
        BOOT_SCOPE("Physics world");
        Impl& m = *impl_;
        m.Drain();
        if (!m.system) { return; }
        m.Join();
        // The player stands on the level that left: its capsule goes, and the play session says where the next one starts (AppController::OnLevelLoaded).
        m.player.reset();
        m.playerEyeValid = false;
        // What the level that left held. What stays is what a reload would build again.
        m.EvictUnusedShapes();
        if (m.layoutChanged)
        {
            m.system->OptimizeBroadPhase(); // once: Jolt forbids it every frame or while streaming
            m.layoutChanged = false;
        }
        LOG_INFO(logging::LogCategory::Physics, "physics: {} objects, {} static + {} kinematic + {} dynamic bodies; shapes: {} meshes of {} triangles, {} hulls, {} primitive; {:.1f} MB",
            m.entries.size(), m.stats.staticBodies, m.stats.kinematicBodies, m.stats.dynamicBodies, m.stats.meshShapes, m.stats.shapeTriangles, m.stats.hullShapes, m.stats.primitiveShapes,
            static_cast<double>(JoltBytes()) / (1024.0 * 1024.0));
    }

    bool World::RayCast(const Math::float3& origin, const Math::float3& unitDirection, float maxDistance, RayHit& out) const
    {
        return impl_->Cast(origin, unitDirection, maxDistance, out);
    }

    void World::SetOcean(const OceanRenderable* ocean) { impl_->ocean = ocean; }

    void World::SpawnPlayer(const Math::float3& feet)
    {
        Impl& m = *impl_;
        m.Join();
        m.player.reset();
        m.playerWanted = true;
        m.playerPossessed = true;
        m.playerSpawn = feet;
        m.playerEyeValid = false;
        m.playerInput = PlayerInput{};
    }

    void World::RemovePlayer()
    {
        Impl& m = *impl_;
        m.Join();
        m.player.reset();
        m.playerWanted = false;
        m.playerEyeValid = false;
        m.playerInput = PlayerInput{};
    }

    RenderableObject* World::FindVessel(const Math::float3& point, float reach) const
    {
        const Impl& m = *impl_;
        RenderableObject* best = nullptr;
        float bestDistance = reach;
        for (const Impl::Entry* entry : m.vessels)
        {
            // To the rectangle of its hull on the water, not to its middle: a boat is long.
            const float dx = point.x - entry->shownPosition.GetX();
            const float dz = point.z - entry->shownPosition.GetZ();
            const float fx = entry->shownForward.GetX();
            const float fz = entry->shownForward.GetZ();
            const float pastEnds = std::max(std::fabs(dx * fx + dz * fz) - entry->halfLength, 0.0f);
            const float pastSides = std::max(std::fabs(dx * fz - dz * fx) - entry->halfBeam, 0.0f);
            const float distance = std::sqrt(pastEnds * pastEnds + pastSides * pastSides);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = entry->object;
            }
        }
        return best;
    }

    bool World::SetHelm(RenderableObject* vessel)
    {
        Impl& m = *impl_;
        m.Join();
        m.helm = nullptr;
        m.helmInput = VesselInput{};
        m.stepHelmInput = VesselInput{};
        m.helmShown = HelmState{};
        if (!vessel) { return false; }
        for (Impl::Entry* entry : m.vessels)
        {
            if (entry->object != vessel) { continue; }
            m.helm = entry;
            m.ShowVessels(1.0f);
            return true;
        }
        return false;
    }

    void World::SetHelmInput(const VesselInput& input) { impl_->helmInput = input; }

    bool World::SetPilot(RenderableObject* vessel, const VesselInput* input)
    {
        // The game's own fields: no step reads them (TakeHelmInputs copies them where a step starts), so no Join.
        for (Impl::Entry* entry : impl_->vessels)
        {
            if (entry->object != vessel) { continue; }
            entry->piloted = input != nullptr;
            entry->pilotInput = input ? *input : VesselInput{};
            return true;
        }
        return false;
    }

    bool World::PlaceVessel(RenderableObject* vessel, const Math::float3& position, float heading)
    {
        Impl& m = *impl_;
        m.Join(); // the step owns the bodies and the entries' poses
        if (!m.system) { return false; }
        for (Impl::Entry* entry : m.vessels)
        {
            if (entry->object != vessel) { continue; }
            // About the vertical, by what takes the hull's own bow (its settings' forward) to the heading: a turn of t
            // takes +z to (sin t, 0, cos t).
            const JPH::Vec3 bow = ToJolt(entry->vessel->forward).Normalized();
            const JPH::Quat rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), heading - std::atan2(bow.GetX(), bow.GetZ()));
            const JPH::Vec3 at(position.x, position.y, position.z);
            JPH::BodyInterface& bodies = m.system->GetBodyInterface();
            bodies.SetPositionAndRotation(entry->body, JPH::RVec3(at), rotation, JPH::EActivation::Activate);
            bodies.SetLinearAndAngularVelocity(entry->body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            entry->previous.position = at;
            entry->previous.rotation = rotation;
            entry->current = entry->previous;
            entry->vesselState = VesselState{};
            entry->water = VesselWater{}; // measured afresh where it is now: the rates it followed were another place's
            entry->anchored = false;
            entry->moored = false; // moors again where it was put
            entry->fellOut = false;
            m.ShowVessels(1.0f);
            return true;
        }
        return false;
    }

    bool World::ToggleAnchor(RenderableObject* vessel)
    {
        Impl& m = *impl_;
        m.Join(); // the step reads the anchor
        for (Impl::Entry* entry : m.vessels)
        {
            if (entry->object != vessel || !m.system) { continue; }
            if (entry->anchored)
            {
                entry->anchored = false;
                if (entry == m.helm) { m.helmShown.anchored = false; } // the HUD before the next Display
                LOG_INFO(logging::LogCategory::Physics, "physics: '{}' weighed its anchor", vessel->GetGeometryPath());
                return false;
            }
            // Dropped straight down from the bow, where it is now: the boat lays out its scope as it drifts back.
            const JPH::BodyInterface& bodies = m.system->GetBodyInterface();
            const JPH::RVec3 bow = bodies.GetPosition(entry->body) + bodies.GetRotation(entry->body) * entry->bowLocal;
            entry->anchorPoint = JPH::Vec3(static_cast<float>(bow.GetX()), static_cast<float>(bow.GetY()), static_cast<float>(bow.GetZ()));
            entry->anchored = true;
            if (entry == m.helm) { m.helmShown.anchored = true; }
            LOG_INFO(logging::LogCategory::Physics, "physics: '{}' dropped its anchor at {:.2f} {:.2f}, {:.1f} m of slack", vessel->GetGeometryPath(),
                     entry->anchorPoint.GetX(), entry->anchorPoint.GetZ(), entry->vessel->anchorScope);
            return true;
        }
        return false;
    }

    const HelmState& World::GetHelm() const { return impl_->helmShown; }

    void World::CollectHulls(std::vector<VesselHull>& out) const
    {
        out.clear();
        for (const Impl::Entry* entry : impl_->vessels)
        {
            if (!entry->object || !entry->vessel) { continue; }
            VesselHull hull;
            hull.object = entry->object;
            hull.settings = entry->vessel.get();
            hull.position = FromJolt(entry->shownPosition);
            hull.forward = FromJolt(entry->shownForward);
            hull.up = FromJolt(entry->shownUp);
            hull.scale = entry->scale;
            hull.speed = entry->shownSpeed;
            hull.velocity = FromJolt(entry->shownVelocity);
            hull.angularVelocity = FromJolt(entry->shownAngular);
            hull.impact = entry->shownImpact;
            hull.immersion = entry->shownImmersion;
            hull.waterHeight = entry->shownWaterHeight;
            hull.atHelm = entry == impl_->helm;
            hull.anchored = entry->anchored;
            hull.piloted = entry->piloted;
            hull.halfLength = entry->halfLength;
            hull.halfBeam = entry->halfBeam;
            hull.rudderLever = entry->rudderLever;
            hull.throttle = entry->shownThrottle;
            hull.rudder = entry->shownRudder;
            out.push_back(hull);
        }
    }

    bool World::TuneVessels(const std::string& key, float value)
    {
        Impl& m = *impl_;
        for (const VesselKey& entry : VesselKeys())
        {
            if (key != entry.key) { continue; }
            m.Join();
            for (Impl::Entry* vessel : m.vessels) { vessel->vessel.get()->*(entry.member) = std::clamp(value, entry.lo, entry.hi); }
            return true;
        }
        return false;
    }

    void World::SetPlayerPossessed(bool possessed)
    {
        impl_->playerPossessed = possessed;
        impl_->playerEyeValid = impl_->playerEyeValid && possessed;
    }

    void World::SetPlayerInput(const PlayerInput& input)
    {
        const bool jump = impl_->playerInput.jump || input.jump; // a press stays until a Tick has taken it
        impl_->playerInput = input;
        impl_->playerInput.jump = jump;
    }

    bool World::PlayerEye(Math::float3& out) const
    {
        if (!impl_->playerEyeValid) { return false; }
        out = impl_->playerEye;
        return true;
    }

    PlayerState World::GetPlayerState() const
    {
        const Impl& m = *impl_;
        PlayerState state;
        if (!m.player) { return state; }
        state.exists = true;
        state.possessed = m.playerPossessed;
        state.onGround = m.player->OnGround();
        state.crouched = m.player->Crouched();
        state.mode = m.player->Mode();
        state.speed = m.player->HorizontalSpeed();
        state.feet = FromJolt(m.player->Feet());
        return state;
    }

    void World::CollectSimulatedPoses(std::vector<SimulatedPose>& out) const
    {
        Impl& m = *impl_;
        out.clear();
        if (!m.system) { return; }
        m.Join();
        const JPH::BodyInterface& bodies = m.system->GetBodyInterface();
        for (const auto& [key, entry] : m.entries)
        {
            if (entry.body.IsInvalid() || entry.motion != Motion::Dynamic || !entry.object || !entry.hasOffset || entry.fellOut) { continue; }
            JPH::Vec3 position;
            JPH::Quat rotation;
            bodies.GetPositionAndRotation(entry.body, position, rotation);
            SimulatedPose pose;
            pose.object = entry.object;
            pose.position = FromJolt(position);
            pose.rotationEulerRad = Math::mat4::EulerXYZRadFromRotation(Math::mat4::FromQuaternion(FromJolt(rotation)));
            out.push_back(pose);
        }
    }

    Stats World::GetStats() const
    {
        const Impl& m = *impl_;
        Stats stats = m.stats;
        stats.objects = static_cast<std::uint32_t>(m.entries.size());
        stats.simulating = m.simulating;
        stats.worldExists = m.system != nullptr;
        if (m.system) { stats.activeBodies = m.system->GetNumActiveBodies(JPH::EBodyType::RigidBody); }
        stats.floatingBodies = static_cast<std::uint32_t>(m.floaters.size() + m.vessels.size());
        stats.bytes = JoltBytes();
        return stats;
    }

    void PreviewCollision(const std::string& geometryPath, const CollisionSettings& settings, bool dynamicBody, CollisionPreview& out)
    {
        out = CollisionPreview{};
        const auto start = std::chrono::steady_clock::now();
        AcquireJolt();
        {
            const BuiltShape built = BuildAssetShape(geometryPath, settings, dynamicBody);
            out.kind = built.kind;
            out.sourceTriangles = built.triangles;
            out.lodUsed = built.lodUsed;
            out.lodCount = built.lodCount;
            out.buildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            out.error = built.error;
            if (built.shape && built.kind != ShapeKind::Custom) // a custom shape's outlines are the editor's own
            {
                // Only a leaf hands out triangles, and a shape off its centre is a decorator round one (a Jolt assert in Debug, "nothing solid" in Release). The body's origin is at the
                // model's: a shape sits with its centre of mass at the position it is given.
                JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
                built.shape->CollectTransformedShapes(JPH::AABox::sBiggest(), built.shape->GetCenterOfMass(), JPH::Quat::sIdentity(), JPH::Vec3::sOne(), JPH::SubShapeIDCreator(), leaves, JPH::ShapeFilter());
                constexpr int kBatch = 256;
                std::vector<JPH::Float3> batch(kBatch * 3);
                std::vector<JPH::Float3> corners;
                for (const JPH::TransformedShape& leaf : leaves.mHits)
                {
                    JPH::Shape::GetTrianglesContext context;
                    leaf.mShape->GetTrianglesStart(context, JPH::AABox::sBiggest(), leaf.mShapePositionCOM, leaf.mShapeRotation, leaf.GetShapeScale());
                    for (;;)
                    {
                        const int count = leaf.mShape->GetTrianglesNext(context, kBatch, batch.data());
                        if (count <= 0) { break; }
                        corners.insert(corners.end(), batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(count) * 3);
                    }
                }
                out.shapeTriangles = static_cast<std::uint32_t>(corners.size() / 3);
                // One triangle in `stride` of a big mesh: the preview is for the shape, not for every edge.
                constexpr std::uint32_t kMaxLines = 12000; // 4 ImGui vertices a line, and a draw list holds 65 536
                const std::uint32_t stride = std::max<std::uint32_t>(1u, out.shapeTriangles * 3u / kMaxLines);
                for (std::uint32_t t = 0; t < out.shapeTriangles; t += stride)
                {
                    const JPH::Float3* c = &corners[static_cast<size_t>(t) * 3];
                    const Math::float3 a(c[0].x, c[0].y, c[0].z), b(c[1].x, c[1].y, c[1].z), d(c[2].x, c[2].y, c[2].z);
                    out.lines.push_back(a); out.lines.push_back(b);
                    out.lines.push_back(b); out.lines.push_back(d);
                    out.lines.push_back(d); out.lines.push_back(a);
                }
            }
        }
        ReleaseJolt();
    }
} // namespace physics
