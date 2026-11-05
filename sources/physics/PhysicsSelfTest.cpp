#include "physics/PhysicsInternal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <tuple>

#include "core/logging/Log.h"
#include "core/task/TaskSystem.h"
#include "core/threading/RenderThread.h"
#include "core/threading/ThreadRole.h"
#include "physics/CollisionJson.h"
#include "physics/PhysicsWorld.h"
#include "rendering/renderables/RenderableObject.h"
#include "third_party/json/json.hpp"

// The gates of docs/physics_plan.md section 5. Each one has an arm that MUST come out wrong -- a reversed triangle list, a coarser
// LOD, a nudged body -- so its silence means something. No window and no scene: plain Jolt worlds over the real assets.
namespace physics
{
    namespace
    {
        enum class Verdict { Pass, Fail, Skip };

        constexpr const char* kIsland = "models/atoll_island/atoll_island.mesh.bin";
        constexpr const char* kPalmJson = "models/date_palm/date_palm.mesh.json";
        constexpr const char* kPalmBin = "models/date_palm/date_palm.mesh.bin";

        struct TestWorld
        {
            LayerTables tables;
            JPH::PhysicsSystem system;
            JPH::TempAllocatorImpl temp{ 32u * 1024u * 1024u };
            JPH::JobSystemThreadPool jobs;

            explicit TestWorld(int threads)
            {
                jobs.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
                system.Init(4096, 0, 65536, 10240, tables.broadPhase, *tables.objectVsBroadPhase, tables.pairs);
                system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
            }
            void Step(int count)
            {
                for (int i = 0; i < count; ++i) { system.Update(1.0f / 60.0f, 1, &temp, &jobs); }
            }
            JPH::BodyID AddStatic(const JPH::Shape* shape)
            {
                JPH::BodyCreationSettings settings(shape, JPH::Vec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kNonMoving);
                return system.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
            }
            JPH::BodyID AddDynamic(const JPH::Shape* shape, JPH::Vec3Arg position, JPH::QuatArg rotation)
            {
                JPH::BodyCreationSettings settings(shape, position, rotation, JPH::EMotionType::Dynamic, layers::kMoving);
                return system.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
            }
        };

        JPH::RefConst<JPH::Shape> BoxOf(float half)
        {
            JPH::BoxShapeSettings settings(JPH::Vec3::sReplicate(half));
            settings.SetEmbedded();
            return settings.Create().Get();
        }

        // ------------------------------------------------------------------------------------------------------------ smoke
        Verdict GateSmoke(std::string& detail)
        {
            TestWorld world(2);
            const JPH::RefConst<JPH::Shape> floor = BoxOf(50.0f);
            JPH::BodyCreationSettings floorSettings(floor.GetPtr(), JPH::Vec3(0.0f, -50.0f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kNonMoving);
            world.system.GetBodyInterface().CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);
            JPH::SphereShapeSettings sphere(0.5f);
            sphere.SetEmbedded();
            const JPH::BodyID ball = world.AddDynamic(sphere.Create().Get().GetPtr(), JPH::Vec3(0.0f, 5.0f, 0.0f), JPH::Quat::sIdentity());
            world.Step(240);
            const float y = world.system.GetBodyInterface().GetCenterOfMassPosition(ball).GetY();
            detail = "ball at y " + std::to_string(y) + ", expected 0.5 less Jolt's 2 cm of penetration slop";
            return std::fabs(y - 0.48f) < 0.005f ? Verdict::Pass : Verdict::Fail;
        }

        // ------------------------------------------------------------------------------------------------------------- pose
        // Numbers cross as they are (docs/physics_plan.md 3.3): a body's corners through Jolt = the same corners through what the
        // renderer does with an object's S*R*T. And the triangles keep their front: a box dropped on the island stops on it, and
        // the same list reversed lets it through, because the simulation ignores back faces.
        Verdict GatePose(std::string& detail)
        {
            // An off-centre hull: the shape recentres itself on its centre of mass, the body's origin must not move with it.
            JPH::Array<JPH::Vec3> corners = { JPH::Vec3(0, 0, 0), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 2, 0), JPH::Vec3(0, 0, 3), JPH::Vec3(1, 2, 3) };
            JPH::ConvexHullShapeSettings hullSettings(corners, 0.0f);
            hullSettings.SetEmbedded();
            const JPH::RefConst<JPH::Shape> hull = hullSettings.Create().Get();

            TestWorld world(1);
            JPH::BodyInterface& bodies = world.system.GetBodyInterface();
            std::mt19937 random(1234);
            std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
            float worst = 0.0f;
            for (int i = 0; i < 64; ++i)
            {
                const Math::float3 axis = Math::float3(unit(random), unit(random), unit(random) + 1.5f).Normalized();
                const Math::quat q = Math::quat::FromAxisAngle(axis, unit(random) * 3.0f);
                const Math::float3 t(unit(random) * 100.0f, unit(random) * 20.0f, unit(random) * 100.0f);
                const Math::float3 scale(0.5f + 0.4f * unit(random), 1.0f + 0.5f * unit(random), 0.8f + 0.2f * unit(random));

                std::string error;
                const JPH::RefConst<JPH::Shape> scaled = ScaledInstance(*hull, scale, error);
                if (!scaled)
                {
                    detail = "scale refused: " + error;
                    return Verdict::Fail;
                }
                JPH::BodyCreationSettings settings(scaled.GetPtr(), ToJolt(t), ToJolt(q), JPH::EMotionType::Static, layers::kNonMoving);
                const JPH::BodyID id = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
                const JPH::RMat44 joltWorld = bodies.GetWorldTransform(id);
                const Math::mat4 engine = Math::mat4::Scaling(scale.x, scale.y, scale.z) * Math::mat4::FromQuaternion(q) * Math::mat4::Translation(t);
                for (const JPH::Vec3& corner : corners)
                {
                    const JPH::Vec3 viaJolt = joltWorld * (corner * ToJolt(scale));
                    const Math::float3 viaEngine = engine.TransformPoint(FromJolt(corner));
                    worst = std::max(worst, (viaJolt - ToJolt(viaEngine)).Length());
                }
                bodies.RemoveBody(id);
                bodies.DestroyBody(id);
            }
            detail = "worst corner " + std::to_string(worst * 1000.0f) + " mm";
            if (worst > 1.0e-3f) { return Verdict::Fail; }

            SolidGeometry island;
            if (!ReadSolidGeometry(kIsland, 0, {}, island))
            {
                detail += "; no island to test the triangles' front on";
                return Verdict::Fail;
            }
            const auto dropOn = [](const SolidGeometry& geometry, float x, float z, float& restY, Math::float3& normal, float& groundY)
            {
                const BuiltShape ground = BuildShape(geometry, CollisionSettings{}, ShapeKind::Mesh);
                if (!ground.shape) { return false; }
                TestWorld drop(2);
                drop.AddStatic(ground.shape.GetPtr());
                drop.system.OptimizeBroadPhase();
                const JPH::RRayCast ray(JPH::Vec3(x, 100.0f, z), JPH::Vec3(0.0f, -200.0f, 0.0f));
                JPH::RayCastResult hit;
                if (!drop.system.GetNarrowPhaseQuery().CastRay(ray, hit)) { return false; }
                const JPH::Vec3 point = ray.GetPointOnRay(hit.mFraction);
                groundY = point.GetY();
                {
                    JPH::BodyLockRead lock(drop.system.GetBodyLockInterface(), hit.mBodyID);
                    normal = FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
                }
                const JPH::RefConst<JPH::Shape> box = BoxOf(0.25f);
                const JPH::BodyID body = drop.AddDynamic(box.GetPtr(), JPH::Vec3(x, groundY + 2.0f, z), JPH::Quat::sIdentity());
                drop.Step(180);
                restY = drop.system.GetBodyInterface().GetCenterOfMassPosition(body).GetY();
                return true;
            };

            // A dry point of the island: the highest vertex's neighbourhood.
            float x = 0.0f, z = 0.0f, highest = -FLT_MAX;
            for (const Math::float3& p : island.positions)
            {
                if (p.y > highest)
                {
                    highest = p.y;
                    x = p.x;
                    z = p.z;
                }
            }
            x += 0.37f;
            z -= 0.21f;
            float restY = 0.0f, groundY = 0.0f;
            Math::float3 normal;
            if (!dropOn(island, x, z, restY, normal, groundY))
            {
                detail += "; the ray missed the island at its top";
                return Verdict::Fail;
            }
            SolidGeometry reversed = island;
            for (size_t i = 0; i + 2 < reversed.indices.size(); i += 3) { std::swap(reversed.indices[i + 1], reversed.indices[i + 2]); }
            float reversedY = 0.0f, reversedGround = 0.0f;
            Math::float3 reversedNormal;
            const bool reversedOk = dropOn(reversed, x, z, reversedY, reversedNormal, reversedGround);

            detail += "; island normal y " + std::to_string(normal.y) + ", box " + std::to_string(restY - groundY) + " m over the sand; reversed: box " +
                std::to_string(reversedY - reversedGround) + " m";
            if (normal.y <= 0.0f || std::fabs(restY - groundY) > 1.0f) { return Verdict::Fail; }
            // The arm that must come out wrong: a reversed list lets the box through.
            if (!reversedOk || reversedY - reversedGround > -1.0f)
            {
                detail += " -- the reversed arm did not fall through: this gate proves nothing";
                return Verdict::Fail;
            }
            return Verdict::Pass;
        }

        // -------------------------------------------------------------------------------------------------------------- ray
        // The shape is the geometry drawn: the height under a ray equals the plain walk over the triangles, within a millimetre.
        // An acceleration that changes the answer is not an acceleration.
        bool LinearRay(const SolidGeometry& g, const Math::float3& origin, const Math::float3& direction, float* outT)
        {
            float best = FLT_MAX;
            for (size_t i = 0; i + 2 < g.indices.size(); i += 3)
            {
                const Math::float3& v0 = g.positions[g.indices[i]];
                const Math::float3 e1 = g.positions[g.indices[i + 1]] - v0;
                const Math::float3 e2 = g.positions[g.indices[i + 2]] - v0;
                const Math::float3 p = direction.Cross(e2);
                const float det = e1.Dot(p);
                if (std::fabs(det) < 1.0e-8f) { continue; }
                const float inv = 1.0f / det;
                const Math::float3 s = origin - v0;
                const float u = s.Dot(p) * inv;
                if (u < 0.0f || u > 1.0f) { continue; }
                const Math::float3 q = s.Cross(e1);
                const float v = direction.Dot(q) * inv;
                if (v < 0.0f || u + v > 1.0f) { continue; }
                const float t = e2.Dot(q) * inv;
                if (t >= 0.0f && t < best) { best = t; }
            }
            *outT = best;
            return best != FLT_MAX;
        }

        Verdict GateRay(std::string& detail)
        {
#if defined(_DEBUG)
            constexpr int kRays = 150;
#else
            constexpr int kRays = 1000;
#endif
            SolidGeometry lod0, lod2;
            if (!ReadSolidGeometry(kIsland, 0, {}, lod0) || !ReadSolidGeometry(kIsland, 2, {}, lod2) || lod0.indices.empty())
            {
                detail = "no island";
                return Verdict::Skip;
            }
            const BuiltShape shape = BuildShape(lod0, CollisionSettings{}, ShapeKind::Mesh);
            const BuiltShape coarse = BuildShape(lod2, CollisionSettings{}, ShapeKind::Mesh);
            if (!shape.shape || !coarse.shape)
            {
                detail = "shape failed: " + shape.error + coarse.error;
                return Verdict::Fail;
            }
            TestWorld world(1);
            world.AddStatic(shape.shape.GetPtr());
            world.system.OptimizeBroadPhase();
            TestWorld coarseWorld(1);
            coarseWorld.AddStatic(coarse.shape.GetPtr());
            coarseWorld.system.OptimizeBroadPhase();

            std::mt19937 random(99);
            std::uniform_real_distribution<float> along(0.0f, 1.0f);
            int hits = 0, mismatches = 0, coarseOver = 0;
            float worst = 0.0f;
            double joltMs = 0.0, linearMs = 0.0;
            for (int i = 0; i < kRays; ++i)
            {
                const Math::float3 origin(shape.boundsMin.x + along(random) * (shape.boundsMax.x - shape.boundsMin.x), 200.0f,
                    shape.boundsMin.z + along(random) * (shape.boundsMax.z - shape.boundsMin.z));
                const Math::float3 down(0.0f, -1.0f, 0.0f);
                const JPH::RRayCast ray(ToJolt(origin), JPH::Vec3(0.0f, -400.0f, 0.0f));
                JPH::RayCastResult hit;
                auto t0 = std::chrono::steady_clock::now();
                const bool joltHit = world.system.GetNarrowPhaseQuery().CastRay(ray, hit);
                auto t1 = std::chrono::steady_clock::now();
                float linearT = 0.0f;
                const bool linearHit = LinearRay(lod0, origin, down, &linearT);
                auto t2 = std::chrono::steady_clock::now();
                joltMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
                linearMs += std::chrono::duration<double, std::milli>(t2 - t1).count();

                if (joltHit != linearHit)
                {
                    ++mismatches; // the edge of a triangle, a hair either way
                    continue;
                }
                if (!joltHit) { continue; }
                ++hits;
                const float joltT = hit.mFraction * 400.0f;
                worst = std::max(worst, std::fabs(joltT - linearT));
                JPH::RayCastResult coarseHit;
                if (coarseWorld.system.GetNarrowPhaseQuery().CastRay(ray, coarseHit) && std::fabs(coarseHit.mFraction * 400.0f - linearT) > 1.0e-3f)
                {
                    ++coarseOver;
                }
            }
            detail = std::to_string(hits) + " hits, worst " + std::to_string(worst * 1000.0f) + " mm, " + std::to_string(mismatches) +
                " hit/miss disagreements, LOD2 over 1 mm on " + std::to_string(coarseOver) + "; a ray " +
                std::to_string(joltMs * 1000.0 / kRays) + " us vs linear " + std::to_string(linearMs * 1000.0 / kRays) + " us";
            if (hits < kRays / 20 || worst > 1.0e-3f || mismatches > kRays / 100) { return Verdict::Fail; }
            // The arm that must come out wrong: LOD2 is up to 20 cm off.
            if (coarseOver < hits / 2)
            {
                detail += " -- LOD2 agreed too: this gate proves nothing";
                return Verdict::Fail;
            }
            return Verdict::Pass;
        }

        // ---------------------------------------------------------------------------------------------------------- foliage
        std::vector<int> FoliageSlotsOf(const std::string& meshJson, bool& hasSolidSlot)
        {
            std::vector<int> slots;
            hasSolidSlot = true;
            std::ifstream file(meshJson);
            if (!file) { return slots; }
            const nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
            if (!j.is_object() || !j.contains("windFoliage") || !j["windFoliage"].is_array()) { return slots; }
            int slot = 0;
            hasSolidSlot = false;
            for (const nlohmann::json& weight : j["windFoliage"])
            {
                if (weight.is_number() && weight.get<float>() > 0.0f) { slots.push_back(slot); }
                else { hasSolidSlot = true; }
                ++slot;
            }
            return slots;
        }

        Verdict GateFoliage(std::string& detail)
        {
            bool hasSolid = true;
            const std::vector<int> leaves = FoliageSlotsOf(kPalmJson, hasSolid);
            SolidGeometry all, solid;
            if (leaves.empty() || !ReadSolidGeometry(kPalmBin, 0, {}, all) || !ReadSolidGeometry(kPalmBin, 0, leaves, solid))
            {
                detail = "no date palm";
                return Verdict::Skip;
            }
            const BuiltShape wholeShape = BuildShape(all, CollisionSettings{}, ShapeKind::Mesh);
            const BuiltShape solidShape = BuildShape(solid, CollisionSettings{}, ShapeKind::Mesh);
            if (!wholeShape.shape || !solidShape.shape)
            {
                detail = "shape failed";
                return Verdict::Fail;
            }
            TestWorld wholeWorld(1), solidWorld(1);
            wholeWorld.AddStatic(wholeShape.shape.GetPtr());
            solidWorld.AddStatic(solidShape.shape.GetPtr());
            int wholeHits = 0, solidHits = 0;
            constexpr int kGrid = 48;
            for (int ix = 0; ix < kGrid; ++ix)
            {
                for (int iz = 0; iz < kGrid; ++iz)
                {
                    const float x = wholeShape.boundsMin.x + (ix + 0.5f) / kGrid * (wholeShape.boundsMax.x - wholeShape.boundsMin.x);
                    const float z = wholeShape.boundsMin.z + (iz + 0.5f) / kGrid * (wholeShape.boundsMax.z - wholeShape.boundsMin.z);
                    const JPH::RRayCast ray(JPH::Vec3(x, wholeShape.boundsMax.y + 1.0f, z), JPH::Vec3(0.0f, -(wholeShape.boundsMax.y - wholeShape.boundsMin.y) - 2.0f, 0.0f));
                    JPH::RayCastResult a, b;
                    if (wholeWorld.system.GetNarrowPhaseQuery().CastRay(ray, a)) { ++wholeHits; }
                    if (solidWorld.system.GetNarrowPhaseQuery().CastRay(ray, b)) { ++solidHits; }
                }
            }
            detail = std::to_string(all.indices.size() / 3) + " triangles, " + std::to_string(solid.indices.size() / 3) + " without the leaves; rays down the crown hit " +
                std::to_string(wholeHits) + " with, " + std::to_string(solidHits) + " without";
            if (solid.indices.size() >= all.indices.size() || solid.indices.empty()) { return Verdict::Fail; }
            if (solidHits * 4 > wholeHits) { return Verdict::Fail; }

            // An asset of nothing but leaves has no body at all.
            for (const std::filesystem::directory_entry& dir : std::filesystem::directory_iterator("models"))
            {
                if (!dir.is_directory()) { continue; }
                for (const std::filesystem::directory_entry& file : std::filesystem::directory_iterator(dir.path()))
                {
                    const std::string name = file.path().generic_string();
                    if (name.size() < 10 || name.compare(name.size() - 10, 10, ".mesh.json") != 0) { continue; }
                    bool anySolid = true;
                    const std::vector<int> slots = FoliageSlotsOf(name, anySolid);
                    if (anySolid || slots.empty()) { continue; }
                    const std::string bin = name.substr(0, name.size() - 5) + ".bin";
                    SolidGeometry none;
                    if (!ReadSolidGeometry(bin, 0, slots, none)) { continue; }
                    const BuiltShape shape = BuildShape(none, CollisionSettings{}, ShapeKind::Mesh);
                    detail += "; '" + file.path().filename().string() + "' is only leaves: " + (shape.shape ? "HAS a shape" : "no shape");
                    return shape.shape ? Verdict::Fail : Verdict::Pass;
                }
            }
            return Verdict::Pass;
        }

        // ------------------------------------------------------------------------------------------------------ determinism
        // The same calls in the same order on one binary give the same poses, whatever the thread count. Hashed by the bits,
        // sorted by id: the active list's order is not deterministic. A body nudged by 0.1 mm must change the hash.
        std::uint64_t RunScene(int threads, bool nudge)
        {
            SolidGeometry island;
            ReadSolidGeometry(kIsland, 0, {}, island);
            const BuiltShape ground = BuildShape(island, CollisionSettings{}, ShapeKind::Mesh);
            TestWorld world(threads);
            if (ground.shape) { world.AddStatic(ground.shape.GetPtr()); }
            else
            {
                const JPH::RefConst<JPH::Shape> floor = BoxOf(500.0f);
                JPH::BodyCreationSettings settings(floor.GetPtr(), JPH::Vec3(0.0f, -500.0f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kNonMoving);
                world.system.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
            }
            world.system.OptimizeBroadPhase();

            const JPH::RefConst<JPH::Shape> box = BoxOf(0.2f);
            std::mt19937 random(7);
            std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
            float groundY = 0.0f;
            {
                const JPH::RRayCast ray(JPH::Vec3(0.0f, 100.0f, 0.0f), JPH::Vec3(0.0f, -200.0f, 0.0f));
                JPH::RayCastResult hit;
                if (world.system.GetNarrowPhaseQuery().CastRay(ray, hit)) { groundY = ray.GetPointOnRay(hit.mFraction).GetY(); }
            }
            std::vector<JPH::BodyID> ids;
            for (int i = 0; i < 64; ++i)
            {
                const JPH::Vec3 position(unit(random) * 3.0f, groundY + 1.0f + 0.5f * static_cast<float>(i), unit(random) * 3.0f);
                ids.push_back(world.AddDynamic(box.GetPtr(), position, JPH::Quat::sRotation(JPH::Vec3::sAxisY(), unit(random))));
            }
            world.Step(10);
            if (nudge)
            {
                JPH::BodyInterface& bodies = world.system.GetBodyInterface();
                bodies.SetPosition(ids[0], bodies.GetPosition(ids[0]) + JPH::Vec3(1.0e-4f, 0.0f, 0.0f), JPH::EActivation::Activate);
            }
            world.Step(590);

            std::uint64_t hash = 1469598103934665603ull;
            const auto mix = [&hash](const void* data, size_t size)
            {
                const auto* bytes = static_cast<const unsigned char*>(data);
                for (size_t i = 0; i < size; ++i)
                {
                    hash ^= bytes[i];
                    hash *= 1099511628211ull;
                }
            };
            for (const JPH::BodyID id : ids)
            {
                JPH::Vec3 position;
                JPH::Quat rotation;
                world.system.GetBodyInterface().GetPositionAndRotation(id, position, rotation);
                const float values[7] = { position.GetX(), position.GetY(), position.GetZ(), rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW() };
                mix(values, sizeof(values));
            }
            return hash;
        }

        Verdict GateDeterminism(std::string& detail)
        {
            const std::uint64_t oneThread = RunScene(1, false);
            const std::uint64_t again = RunScene(1, false);
            const std::uint64_t fourThreads = RunScene(4, false);
            const std::uint64_t nudged = RunScene(4, true);
            char line[128];
            std::snprintf(line, sizeof(line), "1 thread %016llx, again %016llx, 4 threads %016llx, nudged %016llx",
                static_cast<unsigned long long>(oneThread), static_cast<unsigned long long>(again),
                static_cast<unsigned long long>(fourThreads), static_cast<unsigned long long>(nudged));
            detail = line;
            if (oneThread != again || oneThread != fourThreads) { return Verdict::Fail; }
            if (nudged == fourThreads)
            {
                detail += " -- the nudge changed nothing: this gate proves nothing";
                return Verdict::Fail;
            }
            return Verdict::Pass;
        }

        // ------------------------------------------------------------------------------------------------------------ scale
        Verdict GateScale(std::string& detail)
        {
            SolidGeometry geometry;
            if (!ReadSolidGeometry("models/coconut/coconut.mesh.bin", 0, {}, geometry) || geometry.indices.empty())
            {
                detail = "no coconut";
                return Verdict::Skip;
            }
            int shapes = 0;
            for (const ShapeKind kind : { ShapeKind::Mesh, ShapeKind::Hull, ShapeKind::Box, ShapeKind::Sphere, ShapeKind::Capsule })
            {
                const BuiltShape built = BuildShape(geometry, CollisionSettings{}, kind);
                if (!built.shape)
                {
                    detail = std::string(ShapeKindName(kind)) + " failed: " + built.error;
                    return Verdict::Fail;
                }
                for (const Math::float3& scale : { Math::float3(1, 1, 1), Math::float3(2, 2, 2), Math::float3(0.5f, 1.5f, 2.5f) })
                {
                    std::string error;
                    const JPH::RefConst<JPH::Shape> scaled = ScaledInstance(*built.shape, scale, error);
                    if (!scaled)
                    {
                        detail = std::string(ShapeKindName(kind)) + " refused a scale: " + error;
                        return Verdict::Fail;
                    }
                    ++shapes;
                }
            }
            detail = std::to_string(shapes) + " scaled shapes of a coconut";
            return Verdict::Pass;
        }

        // ------------------------------------------------------------------------------------------------------------- walk
        // The walk at 60 and at 300 frames a second: the same speed on the flat, a kerb climbed, a wall that stops it, a box pushed.
        Verdict GateWalk(std::string& detail)
        {
            const auto addStatic = [](TestWorld& world, JPH::Vec3Arg centre, JPH::Vec3Arg half)
            {
                JPH::BoxShapeSettings settings(half);
                settings.SetEmbedded();
                JPH::BodyCreationSettings body(settings.Create().Get().GetPtr(), centre, JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kNonMoving);
                world.system.GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::DontActivate);
            };
            for (const int fps : { 60, 300 })
            {
                TestWorld world(1);
                addStatic(world, JPH::Vec3(0.0f, -1.0f, 0.0f), JPH::Vec3(100.0f, 1.0f, 100.0f));
                PlayerSettings tuning;
                tuning.walkSpeed = 1.6f;
                PlayerBody walker(world.system, JPH::Vec3(0.0f, 0.0f, 0.0f), tuning);
                const float dt = 1.0f / fps;
                const auto walk = [&](float seconds, float along)
                {
                    PlayerInput input;
                    input.move = Math::float3(along, 0.0f, 0.0f);
                    for (int i = 0; i < static_cast<int>(seconds * fps); ++i) { walker.Update(dt, input, tuning, kNoWater, world.temp); }
                };
                walk(0.5f, 0.0f); // settles
                const float start = walker.Feet().GetX();
                walk(2.0f, 1.0f);
                const float walked = walker.Feet().GetX() - start;
                detail += std::to_string(fps) + " fps walked " + std::to_string(walked) + " m of 3.14 (3.2 less the run-up); ";
                if (std::fabs(walked - 3.14f) > 0.1f) { return Verdict::Fail; }

                // A kerb: 20 cm, in the way along z.
                addStatic(world, JPH::Vec3(walker.Feet().GetX() + 1.5f, 0.1f, 0.0f), JPH::Vec3(0.5f, 0.1f, 5.0f));
                walk(1.0f, 1.0f);
                const float onKerb = walker.Feet().GetY();
                detail += "on the kerb at y " + std::to_string(onKerb) + "; ";
                if (onKerb < 0.15f) { return Verdict::Fail; }

                // A wall: 3 m high.
                const float wallX = walker.Feet().GetX() + 2.0f;
                addStatic(world, JPH::Vec3(wallX + 0.25f, 1.5f, 0.0f), JPH::Vec3(0.25f, 1.5f, 20.0f));
                walk(4.0f, 1.0f);
                const float stopped = walker.Feet().GetX();
                detail += "wall at " + std::to_string(wallX) + " stopped it at " + std::to_string(stopped) + "; ";
                if (stopped > wallX - 0.25f || stopped < wallX - 0.45f) { return Verdict::Fail; }
            }

            // A box on the floor, pushed when it is walked at and left alone when it is not.
            for (const bool push : { false, true })
            {
                TestWorld world(1);
                JPH::BoxShapeSettings floor(JPH::Vec3(100.0f, 1.0f, 100.0f));
                floor.SetEmbedded();
                JPH::BodyCreationSettings floorBody(floor.Create().Get().GetPtr(), JPH::Vec3(0.0f, -1.0f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kNonMoving);
                world.system.GetBodyInterface().CreateAndAddBody(floorBody, JPH::EActivation::DontActivate);
                const JPH::RefConst<JPH::Shape> box = BoxOf(0.25f);
                // 20 kg: what a walker shoves. The same box at water's density is 125 kg and hardly gives.
                JPH::BodyCreationSettings crateSettings(box.GetPtr(), JPH::Vec3(1.5f, 0.25f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, layers::kMoving);
                crateSettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                crateSettings.mMassPropertiesOverride.mMass = 20.0f;
                const JPH::BodyID crate = world.system.GetBodyInterface().CreateAndAddBody(crateSettings, JPH::EActivation::Activate);
                PlayerSettings tuning;
                tuning.walkSpeed = 1.6f;
                PlayerBody walker(world.system, JPH::Vec3(0.0f, 0.0f, 0.0f), tuning);
                PlayerInput input;
                input.move = Math::float3(push ? 1.0f : 0.0f, 0.0f, 0.0f);
                for (int i = 0; i < 180; ++i)
                {
                    walker.Update(1.0f / 60.0f, input, tuning, kNoWater, world.temp);
                    world.Step(1);
                }
                const float x = world.system.GetBodyInterface().GetCenterOfMassPosition(crate).GetX();
                detail += std::string(push ? "pushed" : "not pushing") + ": the box at x " + std::to_string(x) + "; ";
                if (push ? x < 1.7f : std::fabs(x - 1.5f) > 0.02f) { return Verdict::Fail; }
            }
            return Verdict::Pass;
        }

        // ----------------------------------------------------------------------------------------------------------- player
        // The controller's movement, UE's formulas, at 60 and 300 frames a second: the run-up and the stop, the jump and what must not jump, the air, the crouch and the
        // ceiling that holds it down, the slopes, the water, a floor that moves. Each thing it allows has its other side that must be refused.
        Verdict GatePlayer(std::string& detail)
        {
            const auto addBox = [](TestWorld& world, JPH::Vec3Arg centre, JPH::Vec3Arg half, JPH::QuatArg rotation)
            {
                JPH::BoxShapeSettings settings(half);
                settings.SetEmbedded();
                JPH::BodyCreationSettings body(settings.Create().Get().GetPtr(), centre, rotation, JPH::EMotionType::Static, layers::kNonMoving);
                world.system.GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::DontActivate);
            };
            const auto addFloor = [&](TestWorld& world) { addBox(world, JPH::Vec3(0.0f, -1.0f, 0.0f), JPH::Vec3(100.0f, 1.0f, 100.0f), JPH::Quat::sIdentity()); };
            const auto fail = [&detail](const std::string& what) { detail += what; return Verdict::Fail; };
            const auto number = [](float value) { char text[32]; std::snprintf(text, sizeof(text), "%.3f", value); return std::string(text); };

            for (const int fps : { 60, 300 })
            {
                const float dt = 1.0f / fps;
                const PlayerSettings tuning;
                const float gravity = 9.81f;
                PlayerInput forward;
                forward.move = Math::float3(1.0f, 0.0f, 0.0f);
                const auto run = [&](PlayerBody& body, TestWorld& world, float seconds, const PlayerInput& input, float water = kNoWater)
                {
                    for (int i = 0; i < static_cast<int>(seconds * fps); ++i) { body.Update(dt, input, tuning, water, world.temp); }
                };
                detail += std::to_string(fps) + " fps: ";

                // The run-up takes speed / acceleration, the stop friction and braking together.
                {
                    TestWorld world(1);
                    addFloor(world);
                    PlayerBody body(world.system, JPH::Vec3::sZero(), tuning);
                    run(body, world, 0.3f, PlayerInput{});
                    float t = 0.0f;
                    while (body.HorizontalSpeed() < 0.95f * tuning.walkSpeed && t < 2.0f)
                    {
                        body.Update(dt, forward, tuning, kNoWater, world.temp);
                        t += dt;
                    }
                    const float expected = 0.95f * tuning.walkSpeed / tuning.acceleration;
                    detail += "95 % of the walk in " + number(t) + " s (" + number(expected) + "); ";
                    if (std::fabs(t - expected) > 0.05f) { return fail("the run-up is wrong"); }
                    run(body, world, 1.0f, forward);
                    if (std::fabs(body.HorizontalSpeed() - tuning.walkSpeed) > 0.05f) { return fail("the walk's speed is " + number(body.HorizontalSpeed())); }
                    PlayerInput sprint = forward;
                    sprint.sprint = true;
                    run(body, world, 1.0f, sprint);
                    if (std::fabs(body.HorizontalSpeed() - tuning.sprintSpeed) > 0.05f) { return fail("the sprint's speed is " + number(body.HorizontalSpeed())); }
                    run(body, world, 1.0f, forward);
                    if (std::fabs(body.HorizontalSpeed() - tuning.walkSpeed) > 0.05f) { return fail("back from the sprint the speed is " + number(body.HorizontalSpeed())); }
                    const float from = body.Feet().GetX();
                    run(body, world, 1.0f, PlayerInput{});
                    const float stop = body.Feet().GetX() - from;
                    detail += "stops in " + number(stop) + " m (0.20); ";
                    if (body.HorizontalSpeed() != 0.0f || std::fabs(stop - 0.20f) > 0.06f) { return fail("the stop is wrong"); }
                    // A turn at full speed goes round by friction, it does not stop and start.
                    run(body, world, 1.0f, forward);
                    PlayerInput back;
                    back.move = Math::float3(-1.0f, 0.0f, 0.0f);
                    run(body, world, 1.0f, back);
                    if (std::fabs(body.HorizontalSpeed() - tuning.walkSpeed) > 0.05f) { return fail("after the turn the speed is " + number(body.HorizontalSpeed())); }
                }

                // The jump: its height, one per press, and no more in the air.
                {
                    TestWorld world(1);
                    addFloor(world);
                    PlayerBody body(world.system, JPH::Vec3::sZero(), tuning);
                    run(body, world, 0.3f, PlayerInput{});
                    if (!body.OnGround()) { return fail("it should stand on the floor"); }
                    PlayerInput press;
                    press.jump = true;
                    const auto apex = [&](const PlayerInput& first, float seconds, bool pressAtTheTop)
                    {
                        float top = 0.0f;
                        body.Update(dt, first, tuning, kNoWater, world.temp);
                        const float upFor = tuning.jumpSpeed / gravity;
                        bool pressed = false;
                        for (float t = dt; t < seconds; t += dt)
                        {
                            const bool now = pressAtTheTop && !pressed && t > upFor;
                            pressed = pressed || now;
                            body.Update(dt, now ? press : PlayerInput{}, tuning, kNoWater, world.temp);
                            top = std::max(top, body.Feet().GetY());
                        }
                        return top;
                    };
                    const float height = apex(press, 1.2f, false);
                    const float expected = tuning.jumpSpeed * tuning.jumpSpeed / (2.0f * gravity);
                    detail += "jumps " + number(height) + " m (" + number(expected) + "); ";
                    if (std::fabs(height - expected) > 0.07f) { return fail("the jump is wrong"); }
                    if (!body.OnGround()) { return fail("it should have landed"); }
                    run(body, world, 0.3f, PlayerInput{});
                    const float again = apex(press, 1.2f, true);
                    if (again > height + 0.02f) { return fail("a press in the air jumped again, to " + number(again)); }
                    run(body, world, 0.3f, PlayerInput{});
                    const float held = apex(PlayerInput{}, 0.5f, false);
                    if (held > 0.02f) { return fail("no press, no jump: " + number(held)); }
                }

                // A press just before landing still jumps; a press long before does not.
                for (const float early : { 0.05f, 0.4f })
                {
                    TestWorld world(1);
                    addFloor(world);
                    PlayerBody body(world.system, JPH::Vec3(0.0f, 2.0f, 0.0f), tuning);
                    const float fall = std::sqrt(2.0f * 2.0f / gravity);
                    PlayerInput press;
                    press.jump = true;
                    int pressFrame = static_cast<int>((fall - early) * fps);
                    float top = 0.0f;
                    bool landed = false;
                    for (int i = 0; i < static_cast<int>((fall + 0.5f) * fps); ++i)
                    {
                        body.Update(dt, i == pressFrame ? press : PlayerInput{}, tuning, kNoWater, world.temp);
                        landed = landed || body.Feet().GetY() < 0.02f;
                        if (landed) { top = std::max(top, body.Feet().GetY()); }
                    }
                    const bool jumped = top > 0.3f;
                    if (jumped != (early < 0.1f)) { return fail("a press " + number(early) + " s before landing " + (jumped ? "jumped" : "did not jump")); }
                }

                // A press just after walking off an edge still jumps; a press long after does not.
                for (const float late : { 0.05f, 0.3f })
                {
                    TestWorld world(1);
                    addBox(world, JPH::Vec3(-10.0f, -5.0f, 0.0f), JPH::Vec3(10.0f, 5.0f, 10.0f), JPH::Quat::sIdentity());
                    PlayerBody body(world.system, JPH::Vec3(-1.0f, 0.0f, 0.0f), tuning);
                    run(body, world, 0.3f, PlayerInput{});
                    float t = 0.0f;
                    while (body.Feet().GetY() > -0.6f && t < 3.0f && body.OnGround()) // past Jolt's 50 cm of sticking
                    {
                        body.Update(dt, forward, tuning, kNoWater, world.temp);
                        t += dt;
                    }
                    if (body.OnGround()) { return fail("it should have walked off the edge"); }
                    run(body, world, late, forward);
                    const float y = body.Feet().GetY();
                    PlayerInput press = forward;
                    press.jump = true;
                    body.Update(dt, press, tuning, kNoWater, world.temp);
                    run(body, world, 0.05f, forward);
                    const bool jumped = body.Feet().GetY() > y;
                    if (jumped != (late < 0.1f)) { return fail("a press " + number(late) + " s after the edge " + (jumped ? "jumped" : "did not jump")); }
                }

                // The air: a share of the acceleration, and what it took off with stays.
                {
                    TestWorld world(1);
                    addFloor(world);
                    PlayerBody body(world.system, JPH::Vec3(0.0f, 20.0f, 0.0f), tuning);
                    run(body, world, 0.3f, forward);
                    const float expected = tuning.acceleration * tuning.airControl * 0.3f;
                    if (std::fabs(body.HorizontalSpeed() - expected) > 0.15f) { return fail("in the air, 0.3 s of the keys give " + number(body.HorizontalSpeed()) + " m/s, not " + number(expected)); }
                    run(body, world, 0.3f, PlayerInput{});
                    if (std::fabs(body.HorizontalSpeed() - expected) > 0.15f) { return fail("nothing in the air should slow it: " + number(body.HorizontalSpeed())); }
                }

                // A jump into a wall: the speed the wall took does not carry over its top.
                {
                    TestWorld world(1);
                    addFloor(world);
                    addBox(world, JPH::Vec3(3.0f, 0.25f, 0.0f), JPH::Vec3(1.0f, 0.75f, 10.0f), JPH::Quat::sIdentity()); // a top at 1.0 m: a jump of 0.9 does not clear it
                    PlayerBody body(world.system, JPH::Vec3::sZero(), tuning);
                    run(body, world, 1.0f, forward);
                    if (body.Feet().GetX() > 1.8f || body.HorizontalSpeed() > 0.2f) { return fail("the wall should have stopped it, at x " + number(body.Feet().GetX()) + ", " + number(body.HorizontalSpeed()) + " m/s"); }
                }

                // The ceiling: too low to walk under, crouched it fits, and under it nothing gets up.
                {
                    TestWorld world(1);
                    addFloor(world);
                    addBox(world, JPH::Vec3(4.0f, 2.4f, 0.0f), JPH::Vec3(2.0f, 1.0f, 10.0f), JPH::Quat::sIdentity()); // from x 2 to 6, its underside at 1.4 m
                    PlayerBody standing(world.system, JPH::Vec3::sZero(), tuning);
                    run(standing, world, 2.0f, forward);
                    if (standing.Feet().GetX() > 1.8f) { return fail("a standing capsule walked under a 1.4 m ceiling, to x " + number(standing.Feet().GetX())); }
                    PlayerBody body(world.system, JPH::Vec3::sZero(), tuning);
                    PlayerInput crouch = forward;
                    crouch.crouch = true;
                    run(body, world, 0.5f, PlayerInput{});
                    run(body, world, 0.2f, crouch);
                    if (!body.Crouched() || std::fabs(body.HorizontalSpeed() - tuning.crouchSpeed) > 0.05f) { return fail("crouched it walks at " + number(body.HorizontalSpeed())); }
                    if (body.Eye(tuning).GetY() > tuning.crouchEyeHeight + 0.2f) { return fail("the eye should come down, it is at " + number(body.Eye(tuning).GetY())); }
                    run(body, world, 1.5f, crouch);
                    if (body.Feet().GetX() < 2.5f) { return fail("crouched it should go under, it stands at x " + number(body.Feet().GetX())); }
                    run(body, world, 0.2f, forward); // let go of the key under the ceiling
                    if (!body.Crouched() || body.Feet().GetX() > 5.5f) { return fail("under the ceiling it must stay down"); }
                    run(body, world, 2.0f, forward); // and out
                    if (body.Crouched() || body.Feet().GetX() < 6.0f) { return fail("out from under it should stand up, x " + number(body.Feet().GetX())); }
                    if (std::fabs(body.Eye(tuning).GetY() - tuning.eyeHeight) > 0.02f) { return fail("the eye should be back up, at " + number(body.Eye(tuning).GetY())); }
                }

                // Stairs up to the step height, not over; a slope up to the limit, not over.
                for (const float step : { 0.3f, 0.6f })
                {
                    TestWorld world(1);
                    addFloor(world);
                    addBox(world, JPH::Vec3(12.0f, step * 0.5f, 0.0f), JPH::Vec3(10.0f, step * 0.5f, 10.0f), JPH::Quat::sIdentity()); // long enough not to walk off its far side
                    PlayerBody body(world.system, JPH::Vec3::sZero(), tuning);
                    run(body, world, 2.0f, forward);
                    const bool up = body.Feet().GetY() > step - 0.05f;
                    if (up != (step < tuning.stepHeight)) { return fail("a step of " + number(step) + " m " + (up ? "was climbed" : "was not climbed")); }
                }
                for (const float degrees : { 30.0f, 60.0f })
                {
                    TestWorld world(1);
                    addFloor(world);
                    const float radians = JPH::DegreesToRadians(degrees);
                    const JPH::Vec3 along(std::cos(radians), std::sin(radians), 0.0f);
                    const JPH::Vec3 normal(-std::sin(radians), std::cos(radians), 0.0f);
                    addBox(world, JPH::Vec3(3.0f, 0.0f, 0.0f) + 10.0f * along - 0.5f * normal, JPH::Vec3(10.0f, 0.5f, 10.0f), JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), radians));
                    PlayerBody body(world.system, JPH::Vec3::sZero(), tuning);
                    run(body, world, 3.0f, forward);
                    const bool up = body.Feet().GetY() > 1.5f;
                    if (up != (degrees < tuning.maxSlopeDeg)) { return fail("a slope of " + number(degrees) + " degrees " + (up ? "was climbed" : "was not climbed") + ", y " + number(body.Feet().GetY())); }
                    if (degrees < tuning.maxSlopeDeg && body.HorizontalSpeed() < 0.9f * tuning.walkSpeed) { return fail("a walkable slope must not eat the speed: " + number(body.HorizontalSpeed())); }
                }

                // The water: over the depth it floats, the eye just over the surface; under it the feet stay on the bottom.
                {
                    TestWorld world(1);
                    addFloor(world);
                    PlayerBody body(world.system, JPH::Vec3::sZero(), tuning);
                    run(body, world, 0.3f, PlayerInput{}, 0.5f);
                    if (body.Mode() != PlayerMode::Walking || body.Feet().GetY() > 0.02f) { return fail("in 0.5 m of water it should walk on the bottom"); }
                    const float water = 2.0f;
                    run(body, world, 4.0f, PlayerInput{}, water);
                    const float eye = body.Eye(tuning).GetY();
                    detail += "swims with the eye at " + number(eye) + "; ";
                    if (body.Mode() != PlayerMode::Swimming || eye < water || eye > water + 0.3f) { return fail("in 2 m of water it should float, the eye over the surface"); }
                    run(body, world, 2.0f, forward, water);
                    if (std::fabs(body.HorizontalSpeed() - tuning.swimSpeed) > 0.05f) { return fail("it swims at " + number(body.HorizontalSpeed())); }
                    const float y = body.Feet().GetY();
                    PlayerInput press;
                    press.jump = true;
                    body.Update(dt, press, tuning, water, world.temp);
                    run(body, world, 0.2f, PlayerInput{}, water);
                    if (body.Feet().GetY() > y + 0.15f) { return fail("a jump out of deep water"); }
                }
            }

            // A floor that moves: it rides, and takes off at the floor's speed. At the world's own 60 Hz.
            {
                TestWorld world(1);
                JPH::BoxShapeSettings platform(JPH::Vec3(20.0f, 0.5f, 20.0f));
                platform.SetEmbedded();
                JPH::BodyCreationSettings platformBody(platform.Create().Get().GetPtr(), JPH::Vec3(0.0f, -0.5f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, layers::kMoving);
                const JPH::BodyID id = world.system.GetBodyInterface().CreateAndAddBody(platformBody, JPH::EActivation::Activate);
                const PlayerSettings tuning;
                PlayerBody body(world.system, JPH::Vec3::sZero(), tuning);
                const float dt = 1.0f / 60.0f;
                const float speed = 1.0f;
                float x = 0.0f;
                const auto ride = [&](int frames, const PlayerInput& input)
                {
                    for (int i = 0; i < frames; ++i)
                    {
                        x += speed * dt;
                        world.system.GetBodyInterface().MoveKinematic(id, JPH::Vec3(x, -0.5f, 0.0f), JPH::Quat::sIdentity(), dt);
                        body.Update(dt, input, tuning, kNoWater, world.temp);
                        world.Step(1);
                    }
                };
                ride(30, PlayerInput{});
                const float before = body.Feet().GetX();
                ride(120, PlayerInput{});
                const float rode = body.Feet().GetX() - before;
                detail += "rode " + number(rode) + " m of 2 on a moving floor; ";
                if (std::fabs(rode - 2.0f) > 0.1f) { return fail("it should ride the floor"); }
                PlayerInput press;
                press.jump = true;
                ride(1, press);
                const float takeOff = body.Feet().GetX();
                ride(20, PlayerInput{});
                const float carried = body.Feet().GetX() - takeOff;
                if (body.OnGround() || std::fabs(carried - speed * 20.0f * dt) > 0.1f) { return fail("off a moving floor it should keep the floor's speed: " + number(carried)); }
            }
            return Verdict::Pass;
        }

        // ------------------------------------------------------------------------------------------------------------ water
        // Jolt's impulse over a plane: a body lighter than the water floats at it and one heavier goes down. Stable at the world's
        // 60 Hz, which a stiff spring on pontoons is not (OceanBuoyancy needs its 1/240 s).
        Verdict GateWater(std::string& detail)
        {
            const auto run = [](float factor)
            {
                TestWorld world(1);
                const JPH::RefConst<JPH::Shape> box = BoxOf(0.5f);
                JPH::BodyCreationSettings settings(box.GetPtr(), JPH::Vec3(0.0f, 2.0f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, layers::kMoving);
                settings.mAllowSleeping = false;
                settings.mLinearDamping = kFloatLinearDamping;
                settings.mAngularDamping = kFloatAngularDamping;
                JPH::BodyInterface& bodies = world.system.GetBodyInterface();
                const JPH::BodyID id = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
                float lowest = 0.0f, last = 0.0f, previous = 0.0f;
                for (int i = 0; i < 1200; ++i)
                {
                    bodies.ApplyBuoyancyImpulse(id, JPH::Vec3::sZero(), JPH::Vec3::sAxisY(), factor, 0.5f, 0.05f, JPH::Vec3::sZero(), world.system.GetGravity(), 1.0f / 60.0f);
                    world.Step(1);
                    previous = last;
                    last = bodies.GetCenterOfMassPosition(id).GetY();
                    lowest = std::min(lowest, last);
                }
                return std::make_tuple(last, std::fabs(last - previous));
            };
            const auto [floatY, floatRate] = run(1.1f);
            const auto [sinkY, sinkRate] = run(0.8f);
            detail = "factor 1.1 settles at y " + std::to_string(floatY) + " (moving " + std::to_string(floatRate * 60.0f) + " m/s); factor 0.8 at y " + std::to_string(sinkY);
            if (floatY < -0.5f || floatY > 0.5f || floatRate * 60.0f > 0.05f) { return Verdict::Fail; }
            return sinkY < -3.0f ? Verdict::Pass : Verdict::Fail; // the arm that must come out wrong
        }

        // --------------------------------------------------------------------------------------------------------- vessel
        // A boat on the water, through the functions the world calls. On still water: it floats at its draft and level; full ahead climbs over the hump and planes, too little throttle stays under
        // it; it slows by itself; a rudder turns it to its own side; a boat lying still answers no rudder and one on its propeller's wash does; going astern it turns the other way. On waves: it
        // rides a long swell up and down and along its slope; a slope pushes it; a sea at an angle yaws it; at full speed into steep waves it slams and stays on its keel. The arms that must come
        // out wrong: no rudder keeps the course, a propeller out of the water pushes nothing, nothing in the water pushes nothing, the weight up top capsizes it, a boat that cannot see the waves
        // does not ride them, no wave force pushes nothing and yaws nothing, no slam slams nothing.
        struct TestBoat
        {
            TestWorld world{ 1 };
            VesselSettings settings;
            VesselState state;
            VesselWater water;
            JPH::BodyID body;
            float factor = 1.0f;
            float time = 0.0f;
            std::function<float(float, float, float)> sea = [](float, float, float) { return 0.0f; };

            explicit TestBoat(const VesselSettings& boat, JPH::QuatArg rotation = JPH::Quat::sIdentity(), float y = 0.0f) : settings(boat)
            {
                // Flat under, the keel at the origin, its upper works up to 2.7 m: the small_yacht's size and no more of it.
                JPH::Array<JPH::Vec3> corners;
                for (const float cx : { -5.0f, 5.0f })
                {
                    for (const float cy : { 0.0f, 2.7f })
                    {
                        for (const float cz : { -1.17f, 1.17f }) { corners.push_back(JPH::Vec3(cx, cy, cz)); }
                    }
                }
                JPH::ConvexHullShapeSettings hull(corners, 0.0f);
                hull.SetEmbedded();
                const JPH::RefConst<JPH::Shape> shape = hull.Create().Get();
                std::string error;
                const JPH::RefConst<JPH::Shape> floating = MakeVesselShape(*shape, settings, Math::float3(1.0f, 1.0f, 1.0f), factor, error);
                JPH::BodyCreationSettings creation(floating.GetPtr(), JPH::Vec3(0.0f, y, 0.0f), rotation, JPH::EMotionType::Dynamic, layers::kMoving);
                SetVesselBody(creation, settings);
                body = world.system.GetBodyInterface().CreateAndAddBody(creation, JPH::EActivation::Activate);
            }
            template <typename OnStep>
            void Run(float seconds, float throttle, float rudder, OnStep&& onStep)
            {
                constexpr float kStep = 1.0f / 60.0f;
                const Math::float3 one(1.0f, 1.0f, 1.0f);
                JPH::BodyInterface& bodies = world.system.GetBodyInterface();
                const int steps = static_cast<int>(seconds * 60.0f + 0.5f);
                for (int i = 0; i < steps; ++i)
                {
                    const std::function<float(float, float)> height = [this](float x, float z) { return sea(x, z, time); };
                    MeasureVesselWater(water, height, bodies.GetPosition(body), bodies.GetCenterOfMassPosition(body), bodies.GetRotation(body), settings, one, 5.0f, 1.17f, water.valid ? kStep : 0.0f);
                    {
                        JPH::BodyLockWrite lock(world.system.GetBodyLockInterface(), body);
                        PushVessel(lock.GetBody(), settings, one, state, VesselInput{ throttle, rudder }, water, factor, world.system.GetGravity(), kStep);
                    }
                    world.Step(1);
                    time += kStep;
                    onStep();
                }
            }
            void Run(float seconds, float throttle, float rudder) { Run(seconds, throttle, rudder, []() {}); }
            JPH::Quat Rotation() const { return world.system.GetBodyInterface().GetRotation(body); }
            JPH::Vec3 Forward() const { return (Rotation() * ToJolt(settings.forward)).Normalized(); }
            JPH::Vec3 Position() const { return world.system.GetBodyInterface().GetPosition(body); }
            float UpY() const { return (Rotation() * JPH::Vec3::sAxisY()).GetY(); }
            float Speed() const { return world.system.GetBodyInterface().GetLinearVelocity(body).Dot(Forward()); }
            float HeadingDeg() const { const JPH::Vec3 f = Forward(); return JPH::RadiansToDegrees(std::atan2(f.GetX(), f.GetZ())); }
            float BowUpDeg() const { return JPH::RadiansToDegrees(std::asin(std::clamp(Forward().GetY(), -1.0f, 1.0f))); }
            float RollDeg() const { return JPH::RadiansToDegrees(std::asin(std::clamp((Rotation() * JPH::Vec3::sAxisY()).Cross(Forward()).GetY(), -1.0f, 1.0f))); }
            float YawRateDeg() const { return JPH::RadiansToDegrees(world.system.GetBodyInterface().GetAngularVelocity(body).GetY()); }
        };

        Verdict GateVessel(std::string& detail)
        {
            const VesselSettings base;
            std::string failures;
            const auto expect = [&failures](bool ok, const std::string& what) { if (!ok) { failures += " [" + what + "]"; } };
            const auto num = [](float value) { return std::to_string(value); };
            const auto turned = [](float degrees) // a heading crosses 180
            {
                while (degrees > 180.0f) { degrees -= 360.0f; }
                while (degrees < -180.0f) { degrees += 360.0f; }
                return degrees;
            };

            // ---- still water
            TestBoat cruise(base, JPH::Quat::sIdentity(), 0.5f); // dropped in
            cruise.Run(30.0f, 0.0f, 0.0f);
            const float restY = cruise.Position().GetY();
            const float restUp = cruise.UpY();
            expect(std::fabs(restY + base.draft) < 0.02f, "the draft");
            expect(restUp > 0.9995f, "level at rest");

            float humpBow = 0.0f;
            float toPlane = -1.0f;
            float ran = 0.0f;
            cruise.Run(60.0f, 1.0f, 0.0f, [&]()
            {
                ran += 1.0f / 60.0f;
                humpBow = std::max(humpBow, cruise.BowUpDeg());
                if (toPlane < 0.0f && cruise.Speed() >= base.planeSpeed) { toPlane = ran; }
            });
            const float top = cruise.Speed();
            const float wantedTop = std::sqrt(base.thrust * (1.0f + base.planingLift) / base.surgeDrag);
            const float riseOnPlane = cruise.Position().GetY() + base.draft;
            const float planeBow = cruise.BowUpDeg();
            expect(std::fabs(top - wantedTop) < 0.08f * wantedTop, "the top speed");
            expect(std::fabs(cruise.HeadingDeg() - 90.0f) < 0.5f, "no rudder keeps the course");
            expect(toPlane > 0.0f && toPlane < 20.0f, "it gets onto the plane");
            expect(planeBow > 0.5f && humpBow > planeBow + 1.0f, "the bow is highest at the hump");
            expect(riseOnPlane > 0.03f, "the speed lifts the hull");
            cruise.Run(20.0f, 0.0f, 0.0f);
            const float coasted = cruise.Speed();
            expect(coasted > 0.0f && coasted < 3.0f, "it slows by itself");

            TestBoat under(base, JPH::Quat::sIdentity(), -base.draft);
            under.Run(40.0f, 0.2f, 0.0f);
            const float underHump = under.Speed();
            expect(underHump < base.humpSpeed, "too little throttle stays under the hump");

            const auto turn = [&base, &turned](float rudder)
            {
                TestBoat boat(base, JPH::Quat::sIdentity(), -base.draft);
                boat.Run(25.0f, 1.0f, 0.0f);
                const float from = boat.HeadingDeg();
                boat.Run(4.0f, 1.0f, rudder);
                return std::make_tuple(turned(boat.HeadingDeg() - from), boat.RollDeg(), boat.UpY(), boat.Speed());
            };
            const auto [starboard, starboardRoll, starboardUp, starboardSpeed] = turn(1.0f);
            const auto [port, portRoll, portUp, portSpeed] = turn(-1.0f);
            expect(starboard > 30.0f, "hard to starboard turns to starboard");
            expect(port < -30.0f && std::fabs(starboard + port) < 0.1f * starboard, "hard to port the same way round");
            expect(starboardRoll < 0.0f && portRoll > 0.0f && starboardUp > 0.8f && portUp > 0.8f, "leans into the turn, on its keel"); // into a turn to starboard its starboard side goes down
            (void)portSpeed;

            TestBoat still(base, JPH::Quat::sIdentity(), -base.draft);
            still.Run(5.0f, 0.0f, 1.0f);
            const float stillTurn = still.HeadingDeg() - 90.0f;
            expect(std::fabs(stillTurn) < 0.5f, "a boat lying still answers no rudder");
            still.Run(3.0f, 0.2f, 1.0f);
            const float washTurn = still.HeadingDeg() - 90.0f;
            const float washSpeed = still.Speed();
            expect(washTurn > 5.0f && washSpeed < 5.0f, "on its propeller's wash it does");

            TestBoat astern(base, JPH::Quat::sIdentity(), -base.draft);
            astern.Run(30.0f, -1.0f, 0.0f);
            const float back = astern.Speed();
            const float wantedBack = -std::sqrt(base.thrust * base.reverseShare / base.surgeDrag);
            expect(std::fabs(back - wantedBack) < 0.07f * std::fabs(wantedBack), "the speed astern");
            const float fromAstern = astern.HeadingDeg();
            astern.Run(3.0f, -1.0f, 1.0f);
            const float asternTurn = turned(astern.HeadingDeg() - fromAstern);
            expect(asternTurn < -5.0f, "going astern it turns the other way");

            VesselSettings dryScrew = base;
            dryScrew.propeller.y = 2.0f;
            TestBoat dry(dryScrew, JPH::Quat::sIdentity(), -base.draft);
            dry.Run(10.0f, 1.0f, 0.0f);
            const float drySpeed = dry.Speed();
            expect(std::fabs(drySpeed) < 0.05f, "a propeller out of the water pushes nothing");

            TestBoat aloft(base, JPH::Quat::sIdentity(), 50.0f);
            aloft.world.system.SetGravity(JPH::Vec3::sZero());
            aloft.Run(2.0f, 1.0f, 1.0f);
            const float aloftSpeed = aloft.Speed();
            const float aloftTurn = aloft.YawRateDeg();
            expect(aloftSpeed == 0.0f && aloftTurn == 0.0f, "no water, no push");

            const JPH::Quat heeled = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(20.0f));
            TestBoat righting(base, heeled, -base.draft);
            righting.Run(15.0f, 0.0f, 0.0f);
            const float rightedUp = righting.UpY();
            expect(rightedUp > 0.9995f, "it rights itself from 20 degrees");
            VesselSettings topHeavy = base;
            topHeavy.centerOfMassY = 2.6f;
            topHeavy.rollStiffness = 0.0f;
            TestBoat capsized(topHeavy, heeled, -base.draft);
            capsized.Run(15.0f, 0.0f, 0.0f);
            const float capsizedUp = capsized.UpY();
            expect(capsizedUp < 0.9f, "the weight up top capsizes it");

            // ---- a long swell along the hull: 60 m, 0.6 m high either way. Deep water's speed.
            constexpr float kTwoPi = 6.2831853f;
            const auto swell = [kTwoPi](float wavelength, float amplitude, float directionX, float directionZ)
            {
                const float k = kTwoPi / wavelength;
                const float omega = std::sqrt(9.81f * k);
                return [=](float x, float z, float t) { return amplitude * std::sin(k * (x * directionX + z * directionZ) - omega * t); };
            };
            const auto ride = [&base](const std::function<float(float, float, float)>& sea, bool blind)
            {
                TestBoat boat(base, JPH::Quat::sIdentity(), -base.draft);
                if (!blind) { boat.sea = sea; }
                boat.Run(15.0f, 0.0f, 0.0f, []() {});
                float low = 1.0e6f, high = -1.0e6f, worstFollow = 0.0f, worstPitch = 0.0f;
                boat.Run(25.0f, 0.0f, 0.0f, [&]()
                {
                    const float y = boat.Position().GetY();
                    low = std::min(low, y);
                    high = std::max(high, y);
                    worstFollow = std::max(worstFollow, std::fabs(y + base.draft - boat.water.height));
                    const float slope = JPH::RadiansToDegrees(std::atan((boat.water.stations[2].height - boat.water.stations[0].height) / (boat.water.stations[2].offset - boat.water.stations[0].offset)));
                    worstPitch = std::max(worstPitch, std::fabs(boat.BowUpDeg() - slope));
                });
                return std::make_tuple(high - low, worstFollow, worstPitch);
            };
            const auto longSwell = swell(60.0f, 0.6f, 1.0f, 0.0f);
            const auto [heave, follow, pitchLag] = ride(longSwell, false);
            const auto [blindHeave, blindFollow, blindPitch] = ride(longSwell, true);
            (void)blindFollow; (void)blindPitch;
            expect(heave > 0.95f, "it rides the swell up and down");
            expect(follow < 0.25f, "and stays at its draft on it");
            expect(pitchLag < 2.5f, "and along its slope");
            expect(blindHeave < 0.05f, "a boat that cannot see the waves does not ride them");

            // ---- a swell on its beam pushes it about, and a short sea at an angle yaws it. Its hull's own drag along its tilted axes moves it too, so without the slope's force it moves less, not not at all.
            const auto drift = [&base, &swell](float waveForce, bool blind)
            {
                VesselSettings settings = base;
                settings.waveForce = waveForce;
                TestBoat boat(settings, JPH::Quat::sIdentity(), -base.draft);
                if (!blind) { boat.sea = swell(40.0f, 0.6f, 0.0f, 1.0f); }
                boat.Run(10.0f, 0.0f, 0.0f);
                float low = 1.0e6f, high = -1.0e6f;
                boat.Run(20.0f, 0.0f, 0.0f, [&]()
                {
                    const float z = boat.Position().GetZ();
                    low = std::min(low, z);
                    high = std::max(high, z);
                });
                return high - low;
            };
            const float pushed = drift(1.0f, false);
            const float unpushed = drift(0.0f, false);
            const float stillWaterDrift = drift(1.0f, true);
            expect(pushed > 1.3f * unpushed, "a slope pushes it");
            expect(stillWaterDrift < 0.001f, "still water pushes nothing");

            const auto yawed = [&base, &swell](bool blind)
            {
                TestBoat boat(base, JPH::Quat::sIdentity(), -base.draft);
                if (!blind) { boat.sea = swell(15.0f, 0.4f, 0.7071f, 0.7071f); }
                float low = 1.0e6f, high = -1.0e6f;
                boat.Run(30.0f, 0.0f, 0.0f, [&]()
                {
                    const float heading = boat.HeadingDeg();
                    low = std::min(low, heading);
                    high = std::max(high, heading);
                });
                return high - low;
            };
            const float yawSwing = yawed(false);
            const float noYawSwing = yawed(true);
            expect(yawSwing > 0.5f, "a sea at an angle yaws it");
            expect(noYawSwing < 0.01f, "still water yaws nothing");

            // ---- full speed into a steep sea, the worst of it. 30 m, 0.7 m either way.
            const auto headSea = [&base, &swell](float slam)
            {
                VesselSettings settings = base;
                settings.slam = slam;
                TestBoat boat(settings, JPH::Quat::sIdentity(), -base.draft);
                boat.sea = swell(30.0f, 0.7f, -1.0f, 0.0f);
                float lowestUp = 1.0f, bumpiest = 0.0f, biggestBow = 0.0f, speedSum = 0.0f;
                int frames = 0;
                boat.Run(40.0f, 1.0f, 0.0f, [&]()
                {
                    lowestUp = std::min(lowestUp, boat.UpY());
                    bumpiest = std::max(bumpiest, boat.state.impact);
                    biggestBow = std::max(biggestBow, std::fabs(boat.BowUpDeg()));
                    speedSum += boat.Speed();
                    ++frames;
                });
                return std::make_tuple(lowestUp, bumpiest, biggestBow, speedSum / static_cast<float>(frames));
            };
            const auto [seaUp, seaSlam, seaBow, seaSpeed] = headSea(base.slam);
            const auto [calmUp, noSlam, calmBow, calmSpeed] = headSea(0.0f);
            (void)calmUp; (void)calmBow; (void)calmSpeed;
            expect(seaUp > 0.8f && seaBow < 35.0f, "into a steep sea it stays on its keel");
            expect(seaSlam > 0.05f, "it slams");
            expect(noSlam == 0.0f, "no slam slams nothing");
            expect(seaSpeed > 6.0f && seaSpeed < top, "the sea costs it speed");

            detail = "factor " + num(cruise.factor) + ", at rest y " + num(restY) + " (draft " + num(base.draft) + "), top " + num(top) + " m/s (wanted " + num(wantedTop) + "), on the plane after " + num(toPlane) +
                " s, risen " + num(riseOnPlane) + " m, bow up " + num(planeBow) + " deg, at the hump " + num(humpBow) + ", coasted to " + num(coasted) + ", a fifth of the throttle " + num(underHump) +
                " m/s; 4 s hard over " + num(starboard) + " / " + num(port) + " deg, roll " + num(starboardRoll) + " deg, speed " + num(starboardSpeed) + "; lying still " + num(stillTurn) + " deg, on the wash " + num(washTurn) +
                " deg at " + num(washSpeed) + " m/s; astern " + num(back) + " m/s, turning " + num(asternTurn) + " deg; dry propeller " + num(drySpeed) + " m/s; righted up.y " + num(rightedUp) + ", top-heavy " + num(capsizedUp) +
                "; swell: heave " + num(heave) + " m (blind " + num(blindHeave) + "), off its draft by " + num(follow) + " m, off the slope by " + num(pitchLag) + " deg; on the beam " + num(pushed) + " / " + num(unpushed) +
                " m, at an angle " + num(yawSwing) + " / " + num(noYawSwing) + " deg; head sea: up.y " + num(seaUp) + ", bow " + num(seaBow) + " deg, slam " + num(seaSlam) + " of the weight, " + num(seaSpeed) + " m/s" +
                (failures.empty() ? "" : "; WRONG:" + failures);
            return failures.empty() ? Verdict::Pass : Verdict::Fail;
        }

        // ------------------------------------------------------------------------------------------------------- kinematic
        // A `buoyant` object that is not dynamic: a kinematic body over the triangles `Auto` gives it. Left to its shape it has no mass
        // (the arm that must come out wrong; creating it so asserts in Debug), and with one given it carries a box up as it rises.
        Verdict GateKinematic(std::string& detail)
        {
            JPH::TriangleList triangles;
            triangles.push_back(JPH::Triangle(JPH::Float3(-5.0f, 0.0f, -5.0f), JPH::Float3(-5.0f, 0.0f, 5.0f), JPH::Float3(5.0f, 0.0f, 5.0f)));
            triangles.push_back(JPH::Triangle(JPH::Float3(-5.0f, 0.0f, -5.0f), JPH::Float3(5.0f, 0.0f, 5.0f), JPH::Float3(5.0f, 0.0f, -5.0f)));
            JPH::MeshShapeSettings meshSettings(triangles);
            meshSettings.SetEmbedded();
            const JPH::RefConst<JPH::Shape> mesh = meshSettings.Create().Get();

            JPH::BodyCreationSettings settings(mesh.GetPtr(), JPH::Vec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, layers::kMoving);
            const float bareMass = settings.GetMassProperties().mMass;
            GiveKinematicMass(settings);
            const float givenMass = settings.GetMassProperties().mMass;

            TestWorld world(1);
            JPH::BodyInterface& bodies = world.system.GetBodyInterface();
            const JPH::BodyID platform = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
            const JPH::RefConst<JPH::Shape> boxShape = BoxOf(0.5f);
            const JPH::BodyID box = world.AddDynamic(boxShape.GetPtr(), JPH::Vec3(0.0f, 0.6f, 0.0f), JPH::Quat::sIdentity());
            world.Step(60); // the box settles on the platform
            for (int i = 1; i <= 120; ++i)
            {
                bodies.MoveKinematic(platform, JPH::Vec3(0.0f, static_cast<float>(i) * 0.5f / 60.0f, 0.0f), JPH::Quat::sIdentity(), 1.0f / 60.0f);
                world.Step(1);
            }
            const float platformY = bodies.GetCenterOfMassPosition(platform).GetY();
            const float boxY = bodies.GetCenterOfMassPosition(box).GetY();
            detail = "mass left to the mesh " + std::to_string(bareMass) + ", given " + std::to_string(givenMass) + "; rose 1 m: platform y " +
                std::to_string(platformY) + ", box y " + std::to_string(boxY);
            if (bareMass != 0.0f || givenMass <= 0.0f) { return Verdict::Fail; }
            return std::fabs(platformY - 1.0f) < 0.02f && std::fabs(boxY - 1.48f) < 0.03f ? Verdict::Pass : Verdict::Fail;
        }

        // ------------------------------------------------------------------------------------------------------- autoshape
        // ShapeKind::Auto's rule on the real assets (the island's triangles, a coconut's hull, a palm's LOD 2 trunk), and a trunk that keeps its
        // shape: rays through LOD 0's surface along its normal land on the shape where they land on LOD 0. The arms that must come out wrong:
        // the rule off gives the palm its LOD 0, and LOD 3 stands further from the surface than LOD 2.
        struct FitBand
        {
            int rays = 0;
            int misses = 0;
            float medianM = 0.0f;
            float p95M = 0.0f;
            float MissShare() const { return rays > 0 ? static_cast<float>(misses) / rays : 0.0f; }
        };
        struct TrunkFit
        {
            FitBand low;  // under kWalkerReach over the mesh's foot: what a person or a low shot meets
            FitBand high; // the rest: what only something falling out of the crown meets
        };
        constexpr float kWalkerReach = 3.0f;

        // Rays through LOD 0's surface along its normal, area-weighted: where the coarse shape's first hit lies against LOD 0's own (a surface can lie
        // in front of the one sampled, and then both shapes must find that one).
        TrunkFit MeasureTrunk(const SolidGeometry& truth, const JPH::Shape& fine, const JPH::Shape& coarse)
        {
            TrunkFit out;
            std::vector<float> weights;
            weights.reserve(truth.indices.size() / 3);
            float total = 0.0f;
            float foot = FLT_MAX;
            const auto cornerOf = [&truth](size_t i) { return truth.positions[truth.indices[i]]; };
            for (size_t i = 0; i + 2 < truth.indices.size(); i += 3)
            {
                total += 0.5f * (cornerOf(i + 1) - cornerOf(i)).Cross(cornerOf(i + 2) - cornerOf(i)).Length();
                weights.push_back(total);
                foot = std::min({ foot, cornerOf(i).y, cornerOf(i + 1).y, cornerOf(i + 2).y });
            }
            if (total <= 0.0f) { return out; }
            TestWorld fineWorld(1), coarseWorld(1);
            fineWorld.AddStatic(&fine);
            coarseWorld.AddStatic(&coarse);
            fineWorld.system.OptimizeBroadPhase();
            coarseWorld.system.OptimizeBroadPhase();
            std::mt19937 random(4242);
            std::uniform_real_distribution<float> pick(0.0f, total);
            constexpr float kReach = 0.5f; // a trunk is thinner than this: the first hit along the ray is on the side it starts on
            std::vector<float> offLow, offHigh;
            for (int r = 0; r < 1500; ++r)
            {
                const size_t tri = static_cast<size_t>(std::lower_bound(weights.begin(), weights.end(), pick(random)) - weights.begin());
                const Math::float3 a = cornerOf(tri * 3), b = cornerOf(tri * 3 + 1), c = cornerOf(tri * 3 + 2);
                const Math::float3 cross = (b - a).Cross(c - a);
                if (cross.Length() < 1.0e-9f) { continue; }
                const Math::float3 normal = cross.Normalized();
                const Math::float3 centre = (a + b + c) * (1.0f / 3.0f);
                const JPH::RRayCast ray(ToJolt(centre + normal * kReach), ToJolt(normal * (-2.0f * kReach)));
                JPH::RayCastResult fineHit, coarseHit;
                if (!fineWorld.system.GetNarrowPhaseQuery().CastRay(ray, fineHit)) { continue; }
                const bool low = centre.y - foot < kWalkerReach;
                FitBand& band = low ? out.low : out.high;
                ++band.rays;
                if (!coarseWorld.system.GetNarrowPhaseQuery().CastRay(ray, coarseHit))
                {
                    ++band.misses;
                    continue;
                }
                (low ? offLow : offHigh).push_back(std::fabs(coarseHit.mFraction - fineHit.mFraction) * 2.0f * kReach);
            }
            const auto finish = [](std::vector<float>& off, FitBand& band)
            {
                if (off.empty()) { return; }
                std::sort(off.begin(), off.end());
                band.medianM = off[off.size() / 2];
                band.p95M = off[off.size() * 95 / 100];
            };
            finish(offLow, out.low);
            finish(offHigh, out.high);
            return out;
        }

        Verdict GateAutoShape(std::string& detail)
        {
            Settings& knobs = GameSettings();
            const Settings saved = knobs;
            knobs.autoShapes = true;
            knobs.forceLod = -1;
            const BuiltShape island = BuildAssetShape(kIsland, CollisionSettings{}, false);
            const BuiltShape coconut = BuildAssetShape("models/coconut/coconut.mesh.bin", CollisionSettings{}, false);
            if (!island.shape || !coconut.shape)
            {
                knobs = saved;
                detail = "no island or no coconut";
                return Verdict::Skip;
            }
            CollisionSettings foliageAsset;
            bool hasSolid = false;
            foliageAsset.excludeSlots = FoliageSlotsOf(kPalmJson, hasSolid);
            const BuiltShape palm = BuildAssetShape(kPalmBin, foliageAsset, false);
            const BuiltShape palmDynamic = BuildAssetShape(kPalmBin, foliageAsset, true);
            knobs.autoShapes = false;
            const BuiltShape palmRuleOff = BuildAssetShape(kPalmBin, foliageAsset, false);
            const BuiltShape coconutRuleOff = BuildAssetShape("models/coconut/coconut.mesh.bin", CollisionSettings{}, false);
            knobs = saved;
            detail = "island " + std::string(ShapeKindName(island.kind)) + " LOD " + std::to_string(island.lodUsed) + " " + std::to_string(island.triangles) + "; coconut " + ShapeKindName(coconut.kind) +
                "; palm " + ShapeKindName(palm.kind) + " LOD " + std::to_string(palm.lodUsed) + " " + std::to_string(palm.triangles) + ", rule off LOD " + std::to_string(palmRuleOff.lodUsed) + " " +
                std::to_string(palmRuleOff.triangles);
            if (island.kind != ShapeKind::Mesh || island.lodUsed != 0 || coconut.kind != ShapeKind::Hull || palm.kind != ShapeKind::Mesh || palm.lodUsed != static_cast<std::uint32_t>(kAutoFoliageLod) ||
                palmDynamic.kind != ShapeKind::Hull)
            {
                return Verdict::Fail;
            }
            // The arms: the rule off is the triangles of LOD 0, everywhere.
            if (palmRuleOff.lodUsed != 0 || palmRuleOff.triangles <= palm.triangles * 4 || coconutRuleOff.kind != ShapeKind::Mesh)
            {
                detail += " -- the rule off did not change them: this gate proves nothing";
                return Verdict::Fail;
            }

            // What the hull's tolerance gives: the faces every contact walks, and how much of the volume it keeps.
            {
                const float defaultTolerance = knobs.hullTolerance;
                std::uint32_t capFaces = 0, defaultFaces = 0;
                float defaultVolume = 0.0f;
                for (const char* name : { "models/coconut/coconut", "models/rock_boulder_game_ready_asset/rock_boulder_game_ready_asset", "models/fishing_boat/fishing_boat" })
                {
                    SolidGeometry g;
                    if (!ReadSolidGeometry(std::string(name) + ".mesh.bin", 0, {}, g)) { continue; }
                    float volume0 = 0.0f;
                    for (const float share : { 0.0f, 0.005f, 0.01f, 0.02f, 0.04f, 0.08f })
                    {
                        knobs.hullTolerance = share;
                        const BuiltShape hull = BuildShape(g, CollisionSettings{}, ShapeKind::Hull);
                        if (!hull.shape) { continue; }
                        const float volume = hull.shape->GetVolume();
                        const std::uint32_t faces = hull.shape->GetStats().mNumTriangles;
                        if (share == 0.0f) { volume0 = volume; }
                        LOG_INFO(logging::LogCategory::Physics, "physics gate: autoshape hull {} (smallest size {:.2f} m) tolerance {:.3f}: {} tris, {:.1f} % of the volume at 1 mm",
                            std::filesystem::path(name).filename().string(), hull.smallestExtent, share, faces, volume0 > 0.0f ? 100.0f * volume / volume0 : 0.0f);
                        if (std::string(name) == "models/coconut/coconut")
                        {
                            if (share == 0.0f) { capFaces = faces; }
                            if (share == defaultTolerance)
                            {
                                defaultFaces = faces;
                                defaultVolume = volume0 > 0.0f ? volume / volume0 : 0.0f;
                            }
                        }
                    }
                }
                knobs.hullTolerance = defaultTolerance;
                detail += "; coconut hull " + std::to_string(defaultFaces) + " faces, " + std::to_string(defaultVolume * 100.0f) + " % of the volume, at 1 mm " + std::to_string(capFaces) + " faces";
                // The arm: without the tolerance a round asset takes Jolt's cap of 256 points (2 x 256 - 4 faces).
                if (capFaces < 400 || defaultFaces * 2 > capFaces || defaultVolume < 0.9f)
                {
                    detail += " -- the tolerance did not do what it is for";
                    return Verdict::Fail;
                }
            }

            // The trunks: slot 0 of all three palms. An asset's LOD 2 thins or drops the crown's small wood (mesh.json lod2DropSlots: frond boots, the nuts on
            // the tree), so the trunk alone must stand where LOD 0 has it, at every height; the whole wood is printed, a falling body passes what went.
            bool fits = true;
            float trunkMissLod2 = 0.0f, trunkMissLod3 = 0.0f;
            for (const char* name : { "models/date_palm/date_palm", "models/coconut_palm/coconut_palm", "models/curly_palm/curly_palm", "models/young_palm/young_palm" })
            {
                const std::string base(name);
                bool any = false;
                const std::vector<int> leaves = FoliageSlotsOf(base + ".mesh.json", any);
                const std::vector<int> notTrunk = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
                SolidGeometry wood, trunk, trunk2, trunk3, lod1, lod2;
                const std::string bin = base + ".mesh.bin";
                if (!ReadSolidGeometry(bin, 0, leaves, wood) || !ReadSolidGeometry(bin, 0, notTrunk, trunk) || !ReadSolidGeometry(bin, kAutoFoliageLod, notTrunk, trunk2) ||
                    !ReadSolidGeometry(bin, 3, notTrunk, trunk3) || !ReadSolidGeometry(bin, 1, leaves, lod1) || !ReadSolidGeometry(bin, kAutoFoliageLod, leaves, lod2))
                {
                    detail += "; " + base + " missing";
                    return Verdict::Skip;
                }
                const BuiltShape woodShape = BuildShape(wood, CollisionSettings{}, ShapeKind::Mesh);
                const BuiltShape trunkShape = BuildShape(trunk, CollisionSettings{}, ShapeKind::Mesh);
                const BuiltShape trunk2Shape = BuildShape(trunk2, CollisionSettings{}, ShapeKind::Mesh);
                const BuiltShape trunk3Shape = BuildShape(trunk3, CollisionSettings{}, ShapeKind::Mesh);
                const BuiltShape shape1 = BuildShape(lod1, CollisionSettings{}, ShapeKind::Mesh);
                const BuiltShape shape2 = BuildShape(lod2, CollisionSettings{}, ShapeKind::Mesh);
                if (!woodShape.shape || !trunkShape.shape || !trunk2Shape.shape || !trunk3Shape.shape || !shape1.shape || !shape2.shape)
                {
                    detail += "; " + base + " shape failed";
                    return Verdict::Fail;
                }
                const auto report = [&](const char* label, const SolidGeometry& truth, const BuiltShape& fine, const BuiltShape& coarse)
                {
                    const TrunkFit fit = MeasureTrunk(truth, *fine.shape, *coarse.shape);
                    LOG_INFO(logging::LogCategory::Physics, "physics gate: autoshape {} {} ({} of {} tris): under {} m miss {:.1f} % median {:.1f} p95 {:.1f} cm ({} rays); over it miss {:.1f} % median {:.1f} p95 {:.1f} cm ({} rays)",
                        std::filesystem::path(base).filename().string(), label, coarse.triangles, fine.triangles, kWalkerReach, fit.low.MissShare() * 100.0f, fit.low.medianM * 100.0f, fit.low.p95M * 100.0f, fit.low.rays,
                        fit.high.MissShare() * 100.0f, fit.high.medianM * 100.0f, fit.high.p95M * 100.0f, fit.high.rays);
                    return fit;
                };
                report("LOD 1 vs all its wood", wood, woodShape, shape1);
                report("LOD 2 vs all its wood", wood, woodShape, shape2);
                const TrunkFit fit2 = report("LOD 2 vs its trunk", trunk, trunkShape, trunk2Shape);
                const TrunkFit fit3 = report("LOD 3 vs its trunk", trunk, trunkShape, trunk3Shape);
                // What the asset really collides with, when tools/collision_fit.py has written it: the trunk's cylinders stand where LOD 0's trunk does.
                {
                    std::ifstream manifestFile(base + ".mesh.json");
                    const nlohmann::json manifest = nlohmann::json::parse(manifestFile, nullptr, false);
                    CollisionSettings fitted;
                    if (manifest.is_object() && manifest.contains("collision") && ReadCollisionSettings(manifest["collision"], fitted, nullptr) && fitted.shape == ShapeKind::Custom)
                    {
                        const BuiltShape cylinders = BuildAssetShape("", fitted, false);
                        if (!cylinders.shape)
                        {
                            detail += "; " + base + " cylinders failed: " + cylinders.error;
                            return Verdict::Fail;
                        }
                        const TrunkFit fit = report("its cylinders vs its trunk", trunk, trunkShape, cylinders);
                        constexpr float kMaxCylinderMiss = 0.05f;
                        constexpr float kMaxCylinderMedian = 0.035f;
                        if (fit.low.MissShare() > kMaxCylinderMiss || fit.high.MissShare() > kMaxCylinderMiss || fit.low.medianM > kMaxCylinderMedian || fit.high.medianM > kMaxCylinderMedian)
                        {
                            detail += "; " + std::filesystem::path(base).filename().string() + "'s cylinders do not stand on its trunk";
                            fits = false;
                        }
                    }
                }
                const TrunkFit self = MeasureTrunk(trunk, *trunkShape.shape, *trunkShape.shape);
                if (self.low.misses + self.high.misses != 0 || self.low.medianM + self.high.medianM != 0.0f)
                {
                    detail += "; LOD 0 against itself differs";
                    fits = false;
                }
                constexpr float kMaxMiss = 0.03f;
                constexpr float kMaxMedian = 0.03f;
                if (fit2.low.MissShare() > kMaxMiss || fit2.high.MissShare() > kMaxMiss || fit2.low.medianM > kMaxMedian || fit2.high.medianM > kMaxMedian)
                {
                    detail += "; " + std::filesystem::path(base).filename().string() + " LOD 2 lost its trunk";
                    fits = false;
                }
                trunkMissLod2 += fit2.low.MissShare() + fit2.high.MissShare();
                trunkMissLod3 += fit3.low.MissShare() + fit3.high.MissShare();
            }
            // The arm: the measure sees a coarser trunk.
            if (trunkMissLod3 <= trunkMissLod2)
            {
                detail += " -- LOD 3 misses no more than LOD 2: this measure proves nothing";
                fits = false;
            }
            return fits ? Verdict::Pass : Verdict::Fail;
        }

        // ---------------------------------------------------------------------------------------------------------- custom
        // A list of boxes, spheres and capsules: from its JSON and back, where the rays say it stands, what lands on it, and doubled by an object's
        // scale. The arms that must come out wrong: a capsule that ignored its rotation stands 1 m higher, and a ray between the parts must miss.
        Verdict GateCustom(std::string& detail)
        {
            const nlohmann::json block = nlohmann::json::parse(R"({"shape":"custom","shapes":[
                {"type":"box","center":[0,1,0],"halfExtents":[1,1,1]},
                {"type":"sphere","center":[5,1,0],"radius":1},
                {"type":"capsule","center":[-5,2,0],"rotationDeg":[0,0,90],"radius":0.5,"halfHeight":1},
                {"type":"box","center":[0,1,10],"rotationDeg":[10,20,30],"halfExtents":[1,0.25,0.5]},
                {"type":"cylinder","center":[0,1,-8],"rotationDeg":[90,0,0],"radius":0.25,"halfHeight":2}]})");
            CollisionSettings settings;
            if (!ReadCollisionSettings(block, settings, nullptr) || settings.shape != ShapeKind::Custom || settings.primitives.size() != 5)
            {
                detail = "the block did not read";
                return Verdict::Fail;
            }
            if (WriteCollisionSettings(settings, false) != block)
            {
                detail = "the block did not come back: " + WriteCollisionSettings(settings, false).dump();
                return Verdict::Fail;
            }
            CollisionSettings empty;
            empty.shape = ShapeKind::Custom;
            if (BuildAssetShape("", empty, false).shape)
            {
                detail = "an empty list made a shape";
                return Verdict::Fail;
            }
            const BuiltShape built = BuildAssetShape("", settings, false);
            if (!built.shape || built.kind != ShapeKind::Custom)
            {
                detail = "shape failed: " + built.error;
                return Verdict::Fail;
            }
            std::string error;
            const JPH::RefConst<JPH::Shape> doubled = ScaledInstance(*built.shape, Math::float3(2.0f, 2.0f, 2.0f), error);
            if (!doubled)
            {
                detail = "scale refused: " + error;
                return Verdict::Fail;
            }

            TestWorld world(2);
            world.AddStatic(built.shape.GetPtr());
            TestWorld worldDoubled(1);
            worldDoubled.AddStatic(doubled.GetPtr());
            world.system.OptimizeBroadPhase();
            worldDoubled.system.OptimizeBroadPhase();
            const auto heightAt = [](TestWorld& w, float x, float z, bool& hit)
            {
                const JPH::RRayCast ray(JPH::Vec3(x, 100.0f, z), JPH::Vec3(0.0f, -200.0f, 0.0f));
                JPH::RayCastResult result;
                hit = w.system.GetNarrowPhaseQuery().CastRay(ray, result);
                return 100.0f - result.mFraction * 200.0f;
            };
            bool hitBox, hitSphere, hitCapsule, hitCapsuleEnd, hitBetween, hitDoubled;
            const float boxTop = heightAt(world, 0.0f, 0.0f, hitBox);
            const float sphereTop = heightAt(world, 5.0f, 0.0f, hitSphere);
            const float capsuleTop = heightAt(world, -5.0f, 0.0f, hitCapsule);
            const float capsuleEnd = heightAt(world, -6.2f, 0.0f, hitCapsuleEnd); // only a capsule lying along x reaches here
            heightAt(world, 2.5f, 0.0f, hitBetween);
            const float doubledTop = heightAt(worldDoubled, 0.0f, 0.0f, hitDoubled);
            detail = "tops box " + std::to_string(boxTop) + " sphere " + std::to_string(sphereTop) + " capsule " + std::to_string(capsuleTop) + "; doubled box " + std::to_string(doubledTop);
            // The cylinder lies along z from -10 to -6. Its flat end is where a capsule's cap would still go on, and x = 0.3 is past its radius.
            bool hitCylinder, hitCylinderEnd, hitPastEnd, hitBeside;
            const float cylinderTop = heightAt(world, 0.0f, -8.0f, hitCylinder);
            const float cylinderEnd = heightAt(world, 0.0f, -6.02f, hitCylinderEnd);
            heightAt(world, 0.0f, -5.98f, hitPastEnd);
            heightAt(world, 0.3f, -8.0f, hitBeside);
            detail += "; cylinder top " + std::to_string(cylinderTop) + ", at its end " + std::to_string(cylinderEnd);
            if (!hitCylinder || !hitCylinderEnd || hitPastEnd || hitBeside || std::fabs(cylinderTop - 1.25f) > 1.0e-3f || std::fabs(cylinderEnd - 1.25f) > 2.0e-3f) { return Verdict::Fail; }
            if (!hitBox || !hitSphere || !hitCapsule || !hitCapsuleEnd || hitBetween || !hitDoubled || std::fabs(boxTop - 2.0f) > 1.0e-3f || std::fabs(sphereTop - 2.0f) > 1.0e-3f ||
                std::fabs(capsuleTop - 2.5f) > 1.0e-3f || std::fabs(doubledTop - 4.0f) > 1.0e-3f)
            {
                return Verdict::Fail;
            }

            // A tilted box stands where the engine's own matrix puts it (RotationFromEulerXYZRad, as a level object's rotationDeg): along its face's normal
            // from outside, a ray lands on the face's centre. Alone (a RotatedTranslated) and inside the compound.
            CollisionSettings alone;
            alone.shape = ShapeKind::Custom;
            alone.primitives.push_back(settings.primitives[3]); // the tilted box
            const BuiltShape aloneShape = BuildAssetShape("", alone, false);
            constexpr float kToRad = 3.14159265358979f / 180.0f;
            const Math::mat4 tilt = Math::mat4::RotationFromEulerXYZRad(Math::float3(10.0f * kToRad, 20.0f * kToRad, 30.0f * kToRad)) * Math::mat4::Translation(Math::float3(0.0f, 1.0f, 10.0f));
            const Math::float3 faceCentre = tilt.TransformPoint(Math::float3(0.0f, 0.25f, 0.0f));
            const Math::float3 outside = tilt.TransformPoint(Math::float3(0.0f, 3.25f, 0.0f));
            float worst = 0.0f;
            TestWorld aloneWorld(1);
            aloneWorld.AddStatic(aloneShape.shape.GetPtr());
            aloneWorld.system.OptimizeBroadPhase();
            for (TestWorld* w : { &world, &aloneWorld })
            {
                const JPH::RRayCast ray(ToJolt(outside), ToJolt(faceCentre - outside) * 2.0f);
                JPH::RayCastResult result;
                if (!w->system.GetNarrowPhaseQuery().CastRay(ray, result))
                {
                    detail += "; the tilted box is not where the matrix puts it";
                    return Verdict::Fail;
                }
                worst = std::max(worst, std::fabs(result.mFraction * 2.0f - 1.0f) * 3.0f);
            }
            detail += "; tilted face off by " + std::to_string(worst * 1000.0f) + " mm";
            if (worst > 1.0e-3f) { return Verdict::Fail; }

            // What lands on it: a ball on the box (less Jolt's 2 cm of slop); the list as one dynamic body on a floor, its lowest part on it.
            JPH::SphereShapeSettings sphere(0.5f);
            sphere.SetEmbedded();
            const JPH::BodyID ball = world.AddDynamic(sphere.Create().Get().GetPtr(), JPH::Vec3(0.0f, 5.0f, 0.0f), JPH::Quat::sIdentity());
            world.Step(240);
            const float ballY = world.system.GetBodyInterface().GetCenterOfMassPosition(ball).GetY();

            TestWorld fall(2);
            const JPH::RefConst<JPH::Shape> floor = BoxOf(50.0f);
            JPH::BodyCreationSettings floorSettings(floor.GetPtr(), JPH::Vec3(0.0f, -50.0f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kNonMoving);
            fall.system.GetBodyInterface().CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);
            CollisionSettings solo;
            solo.shape = ShapeKind::Custom;
            CollisionPrimitive box;
            box.center = Math::float3(0.0f, 1.0f, 0.0f);
            box.halfExtents = Math::float3(1.0f, 1.0f, 1.0f);
            CollisionPrimitive other = box;
            other.center = Math::float3(3.0f, 1.0f, 0.0f);
            solo.primitives = { box, other };
            const BuiltShape soloShape = BuildAssetShape("", solo, true);
            const JPH::BodyID body = fall.AddDynamic(soloShape.shape.GetPtr(), JPH::Vec3(0.0f, 3.0f, 0.0f), JPH::Quat::sIdentity());
            fall.Step(240);
            const float bodyY = fall.system.GetBodyInterface().GetPosition(body).GetY();
            detail += "; ball rests at " + std::to_string(ballY) + ", a two-box body's origin at " + std::to_string(bodyY);
            if (std::fabs(ballY - 2.48f) >= 0.01f || std::fabs(bodyY + 0.02f) >= 0.02f) { return Verdict::Fail; }

            // A box lies along the mesh: a plate turned 37 degrees comes back at 37 degrees and its own size, where the axes' bounds would be a block round it; a plate along its axes stays on them.
            const auto plate = [](float yawDeg, Math::float3 at)
            {
                const float yaw = yawDeg * 3.14159265f / 180.0f;
                std::vector<Math::float3> points;
                for (const float x : { -0.45f, 0.45f })
                {
                    for (const float y : { 0.0f, 2.0f })
                    {
                        for (const float z : { -0.03f, 0.03f })
                        {
                            points.emplace_back(at.x + x * std::cos(yaw) + z * std::sin(yaw), at.y + y, at.z - x * std::sin(yaw) + z * std::cos(yaw));
                        }
                    }
                }
                return points;
            };
            const SolidBox turned = FitSolidBox(plate(37.0f, Math::float3(1.0f, 0.5f, -2.0f)));
            detail += "; a plate turned 37: yaw " + std::to_string(turned.yawDeg) + ", half " + std::to_string(turned.half.x) + " " + std::to_string(turned.half.z);
            if (std::fabs(turned.yawDeg - 37.0f) > 0.01f || std::fabs(turned.half.x - 0.45f) > 0.001f || std::fabs(turned.half.y - 1.0f) > 0.001f || std::fabs(turned.half.z - 0.03f) > 0.001f ||
                std::fabs(turned.centre.x - 1.0f) > 0.001f || std::fabs(turned.centre.y - 1.5f) > 0.001f || std::fabs(turned.centre.z + 2.0f) > 0.001f)
            {
                return Verdict::Fail;
            }
            const SolidBox straight = FitSolidBox(plate(0.0f, Math::float3(0.0f, 0.0f, 0.0f)));
            if (straight.yawDeg != 0.0f || std::fabs(straight.half.x - 0.45f) > 0.001f) { detail += "; a plate along its axes left them"; return Verdict::Fail; }

            // The same through a real shape: its box stands where the fit says, and rays square to the plate land on its face.
            {
                const std::vector<Math::float3> points = plate(37.0f, Math::float3(0.0f, 0.0f, 0.0f));
                SolidGeometry geometry;
                geometry.positions = points;
                for (const std::uint32_t i : { 0u, 1u, 2u }) { geometry.indices.push_back(i); }
                CollisionSettings boxSettings;
                boxSettings.shape = ShapeKind::Box;
                const BuiltShape built = BuildShape(geometry, boxSettings, ShapeKind::Box);
                TestWorld world(1);
                if (!built.shape) { detail += "; no box shape: " + built.error; return Verdict::Fail; }
                world.AddStatic(built.shape.GetPtr());
                const float yaw = 37.0f * 3.14159265f / 180.0f;
                const JPH::Vec3 normal(std::sin(yaw), 0.0f, std::cos(yaw));
                const JPH::RRayCast ray(normal * 2.0f + JPH::Vec3(0.0f, 1.0f, 0.0f), -normal * 4.0f);
                world.system.GetBroadPhaseQuery();
                world.system.OptimizeBroadPhase();
                JPH::RayCastResult hit;
                if (!world.system.GetNarrowPhaseQuery().CastRay(ray, hit)) { detail += "; a ray square to the box missed it"; return Verdict::Fail; }
                const float distance = hit.mFraction * 4.0f;
                detail += ", through Jolt the face at " + std::to_string(distance) + " (1.97)";
                if (std::fabs(distance - 1.97f) > 0.005f) { return Verdict::Fail; }
            }
            return Verdict::Pass;
        }

        // --------------------------------------------------------------------------------------------------------- rolling
        // ApplyRollingResistance: on a 10 degree ramp (sine 0.17) a ball with none rolls away (the arm that must come out wrong), at 0.1 rolls slower, at 0.3 stays and sleeps;
        // on the flat a ball sent at 3 m/s stops where the deceleration says; a spin in flight is left alone.
        Verdict GateRolling(std::string& detail)
        {
            constexpr float kGravity = -9.81f;
            constexpr float kDt = 1.0f / 60.0f;
            constexpr float kRadius = 0.12f;
            const auto makeBall = [](TestWorld& world, JPH::Vec3Arg at)
            {
                JPH::SphereShapeSettings sphere(kRadius);
                sphere.SetEmbedded();
                JPH::BodyCreationSettings settings(sphere.Create().Get().GetPtr(), at, JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, layers::kMoving);
                settings.mAngularDamping = 0.0f;
                settings.mFriction = 0.5f;
                settings.mRestitution = 0.0f;
                return world.system.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
            };
            struct Result { float x; bool asleep; };
            const auto onRamp = [&](float resistance)
            {
                TestWorld world(1);
                const JPH::RefConst<JPH::Shape> floor = BoxOf(200.0f);
                const JPH::Quat tilt = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), -10.0f * 3.14159265f / 180.0f);
                JPH::BodyCreationSettings floorSettings(floor.GetPtr(), tilt * JPH::Vec3(0.0f, -200.0f, 0.0f), tilt, JPH::EMotionType::Static, layers::kNonMoving);
                floorSettings.mFriction = 0.5f;
                JPH::BodyInterface& bodies = world.system.GetBodyInterface();
                bodies.CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);
                const JPH::BodyID ball = makeBall(world, tilt * JPH::Vec3(0.0f, kRadius + 0.01f, 0.0f));
                float velocityY = 0.0f;
                for (int i = 0; i < 240; ++i)
                {
                    world.Step(1);
                    if (resistance > 0.0f && bodies.IsActive(ball)) { ApplyRollingResistance(bodies, ball, resistance, kRadius, kGravity, kDt, velocityY); }
                }
                return Result{ bodies.GetPosition(ball).GetX(), !bodies.IsActive(ball) };
            };
            const Result none = onRamp(0.0f);
            const Result some = onRamp(0.1f);
            const Result held = onRamp(0.3f);

            TestWorld flat(1);
            const JPH::RefConst<JPH::Shape> floor = BoxOf(200.0f);
            JPH::BodyCreationSettings floorSettings(floor.GetPtr(), JPH::Vec3(0.0f, -200.0f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kNonMoving);
            JPH::BodyInterface& bodies = flat.system.GetBodyInterface();
            bodies.CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);
            const JPH::BodyID sent = makeBall(flat, JPH::Vec3(0.0f, kRadius, 0.0f));
            bodies.SetLinearVelocity(sent, JPH::Vec3(3.0f, 0.0f, 0.0f));
            bodies.SetAngularVelocity(sent, JPH::Vec3(0.0f, 0.0f, -3.0f / kRadius));
            float velocityY = 0.0f;
            for (int i = 0; i < 300; ++i)
            {
                flat.Step(1);
                if (bodies.IsActive(sent)) { ApplyRollingResistance(bodies, sent, 0.2f, kRadius, kGravity, kDt, velocityY); }
            }
            const float rolled = bodies.GetPosition(sent).GetX();

            TestWorld air(1);
            const JPH::BodyID thrown = makeBall(air, JPH::Vec3(0.0f, 100.0f, 0.0f));
            JPH::BodyInterface& airBodies = air.system.GetBodyInterface();
            airBodies.SetAngularVelocity(thrown, JPH::Vec3(0.0f, 0.0f, 10.0f));
            velocityY = 0.0f;
            for (int i = 0; i < 60; ++i)
            {
                air.Step(1);
                ApplyRollingResistance(airBodies, thrown, 0.5f, kRadius, kGravity, kDt, velocityY);
            }
            const float airSpin = airBodies.GetAngularVelocity(thrown).Length();

            const float expected = 9.0f / (2.0f * (5.0f / 7.0f) * 0.2f * 9.81f); // v^2 / 2a
            detail = "on a 10 degree ramp after 4 s: none " + std::to_string(none.x) + " m, 0.1 " + std::to_string(some.x) + " m, 0.3 " + std::to_string(held.x) + " m asleep " + (held.asleep ? "yes" : "no") +
                "; sent at 3 m/s it rolled " + std::to_string(rolled) + " m (v2/2a " + std::to_string(expected) + "); spin in flight " + std::to_string(airSpin);
            if (std::fabs(none.x) < 5.0f || std::fabs(some.x) >= 0.9f * std::fabs(none.x) || std::fabs(held.x) > 0.1f || !held.asleep) { return Verdict::Fail; }
            if (std::fabs(rolled - expected) > 0.25f * expected) { return Verdict::Fail; }
            return airSpin > 9.5f ? Verdict::Pass : Verdict::Fail;
        }

        // ------------------------------------------------------------------------------------------------------------ async
        // physics.async, through the real World and real objects (the other gates are plain Jolt). Frame by frame, a step that lands before the hand-over must give the very bytes the
        // step in line gives: no lag. A slow one must not hold the frame up, the pose must wait instead (the arm that comes out wrong: those frames differ from the step in line, and
        // the step in line, slowed the same, does stall), and it must lose no step: all arms come to rest on the same bytes. And a ray or an object that goes while a step runs.
        class GateBody final : public RenderableObject
        {
        public:
            GateBody() : RenderableObject("", L"") {}
            const std::string& GetGeometryPath() const override { return path_; }
            bool IsSimpleRender() const override { return false; }

        private:
            std::string path_ = "gate"; // a custom shape reads no file; the world only wants a name
        };

        CollisionPrimitive GatePrimitive(PrimitiveKind kind, const Math::float3& halfExtents, float radius)
        {
            CollisionPrimitive primitive;
            primitive.kind = kind;
            primitive.halfExtents = halfExtents;
            primitive.radius = radius;
            return primitive;
        }

        struct AsyncScene
        {
            GateBody floor;
            std::vector<std::unique_ptr<GateBody>> balls;
            std::vector<std::unique_ptr<GateBody>> decoys; // never registered: they only move the balls to other addresses

            explicit AsyncScene(int scatter = 0)
            {
                CollisionSettings floorShape;
                floorShape.shape = ShapeKind::Custom;
                floorShape.primitives.push_back(GatePrimitive(PrimitiveKind::Box, Math::float3(40.0f, 1.0f, 40.0f), 0.0f));
                floor.SetPosition(Math::float3(0.0f, -1.0f, 0.0f));
                floor.SetCollisionSettings(floorShape);
                floor.SetPhysicsMotion(Motion::Static);
                CollisionSettings ballShape;
                ballShape.shape = ShapeKind::Custom;
                ballShape.rollingResistance = 0.4f;
                ballShape.primitives.push_back(GatePrimitive(PrimitiveKind::Sphere, Math::float3(0.0f, 0.0f, 0.0f), 0.25f));
                for (int i = 0; i < 48; ++i)
                {
                    for (int d = 0; d < (i * scatter) % 5; ++d) { decoys.push_back(std::make_unique<GateBody>()); }
                    auto ball = std::make_unique<GateBody>();
                    ball->SetPosition(Math::float3(0.7f * static_cast<float>(i % 8), 0.5f + 0.05f * static_cast<float>(i), 0.7f * static_cast<float>(i / 8)));
                    ball->SetCollisionSettings(ballShape);
                    ball->SetPhysicsMotion(Motion::Dynamic);
                    balls.push_back(std::move(ball));
                }
            }
            std::uint64_t HashOfOffsets() const
            {
                std::uint64_t hash = 14695981039346656037ull;
                for (const auto& ball : balls)
                {
                    const Math::mat4 offset = ball->HasRenderOffset() ? ball->GetRenderOffset() : Math::mat4::Identity();
                    const auto* bytes = reinterpret_cast<const unsigned char*>(&offset.m);
                    for (size_t i = 0; i < sizeof(offset.m); ++i) { hash = (hash ^ bytes[i]) * 1099511628211ull; }
                }
                return hash;
            }
        };

        struct AsyncRun
        {
            std::vector<std::uint64_t> frames;
            std::uint64_t rest = 0;
            float stallP95 = 0.0f; // what Tick + SyncPoses took off the game thread, ms
            float stallMax = 0.0f;
            std::uint64_t held = 0;
            std::uint64_t waits = 0;
            std::uint64_t steps = 0;
            bool rested = false;
        };

        // The same frame times for every arm: a few ms, and now and then a slow frame that owes several steps.
        float GateFrameTime(int frame)
        {
            return frame % 37 == 36 ? 0.040f : 0.003f + 0.0005f * static_cast<float>((frame * 7) % 13);
        }

        AsyncRun RunAsyncScene(bool async, float delayMs, bool landBeforeHandOver, float frameSleepMs, int frameLimit, bool untilRest, int scatter = 0)
        {
            Settings& settings = GameSettings();
            settings.async = async;
            settings.stepDelayMs = delayMs;
            settings.threads = 2;
            settings.simulate = true;
            AsyncRun run;
            AsyncScene scene(scatter);
            {
                World world;
                std::vector<float> stalls;
                int restFrames = 0;
                for (int frame = 0; frame < frameLimit; ++frame)
                {
                    const auto start = std::chrono::steady_clock::now();
                    world.Tick(GateFrameTime(frame));
                    world.KickStep();
                    float stall = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
                    if (landBeforeHandOver) { world.FinishStep(); }
                    const auto handOver = std::chrono::steady_clock::now();
                    world.SyncPoses();
                    stall += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - handOver).count();
                    if (frame > 0) { stalls.push_back(stall); } // the first one builds the bodies
                    run.frames.push_back(scene.HashOfOffsets());
                    if (frameSleepMs > 0.0f) { std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(frameSleepMs * 1000.0f))); }
                    restFrames = world.GetStats().activeBodies == 0 ? restFrames + 1 : 0;
                    if (untilRest && restFrames >= 3) { run.rested = true; break; }
                }
                world.FinishStep();
                const Stats stats = world.GetStats();
                run.held = stats.handOversHeld;
                run.waits = stats.stepWaits;
                run.steps = stats.stepsTotal;
                run.rest = scene.HashOfOffsets();
                if (!stalls.empty())
                {
                    std::sort(stalls.begin(), stalls.end());
                    run.stallP95 = stalls[stalls.size() * 95 / 100];
                    run.stallMax = stalls.back();
                }
            }
            return run;
        }

        Verdict GateAsync(std::string& detail)
        {
            const Settings saved = GameSettings();
            const bool savedRenderThread = threading::g_renderThreadEnabled;
            threading::g_renderThreadEnabled = true; // what physics.async needs
            threading::RoleScope role(threading::ThreadRole::Game);
            TaskSystem::Get().Start(); // the world builds its shapes on it; running already = nothing
            const auto finish = [&](Verdict verdict)
            {
                GameSettings() = saved;
                threading::g_renderThreadEnabled = savedRenderThread;
                return verdict;
            };

            constexpr int kFrames = 2400;
            const AsyncRun inLine = RunAsyncScene(false, 0.0f, false, 0.0f, kFrames, true);
            if (!inLine.rested)
            {
                detail = "the balls never came to rest in " + std::to_string(kFrames) + " frames";
                return finish(Verdict::Fail);
            }
            // The control: without it a difference below could be the world's, not the thread's. The objects stand at other addresses in another run, and the world must not care.
            const AsyncRun again = RunAsyncScene(false, 0.0f, false, 0.0f, static_cast<int>(inLine.frames.size()), false, 3);
            if (again.frames != inLine.frames)
            {
                size_t first = 0;
                const size_t common = std::min(again.frames.size(), inLine.frames.size());
                while (first < common && again.frames[first] == inLine.frames[first]) { ++first; }
                detail = "the same scene twice, in line, differs at frame " + std::to_string(first) + " of " + std::to_string(common);
                return finish(Verdict::Fail);
            }
            const AsyncRun landed = RunAsyncScene(true, 0.0f, true, 0.0f, static_cast<int>(inLine.frames.size()), false);
            if (landed.frames != inLine.frames)
            {
                size_t first = 0;
                const size_t common = std::min(landed.frames.size(), inLine.frames.size());
                while (first < common && landed.frames[first] == inLine.frames[first]) { ++first; }
                detail = "a step that lands before the hand-over differs from the step in line at frame " + std::to_string(first) + " of " + std::to_string(common);
                return finish(Verdict::Fail);
            }

            // 6 ms a step against a frame of ~5: most hand-overs find it running.
            const AsyncRun slow = RunAsyncScene(true, 6.0f, false, 3.0f, kFrames, true);
            const AsyncRun stalled = RunAsyncScene(false, 6.0f, false, 0.0f, 48, false);
            std::string numbers = "in line and landed the same over " + std::to_string(inLine.frames.size()) + " frames, " + std::to_string(inLine.steps) + " steps; a step of 6 ms: game thread p95 " +
                std::to_string(slow.stallP95) + " ms beside the frame, in line " + std::to_string(stalled.stallMax) + " ms at worst; " + std::to_string(slow.held) + " hand-overs of " +
                std::to_string(slow.frames.size()) + " held";
            size_t different = 0;
            for (size_t i = 0; i < std::min(slow.frames.size(), inLine.frames.size()); ++i) { different += slow.frames[i] != inLine.frames[i] ? 1u : 0u; }
            numbers += ", " + std::to_string(different) + " frames differ";
            if (!slow.rested) { detail = "the slow step never came to rest: " + numbers; return finish(Verdict::Fail); }
            if (slow.stallP95 > 3.0f) { detail = "a slow step held the game thread up: " + numbers; return finish(Verdict::Fail); }
            if (stalled.stallMax < 5.0f) { detail = "the control: the step in line, slowed by 6 ms, should stall: " + numbers; return finish(Verdict::Fail); }
            if (slow.held == 0 || different == 0) { detail = "the arm that must come out wrong did not: nothing was held back: " + numbers; return finish(Verdict::Fail); }
            if (slow.rest != inLine.rest) { detail = "the slow step came to rest somewhere else, so it lost or changed a step: " + numbers; return finish(Verdict::Fail); }

            // A ray and an object that goes while a step runs.
            {
                Settings& settings = GameSettings();
                settings.async = true;
                settings.stepDelayMs = 20.0f;
                settings.threads = 2;
                settings.simulate = true;
                AsyncScene scene;
                World world;
                float dt = 0.0f;
                for (int frame = 0; frame < 8; ++frame)
                {
                    dt = GateFrameTime(frame) + 0.017f; // every frame owes a step
                    world.Tick(dt);
                    world.KickStep();
                    if (frame == 3) { scene.balls.resize(24); } // the step has its bodies, these objects are gone
                    if (frame == 5)
                    {
                        RayHit hit;
                        if (!world.RayCast(Math::float3(30.0f, 5.0f, 30.0f), Math::float3(0.0f, -1.0f, 0.0f), 20.0f, hit) || hit.object != &scene.floor)
                        {
                            detail = "a ray during a step did not find the floor";
                            return finish(Verdict::Fail);
                        }
                    }
                    world.SyncPoses();
                }
                world.FinishStep();
                const Stats stats = world.GetStats();
                if (stats.objects != 25 || stats.dynamicBodies != 24 || stats.stepWaits == 0)
                {
                    detail = "objects " + std::to_string(stats.objects) + " dynamic " + std::to_string(stats.dynamicBodies) + " waits " + std::to_string(stats.stepWaits) + " after half of them went during a step";
                    return finish(Verdict::Fail);
                }
                // Gone between the step and the hand-over: the hand-over writes poses, and must not write to these.
                settings.stepDelayMs = 0.0f;
                world.Tick(0.02f);
                world.FinishStep();
                scene.balls.resize(12);
                world.SyncPoses();
                if (world.GetStats().objects != 13)
                {
                    detail = "objects " + std::to_string(world.GetStats().objects) + " at a hand-over after half of them went: it wrote poses to what was still on its list";
                    return finish(Verdict::Fail);
                }
            }
            detail = numbers;
            return finish(Verdict::Pass);
        }

        // ------------------------------------------------------------------------------------------------------------ world
        // The world as the game drives it, with real objects: what the others take on trust from plain Jolt. A boat through a frame that owes several steps stays on its wave (the premise
        // that must come out wrong: one MoveKinematic and two steps go twice as far). A pose under a scale and a rotation, in the order the renderer multiplies (the other order is the
        // arm). Stop. A body under killY stays still and asleep whatever wakes the rest. A custom shape mirrored and stretched stands where the mesh does (Jolt's nearest scale is the arm).
        Verdict GateWorld(std::string& detail)
        {
            const Settings saved = GameSettings();
            const bool savedRenderThread = threading::g_renderThreadEnabled;
            threading::g_renderThreadEnabled = true;
            threading::RoleScope role(threading::ThreadRole::Game);
            TaskSystem::Get().Start();
            const auto finish = [&](Verdict verdict)
            {
                GameSettings() = saved;
                threading::g_renderThreadEnabled = savedRenderThread;
                return verdict;
            };
            Settings& settings = GameSettings();
            settings.async = true;
            settings.threads = 2;
            settings.simulate = true;
            settings.stepDelayMs = 0.0f;
            const auto frame = [](World& world, float dt)
            {
                world.Tick(dt);
                world.KickStep();
                world.FinishStep();
                world.SyncPoses();
            };
            const auto topAt = [](World& world, float x, float z, float& y)
            {
                RayHit hit;
                if (!world.RayCast(Math::float3(x, 100.0f, z), Math::float3(0.0f, -1.0f, 0.0f), 200.0f, hit)) { return false; }
                y = hit.point.y;
                return true;
            };
            const auto customBox = [](const Math::float3& half)
            {
                CollisionSettings shape;
                shape.shape = ShapeKind::Custom;
                shape.primitives.push_back(GatePrimitive(PrimitiveKind::Box, half, 0.0f));
                return shape;
            };
            const auto customBall = [](float radius)
            {
                CollisionSettings shape;
                shape.shape = ShapeKind::Custom;
                shape.rollingResistance = 0.4f;
                shape.primitives.push_back(GatePrimitive(PrimitiveKind::Sphere, Math::float3(0.0f, 0.0f, 0.0f), radius));
                return shape;
            };

            std::string report;
            // A boat.
            {
                TestWorld jolt(1);
                const JPH::RefConst<JPH::Shape> box = BoxOf(0.5f);
                JPH::BodyCreationSettings body(box.GetPtr(), JPH::Vec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, layers::kMoving);
                JPH::BodyInterface& bodies = jolt.system.GetBodyInterface();
                const JPH::BodyID id = bodies.CreateAndAddBody(body, JPH::EActivation::Activate);
                bodies.MoveKinematic(id, JPH::Vec3(0.0f, 1.0f, 0.0f), JPH::Quat::sIdentity(), 1.0f / 60.0f);
                jolt.Step(2);
                const float twoSteps = bodies.GetCenterOfMassPosition(id).GetY();
                if (std::fabs(twoSteps - 2.0f) > 0.01f)
                {
                    detail = "the premise: one MoveKinematic and two steps should go 2 m, went " + std::to_string(twoSteps);
                    return finish(Verdict::Fail);
                }

                GateBody boat;
                boat.SetCollisionSettings(customBox(Math::float3(1.0f, 0.5f, 1.0f)));
                boat.SetBuoyant(true);
                boat.SetPhysicsMotion(Motion::Static);
                World world;
                float worst = 0.0f;
                float top = 0.0f;
                for (int i = 0; i < 90; ++i)
                {
                    const float wave = i < 60 ? 0.5f * std::sin(0.3f * static_cast<float>(i)) : 0.5f * std::sin(0.3f * 59.0f); // then it holds still: no wave is no speed
                    boat.SetRenderOffset(Math::mat4::Translation(Math::float3(0.0f, wave, 0.0f)));
                    frame(world, 0.05f); // three steps a frame: 20 fps
                    if (!topAt(world, 0.0f, 0.0f, top))
                    {
                        detail = "the boat left the world at frame " + std::to_string(i);
                        return finish(Verdict::Fail);
                    }
                    worst = std::max(worst, std::fabs(top - (0.5f + wave)));
                }
                report += "boat off its wave by " + std::to_string(worst) + " m at 20 fps; ";
                if (worst > 0.002f)
                {
                    detail = report;
                    return finish(Verdict::Fail);
                }
            }
            // A pose, and Stop.
            {
                GateBody floor;
                floor.SetCollisionSettings(customBox(Math::float3(40.0f, 1.0f, 40.0f)));
                floor.SetPosition(Math::float3(0.0f, -1.0f, 0.0f));
                floor.SetPhysicsMotion(Motion::Static);
                GateBody ball;
                ball.SetCollisionSettings(customBall(0.25f));
                ball.SetPosition(Math::float3(1.0f, 3.0f, -2.0f));
                ball.SetScale(Math::float3(2.0f, 2.0f, 2.0f));
                ball.SetRotationEulerDeg(Math::float3(0.0f, 40.0f, 0.0f));
                ball.SetPhysicsMotion(Motion::Dynamic);
                World world;
                int frames = 0;
                do
                {
                    frame(world, 1.0f / 60.0f);
                } while (world.GetStats().activeBodies != 0 && ++frames < 900);
                frame(world, 1.0f / 60.0f);
                const Math::mat4 model = ball.GetAuthoredModelMatrix() * ball.GetRenderOffset();
                const Math::mat4 wrong = ball.GetRenderOffset() * ball.GetAuthoredModelMatrix();
                report += "ball at rest at " + std::to_string(model.m._41) + " " + std::to_string(model.m._42) + " " + std::to_string(model.m._43) + "; ";
                if (std::fabs(model.m._41 - 1.0f) > 0.02f || std::fabs(model.m._42 - 0.5f) > 0.03f || std::fabs(model.m._43 + 2.0f) > 0.02f)
                {
                    detail = report + "should stand at 1 0.5 -2, its radius off the floor";
                    return finish(Verdict::Fail);
                }
                if (std::fabs(wrong.m._42 - model.m._42) < 0.5f)
                {
                    detail = report + "the arm: the other order of the matrices should land elsewhere";
                    return finish(Verdict::Fail);
                }
                settings.simulate = false;
                world.Tick(1.0f / 60.0f);
                float top = 0.0f;
                if (ball.HasRenderOffset() || !topAt(world, 1.0f, -2.0f, top) || std::fabs(top - 3.5f) > 0.01f)
                {
                    detail = report + "Stop should put the ball back at the level's place, its top at 3.5: " + std::to_string(top);
                    return finish(Verdict::Fail);
                }
                settings.simulate = true;
            }
            // Under the world.
            {
                const float killY = settings.killY;
                settings.killY = -5.0f;
                GateBody ball;
                ball.SetCollisionSettings(customBall(0.25f));
                ball.SetPhysicsMotion(Motion::Dynamic);
                World world;
                int frames = 0;
                while (world.GetStats().fellOut == 0 && ++frames < 900) { frame(world, 1.0f / 60.0f); }
                frame(world, 1.0f / 60.0f);
                const Math::mat4 before = ball.GetRenderOffset();
                const float gravity = settings.gravity;
                settings.gravity = -5.0f; // wakes everything that may wake
                for (int i = 0; i < 20; ++i) { frame(world, 1.0f / 60.0f); }
                settings.gravity = gravity;
                settings.killY = killY;
                const Stats stats = world.GetStats();
                const bool still = std::memcmp(&before.m, &ball.GetRenderOffset().m, sizeof(before.m)) == 0;
                report += "fell out after " + std::to_string(frames) + " frames, then " + (still ? "still" : "moving") + ", " + std::to_string(stats.activeBodies) + " awake; ";
                if (stats.fellOut != 1 || !still || stats.activeBodies != 0)
                {
                    detail = report;
                    return finish(Verdict::Fail);
                }
            }
            // A custom shape, mirrored and stretched.
            {
                CollisionSettings shape;
                shape.shape = ShapeKind::Custom;
                CollisionPrimitive cylinder = GatePrimitive(PrimitiveKind::Cylinder, Math::float3(0.0f, 0.0f, 0.0f), 0.5f);
                cylinder.center = Math::float3(2.0f, 1.0f, 0.0f);
                cylinder.halfHeight = 1.0f;
                cylinder.rotationDeg = Math::float3(5.0f, 0.0f, 0.0f); // turned: what a palm's trunk is, and what Jolt's compound takes only a uniform scale for
                shape.primitives.push_back(cylinder);
                CollisionPrimitive box = GatePrimitive(PrimitiveKind::Box, Math::float3(0.5f, 0.5f, 0.5f), 0.0f);
                box.center = Math::float3(-2.0f, 0.5f, 0.0f);
                shape.primitives.push_back(box);

                GateBody stretched;
                stretched.SetCollisionSettings(shape);
                stretched.SetScale(Math::float3(1.0f, 3.0f, 1.0f));
                stretched.SetPhysicsMotion(Motion::Static);
                GateBody mirrored;
                mirrored.SetCollisionSettings(shape);
                mirrored.SetPosition(Math::float3(0.0f, 0.0f, 20.0f));
                mirrored.SetScale(Math::float3(-1.0f, 1.0f, 1.0f));
                mirrored.SetPhysicsMotion(Motion::Static);
                World world;
                frame(world, 1.0f / 60.0f);
                float stretchedTrunk = 0.0f, stretchedBox = 0.0f, mirroredTrunk = 0.0f, mirroredBox = 0.0f;
                const bool hits = topAt(world, 2.0f, 0.0f, stretchedTrunk) && topAt(world, -2.0f, 0.0f, stretchedBox) && topAt(world, -2.0f, 20.0f, mirroredTrunk) && topAt(world, 2.0f, 20.0f, mirroredBox);
                report += "stretched x3 tops " + std::to_string(stretchedTrunk) + " " + std::to_string(stretchedBox) + ", mirrored " + std::to_string(mirroredTrunk) + " " + std::to_string(mirroredBox) + "; ";
                if (!hits || std::fabs(stretchedTrunk - 6.0f) > 0.4f || std::fabs(stretchedBox - 3.0f) > 0.05f || std::fabs(mirroredTrunk - 2.0f) > 0.1f || std::fabs(mirroredBox - 1.0f) > 0.05f)
                {
                    detail = report + "should be 6 3, 2 1";
                    return finish(Verdict::Fail);
                }
                // The arm: what Jolt makes of that scale by itself.
                const BuiltShape built = BuildAssetShape("gate", shape, false);
                std::string error;
                const JPH::RefConst<JPH::Shape> nearest = ScaledInstance(*built.shape, Math::float3(1.0f, 3.0f, 1.0f), error);
                TestWorld jolt(1);
                jolt.AddStatic(nearest.GetPtr());
                jolt.system.OptimizeBroadPhase();
                const JPH::RRayCast ray(JPH::Vec3(2.0f, 100.0f, 0.0f), JPH::Vec3(0.0f, -200.0f, 0.0f));
                JPH::RayCastResult hit;
                const bool nearestHit = jolt.system.GetNarrowPhaseQuery().CastRay(ray, hit);
                const float nearestTop = nearestHit ? 100.0f - hit.mFraction * 200.0f : -1000.0f;
                report += "Jolt's nearest scale " + std::to_string(nearestTop);
                if (std::fabs(nearestTop - 6.0f) < 0.5f)
                {
                    detail = report + ": the arm should come out wrong";
                    return finish(Verdict::Fail);
                }
            }
            // The helm's road through the world: who is near a hull (its rectangle, not a circle of its length), taking it, the numbers by name, and a boat gone with its helm held.
            {
                auto boat = std::make_unique<GateBody>();
                boat->SetCollisionSettings(customBox(Math::float3(5.0f, 1.35f, 1.17f)));
                boat->SetVesselSettings(std::make_shared<VesselSettings>());
                boat->SetBuoyant(true);
                boat->SetPhysicsMotion(Motion::Dynamic);
                World world;
                frame(world, 1.0f / 60.0f);
                const Stats made = world.GetStats();
                const bool beside = world.FindVessel(Math::float3(0.0f, 0.0f, 3.5f), 3.0f) == boat.get();
                const bool past = world.FindVessel(Math::float3(0.0f, 0.0f, 4.5f), 3.0f) == nullptr;
                const bool taken = world.SetHelm(boat.get()) && world.GetHelm().exists;
                world.SetHelmInput(VesselInput{ 1.0f, 1.0f });
                frame(world, 1.0f / 60.0f);
                const bool tuned = world.TuneVessels("thrust", 1000.0f) && !world.TuneVessels("nonsense", 1.0f);
                boat.reset();
                frame(world, 1.0f / 60.0f);
                const bool gone = !world.GetHelm().exists && !world.SetHelm(nullptr);
                report += "helm: floating " + std::to_string(made.floatingBodies) + ", beside " + std::to_string(beside) + ", past " + std::to_string(past) + ", taken " + std::to_string(taken) +
                    ", tuned " + std::to_string(tuned) + ", gone " + std::to_string(gone) + "; ";
                if (made.floatingBodies != 1 || !beside || !past || !taken || !tuned || !gone)
                {
                    detail = report;
                    return finish(Verdict::Fail);
                }
            }
            detail = report;
            return finish(Verdict::Pass);
        }

        struct Gate
        {
            const char* name;
            Verdict (*run)(std::string&);
        };
        constexpr Gate kGates[] = {
            { "smoke", &GateSmoke },
            { "pose", &GatePose },
            { "ray", &GateRay },
            { "foliage", &GateFoliage },
            { "determinism", &GateDeterminism },
            { "scale", &GateScale },
            { "walk", &GateWalk },
            { "player", &GatePlayer },
            { "water", &GateWater },
            { "vessel", &GateVessel },
            { "kinematic", &GateKinematic },
            { "autoshape", &GateAutoShape },
            { "custom", &GateCustom },
            { "rolling", &GateRolling },
            { "async", &GateAsync },
            { "world", &GateWorld },
        };
    }

    int RunGates(const std::string& name)
    {
        AcquireJolt();
        int failed = 0;
        int ran = 0;
        for (const Gate& gate : kGates)
        {
            if (!name.empty() && name != gate.name) { continue; }
            ++ran;
            std::string detail;
            const auto start = std::chrono::steady_clock::now();
            const Verdict verdict = gate.run(detail);
            const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (verdict == Verdict::Fail)
            {
                ++failed;
                LOG_ERROR(logging::LogCategory::Physics, "physics gate: {} FAIL {} ({:.0f} ms)", gate.name, detail, ms);
            }
            else
            {
                LOG_INFO(logging::LogCategory::Physics, "physics gate: {} {} {} ({:.0f} ms)", gate.name, verdict == Verdict::Pass ? "PASS" : "SKIP", detail, ms);
            }
        }
        if (ran == 0)
        {
            ++failed;
            LOG_ERROR(logging::LogCategory::Physics, "physics gate: no gate named '{}'", name);
        }
        LOG_INFO(logging::LogCategory::Physics, "physics gates: {} ran, {} failed", ran, failed);
        ReleaseJolt();
        return failed;
    }
} // namespace physics
