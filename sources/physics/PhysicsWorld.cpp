#include "physics/PhysicsInternal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyManager.h>

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

        std::string ShapeKeyOf(const std::string& geometry, const CollisionSettings& s, ShapeKind kind)
        {
            std::string key = geometry;
            key += '|';
            key += ShapeKindName(kind);
            key += '|';
            key += std::to_string(s.lod);
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
            bool hasOffset = false;    // this world has written its render offset
            Math::mat4 lastKinematicOffset; // what the buoyancy said at the last step
            bool kinematicValid = false;
            bool fellOut = false;      // below physics.killY: asleep where it is, and Keep leaves it alone
            bool floats = false;       // Dynamic and "buoyant": the water pushes it
            float floatFactor = 1.1f;
            float floatSpan = 0.5f;    // how far apart the water's height is sampled for its tilt, m
        };

        std::unordered_map<RenderableObjectBase*, Entry> entries;
        std::unordered_map<std::string, BuiltShape> shapes;
        std::vector<Entry*> byBodyIndex;
        std::vector<Entry*> awake;       // Dynamic bodies between waking and their last pose
        std::vector<Entry*> kinematics;
        std::vector<Entry*> floaters;
        const OceanRenderable* ocean = nullptr;
        std::unique_ptr<WalkingCharacter> character;
        Math::float3 walkIntent;
        bool walkJump = false;
        Math::float3 walkEye;
        float walkEyeFeetY = 0.0f;
        bool walkEyeValid = false;
        std::unordered_set<RenderableObjectBase*> dirty;
        std::vector<Event> eventScratch;
        JPH::BodyIDVector activeScratch;
        std::vector<JPH::BodyID> removeScratch;
        std::chrono::steady_clock::time_point lastAwakeLog{};

        std::unique_ptr<LayerTables> tables;
        std::unique_ptr<JPH::PhysicsSystem> system;
        std::unique_ptr<JPH::TempAllocatorImpl> temp;
        std::unique_ptr<JPH::JobSystemThreadPool> jobs;
        std::unique_ptr<LineRenderer> lines;
        bool joltHeld = false;
        bool worldBuiltForEnabled = false;
        bool simulating = false;
        bool layoutChanged = false;      // something new since the last OptimizeBroadPhase
        float appliedGravity = 0.0f;
        double accumulator = 0.0;
        std::uint64_t stepIndex = 0;
        Stats stats;

        ~Impl()
        {
            DestroySystem();
            if (joltHeld) { ReleaseJolt(); }
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
            constexpr JPH::uint kMaxContactConstraints = 10240;
            system->Init(kMaxBodies, kBodyMutexes, kMaxBodyPairs, kMaxContactConstraints, tables->broadPhase, *tables->objectVsBroadPhase, tables->pairs);
            appliedGravity = GameSettings().gravity;
            system->SetGravity(JPH::Vec3(0.0f, appliedGravity, 0.0f));
            temp = std::make_unique<JPH::TempAllocatorImpl>(16u * 1024u * 1024u);
            stats.worldExists = true;
            LOG_INFO(logging::LogCategory::Physics, "physics: world up, {} bodies at most", kMaxBodies);
        }

        void DestroySystem()
        {
            lines.reset();
            character.reset();
            walkEyeValid = false;
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
                jobs->SetThreadInitFunction([](int) { logging::SetCurrentThreadName("Physics"); });
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
                awake.erase(std::remove(awake.begin(), awake.end(), &entry), awake.end());
                entry.inMotion = false;
            }
            if (entry.floats)
            {
                floaters.erase(std::remove(floaters.begin(), floaters.end(), &entry), floaters.end());
                entry.floats = false;
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
                worldBuiltForEnabled = true;
                for (const auto& [key, entry] : entries) { dirty.insert(key); }
            }
            if (!dirty.empty()) { BuildDirty(); }
        }

        // The bodies of what changed. New shapes in parallel: most of a level's cost lies there.
        void BuildDirty()
        {
            BOOT_SCOPE("Physics bodies");
            MEM_SCOPE(MemTag::Physics);
            const auto start = std::chrono::steady_clock::now();
            EnsureSystem();
            JPH::BodyInterface& bodies = system->GetBodyInterface();

            struct Wanted
            {
                Entry* entry;
                ShapeKind kind;
                std::string key;
            };
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
                std::string shapeKey = ShapeKeyOf(object.GetGeometryPath(), object.GetCollisionSettings(), kind);

                const bool floats = dynamicBody && object.IsBuoyant();
                const bool same = !entry.body.IsInvalid() && entry.shapeKey == shapeKey && entry.motion == object.GetPhysicsMotion() &&
                    entry.kinematic == kinematicBody && entry.floats == floats && entry.revision == object.GetCollisionRevision() &&
                    entry.scale.x == object.GetScale().x && entry.scale.y == object.GetScale().y && entry.scale.z == object.GetScale().z;
                if (same)
                {
                    Teleport(entry);
                    continue;
                }
                Forget(entry);
                ClearOffset(entry);
                wanted.push_back(Wanted{ &entry, kind, std::move(shapeKey) });
            }
            FlushRemovals();
            dirty.clear();
            if (wanted.empty()) { return; }

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
                    SolidGeometry geometry;
                    const CollisionSettings& settings = object.GetCollisionSettings();
                    if (!ReadSolidGeometry(object.GetGeometryPath(), settings.lod, settings.excludeSlots, geometry))
                    {
                        built[i].error = "geometry not readable";
                        return;
                    }
                    built[i] = BuildShape(geometry, settings, w.kind);
                }, 1);
            for (size_t i = 0; i < toBuild.size(); ++i)
            {
                if (!built[i].error.empty())
                {
                    LOG_WARNING(logging::LogCategory::Physics, "physics: shape of '{}' ({}): {}",
                        toBuild[i]->entry->object->GetGeometryPath(), ShapeKindName(toBuild[i]->kind), built[i].error);
                }
                if (built[i].shape) { stats.shapeTriangles += built[i].triangles; }
                shapes.emplace(toBuild[i]->key, std::move(built[i]));
            }
            stats.shapes = static_cast<std::uint32_t>(shapes.size());

            // The bodies, as batches: a batch reaches the broad phase at once.
            std::vector<JPH::BodyID> staticIds;
            std::vector<JPH::BodyID> movingIds;
            staticIds.reserve(wanted.size());
            for (const Wanted& w : wanted)
            {
                Entry& entry = *w.entry;
                RenderableObject& object = *entry.object;
                const BuiltShape& built_ = shapes.find(w.key)->second;
                entry.motion = object.GetPhysicsMotion();
                entry.shapeKey = w.key;
                entry.scale = object.GetScale();
                entry.revision = object.GetCollisionRevision();
                entry.kinematic = entry.motion != Motion::Dynamic && object.IsBuoyant();
                if (!built_.shape) { continue; } // nothing solid in it: only leaves

                std::string error;
                const JPH::RefConst<JPH::Shape> scaled = ScaledInstance(*built_.shape, entry.scale, error);
                if (!scaled)
                {
                    LOG_WARNING(logging::LogCategory::Physics, "physics: scale of '{}': {}", object.GetGeometryPath(), error);
                    continue;
                }
                const Pose pose = AuthoredPose(object);
                entry.rest = pose;
                entry.previous = pose;
                entry.current = pose;
                entry.restInverse = Math::mat4::Inverse(RigidMatrix(pose.position, pose.rotation));
                entry.kinematicValid = false;

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
                }
                JPH::Body* body = bodies.CreateBody(creation);
                if (!body)
                {
                    LOG_WARNING_THROTTLED(std::chrono::seconds(5), logging::LogCategory::Physics, "physics: out of bodies");
                    continue;
                }
                entry.body = body->GetID();
                if (entry.floats) { floaters.push_back(&entry); }
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

        void SetSimulating(bool on)
        {
            if (on == simulating) { return; }
            simulating = on;
            accumulator = 0.0;
            if (!system) { return; }
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            if (on)
            {
                for (auto& [key, entry] : entries)
                {
                    if (!entry.body.IsInvalid() && entry.motion == Motion::Dynamic) { bodies.ActivateBody(entry.body); }
                }
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
            }
            stats.fellOut = 0;
            LOG_INFO(logging::LogCategory::Physics, "physics: stopped, the bodies are back where the level put them");
        }

        // The boats: their pose is the buoyancy's, the world follows so what they hit moves.
        bool MoveKinematics(float step)
        {
            bool moved = false;
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            for (Entry* entry : kinematics)
            {
                const RenderableObject& object = *entry->object;
                const Math::mat4 offset = object.HasRenderOffset() ? object.GetRenderOffset() : Math::mat4::Identity();
                if (entry->kinematicValid && std::memcmp(&offset.m, &entry->lastKinematicOffset.m, sizeof(offset.m)) == 0) { continue; }
                entry->lastKinematicOffset = offset;
                entry->kinematicValid = true;
                const Math::mat4 world = RigidMatrix(entry->rest.position, entry->rest.rotation) * offset;
                const JPH::Vec3 position(world.m._41, world.m._42, world.m._43);
                bodies.MoveKinematic(entry->body, position, ToJolt(QuatFromMatrix(world)), step);
                moved = true;
            }
            return moved;
        }

        // The bodies in the water: Jolt's impulse model over the surface under each, the plane through three heights so a slope tilts
        // it. The same surface the boats and the renderer's vertices read (OceanReadback).
        void ApplyWater(float step)
        {
            if (floaters.empty() || !ocean || !ocean->IsVisibleGame()) { return; }
            const OceanReadback& readback = ocean->GetReadback();
            const bool waves = readback.HasSurface();
            const float level = ocean->GetWaterLevel();
            const auto height = [&](float x, float z) { return waves ? readback.SampleHeight(x, z) : level; };
            JPH::BodyInterface& bodies = system->GetBodyInterface();
            const JPH::Vec3 gravity = system->GetGravity();
            for (const Entry* entry : floaters)
            {
                const JPH::Vec3 centre = bodies.GetCenterOfMassPosition(entry->body);
                const float span = entry->floatSpan;
                const float h0 = height(centre.GetX(), centre.GetZ());
                const float hx = height(centre.GetX() + span, centre.GetZ());
                const float hz = height(centre.GetX(), centre.GetZ() + span);
                const JPH::Vec3 normal = JPH::Vec3(-(hx - h0) / span, 1.0f, -(hz - h0) / span).Normalized();
                bodies.ApplyBuoyancyImpulse(entry->body, JPH::Vec3(centre.GetX(), h0, centre.GetZ()), normal, entry->floatFactor,
                    0.5f, 0.05f, JPH::Vec3::sZero(), gravity, step); // Jolt's sample's drags
            }
        }

        // physics.walk. On the frame's own clock, not the fixed step: it is the camera, and a camera moves every frame.
        void TickCharacter(float deltaTime, const Math::float3& camera)
        {
            Settings& settings = GameSettings();
            if (!settings.walk)
            {
                character.reset();
                walkEyeValid = false;
                return;
            }
            if (!character)
            {
                RayHit ground;
                const Math::float3 above(camera.x, camera.y + 0.5f, camera.z);
                if (!Cast(above, Math::float3(0.0f, -1.0f, 0.0f), 1000.0f, ground))
                {
                    LOG_WARNING(logging::LogCategory::Physics, "physics.walk: no ground under the camera, staying free");
                    settings.walk = false;
                    return;
                }
                MEM_SCOPE(MemTag::Physics);
                character = std::make_unique<WalkingCharacter>(*system, ToJolt(ground.point));
                walkEyeFeetY = ground.point.y;
                LOG_INFO(logging::LogCategory::Physics, "physics.walk: on the ground at {:.2f} {:.2f} {:.2f}", ground.point.x, ground.point.y, ground.point.z);
            }
            const float dt = std::clamp(deltaTime, 1.0e-4f, 1.0f / 30.0f);
            const JPH::Vec3 before = character->Feet();
            const bool hasWater = ocean && ocean->IsVisibleGame();
            const float level = hasWater ? ocean->GetWaterLevel() : 0.0f;
            character->Update(dt, ToJolt(walkIntent) + JPH::Vec3(settings.walkTestX, 0.0f, settings.walkTestZ), walkJump, simulating, *temp);
            walkJump = false;
            JPH::Vec3 feet = character->Feet();
            // The sea: down to the chest, then it stops as a wall does. Deeper already (the camera came down over the lagoon): only out.
            if (hasWater && level - feet.GetY() > settings.walkMaxDepth && feet.GetY() < before.GetY() - 1.0e-3f)
            {
                character->SetFeet(before);
                feet = before;
            }
            // A step up eases in for the eye, not for the capsule (down and level follow at once).
            const float tau = 0.08f;
            if (feet.GetY() > walkEyeFeetY) { walkEyeFeetY += (feet.GetY() - walkEyeFeetY) * (1.0f - std::exp(-dt / tau)); }
            else { walkEyeFeetY = feet.GetY(); }
            walkEye = Math::float3(feet.GetX(), walkEyeFeetY + settings.eyeHeight, feet.GetZ());
            walkEyeValid = true;
        }

        bool Cast(const Math::float3& origin, const Math::float3& unitDirection, float maxDistance, RayHit& out) const
        {
            if (!system) { return false; }
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
                entry->activeStep = stepIndex;
                // UE's KillZ: nothing brings a body back from under the world, and awake it would redraw shadows and steps for ever.
                if (entry->current.position.GetY() < GameSettings().killY && !entry->fellOut)
                {
                    entry->fellOut = true;
                    ++stats.fellOut;
                    bodies.DeactivateBody(id);
                    const Math::float3 p = FromJolt(entry->current.position);
                    LOG_WARNING_THROTTLED(std::chrono::seconds(2), logging::LogCategory::Physics,
                        "physics: '{}' fell out of the world at {:.1f} {:.1f}", entry->object ? entry->object->GetGeometryPath() : std::string(), p.x, p.z);
                }
                if (!entry->inMotion)
                {
                    entry->inMotion = true;
                    awake.push_back(entry);
                }
            }
        }

        // Why a world stays awake: every 2 s, from the list the step already read.
        void LogAwake()
        {
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

        void Simulate(float deltaTime)
        {
            const Settings& settings = GameSettings();
            const double stepSeconds = 1.0 / std::clamp(static_cast<double>(settings.stepHz), 10.0, 480.0);
            const float step = static_cast<float>(stepSeconds);
            if (settings.gravity != appliedGravity)
            {
                appliedGravity = settings.gravity;
                system->SetGravity(JPH::Vec3(0.0f, appliedGravity, 0.0f));
                for (auto& [key, entry] : entries)
                {
                    if (!entry.body.IsInvalid() && entry.motion == Motion::Dynamic) { system->GetBodyInterface().ActivateBody(entry.body); }
                }
            }
            stats.stepsLastFrame = 0;

            accumulator += deltaTime;
            int steps = static_cast<int>(accumulator / stepSeconds);
            const int maxSteps = std::max(settings.maxSteps, 1);
            if (steps > maxSteps)
            {
                LOG_DEBUG_THROTTLED(std::chrono::seconds(5), logging::LogCategory::Physics,
                    "physics: {} steps owed in one frame, {} taken, the rest of the time is dropped", steps, maxSteps);
                accumulator = maxSteps * stepSeconds;
                steps = maxSteps;
            }
            for (int i = 0; i < steps; ++i)
            {
                accumulator -= stepSeconds;
                const bool kinematicMoved = MoveKinematics(step);
                ApplyWater(step);
                if (!kinematicMoved && system->GetNumActiveBodies(JPH::EBodyType::RigidBody) == 0) { continue; }
                EnsureJobs(std::clamp(settings.threads, 0, 16));
                const auto start = std::chrono::steady_clock::now();
                {
                    CPU_SCOPE(ProfilerScopes::kPhysicsStep);
                    const JPH::EPhysicsUpdateError error = system->Update(step, 1, temp.get(), jobs.get());
                    if (error != JPH::EPhysicsUpdateError::None)
                    {
                        LOG_WARNING_THROTTLED(std::chrono::seconds(5), logging::LogCategory::Physics,
                            "physics: the step ran out of room (error {}): raise the world's limits", static_cast<int>(error));
                    }
                }
                stats.stepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
                stats.stepMsEma = stats.stepsTotal == 0 ? stats.stepMs : stats.stepMsEma + 0.05f * (stats.stepMs - stats.stepMsEma);
                ++stats.stepsTotal;
                ++stats.stepsLastFrame;
                AfterStep();
                LogAwake();
            }

            // The frame stands between the last two steps: without this a body moves in jumps of 16 ms at hundreds of frames a
            // second. A body that stopped being awake takes its exact pose, once, and goes back to being free.
            if (awake.empty()) { return; }
            const float alpha = static_cast<float>(std::clamp(accumulator / stepSeconds, 0.0, 1.0));
            for (size_t i = 0; i < awake.size();)
            {
                Entry& entry = *awake[i];
                if (entry.activeStep != stepIndex)
                {
                    WriteOffset(entry, entry.current.position, entry.current.rotation);
                    entry.inMotion = false;
                    awake[i] = awake.back();
                    awake.pop_back();
                    continue;
                }
                const JPH::Vec3 position = entry.previous.position + (entry.current.position - entry.previous.position) * alpha;
                WriteOffset(entry, position, entry.previous.rotation.SLERP(entry.current.rotation, alpha).Normalized());
                ++i;
            }
        }

        void DrawDebug(const Math::float3& eye)
        {
            const Settings& settings = GameSettings();
            if (settings.debug == 0)
            {
                lines.reset();
                return;
            }
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
        if (m.simulating) { m.Simulate(deltaTime); }
        else { m.stats.stepsLastFrame = 0; }
        m.TickCharacter(deltaTime, camera_);
        m.DrawDebug(camera_);
    }

    void World::LoadFinished()
    {
        BOOT_SCOPE("Physics world");
        Impl& m = *impl_;
        m.Drain();
        if (!m.system) { return; }
        // The shapes no body holds any more: the level that left. What stays is what a reload would build again.
        for (auto it = m.shapes.begin(); it != m.shapes.end();)
        {
            // An empty entry (only leaves) stays: it costs nothing, and without it the next copy reads the file to find that out again.
            if (!it->second.shape || it->second.shape->GetRefCount() > 1)
            {
                ++it;
                continue;
            }
            m.stats.shapeTriangles -= std::min<std::uint64_t>(m.stats.shapeTriangles, it->second.triangles);
            it = m.shapes.erase(it);
        }
        m.stats.shapes = static_cast<std::uint32_t>(m.shapes.size());
        if (m.layoutChanged)
        {
            m.system->OptimizeBroadPhase(); // once: Jolt forbids it every frame or while streaming
            m.layoutChanged = false;
        }
        LOG_INFO(logging::LogCategory::Physics, "physics: {} objects, {} static + {} kinematic + {} dynamic bodies, {} shapes of {} triangles",
            m.entries.size(), m.stats.staticBodies, m.stats.kinematicBodies, m.stats.dynamicBodies, m.stats.shapes, m.stats.shapeTriangles);
    }

    bool World::RayCast(const Math::float3& origin, const Math::float3& unitDirection, float maxDistance, RayHit& out) const
    {
        return impl_->Cast(origin, unitDirection, maxDistance, out);
    }

    void World::SetOcean(const OceanRenderable* ocean) { impl_->ocean = ocean; }

    void World::SetWalkIntent(const Math::float3& velocity, bool jump)
    {
        impl_->walkIntent = velocity;
        impl_->walkJump = impl_->walkJump || jump;
    }

    bool World::WalkEye(Math::float3& out) const
    {
        if (!impl_->walkEyeValid) { return false; }
        out = impl_->walkEye;
        return true;
    }

    void World::CollectSimulatedPoses(std::vector<SimulatedPose>& out) const
    {
        const Impl& m = *impl_;
        out.clear();
        if (!m.system) { return; }
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
        stats.floatingBodies = static_cast<std::uint32_t>(m.floaters.size());
        stats.walking = m.character != nullptr;
        stats.walkOnGround = m.character && m.character->OnGround();
        return stats;
    }

    void PreviewCollision(const std::string& geometryPath, const CollisionSettings& settings, bool dynamicBody, CollisionPreview& out)
    {
        out = CollisionPreview{};
        out.kind = ResolveShapeKind(settings.shape, dynamicBody);
        SolidGeometry geometry;
        const auto start = std::chrono::steady_clock::now();
        if (!ReadSolidGeometry(geometryPath, settings.lod, settings.excludeSlots, geometry))
        {
            out.error = "the geometry cannot be read";
            return;
        }
        out.sourceTriangles = static_cast<std::uint32_t>(geometry.indices.size() / 3);
        out.lodUsed = geometry.lodUsed;
        out.lodCount = geometry.lodCount;
        AcquireJolt();
        {
            const BuiltShape built = BuildShape(geometry, settings, out.kind);
            out.buildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            out.error = built.error;
            if (built.shape)
            {
                JPH::Shape::GetTrianglesContext context;
                built.shape->GetTrianglesStart(context, JPH::AABox::sBiggest(), JPH::Vec3::sZero(), JPH::Quat::sIdentity(), JPH::Vec3::sOne());
                constexpr int kBatch = 256;
                std::vector<JPH::Float3> corners(kBatch * 3);
                // One triangle in `stride` of a big mesh: the preview is for the shape, not for every edge.
                constexpr std::uint32_t kMaxLines = 60000;
                const std::uint32_t stride = std::max<std::uint32_t>(1u, out.sourceTriangles * (settings.twoSided ? 2u : 1u) * 3u / kMaxLines);
                std::uint32_t seen = 0;
                for (;;)
                {
                    const int count = built.shape->GetTrianglesNext(context, kBatch, corners.data());
                    if (count <= 0) { break; }
                    out.shapeTriangles += static_cast<std::uint32_t>(count);
                    for (int t = 0; t < count; ++t, ++seen)
                    {
                        if (seen % stride != 0) { continue; }
                        const JPH::Float3* c = &corners[static_cast<size_t>(t) * 3];
                        const Math::float3 a(c[0].x, c[0].y, c[0].z), b(c[1].x, c[1].y, c[1].z), d(c[2].x, c[2].y, c[2].z);
                        out.lines.push_back(a); out.lines.push_back(b);
                        out.lines.push_back(b); out.lines.push_back(d);
                        out.lines.push_back(d); out.lines.push_back(a);
                    }
                }
            }
        }
        ReleaseJolt();
    }
} // namespace physics
