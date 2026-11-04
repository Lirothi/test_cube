#include "physics/PhysicsInternal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <tuple>

#include "core/logging/Log.h"
#include "physics/PhysicsWorld.h"
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
                WalkingCharacter walker(world.system, JPH::Vec3(0.0f, 0.0f, 0.0f));
                const float dt = 1.0f / fps;
                const auto walk = [&](float seconds, JPH::Vec3Arg velocity)
                {
                    for (int i = 0; i < static_cast<int>(seconds * fps); ++i) { walker.Update(dt, velocity, false, true, world.temp); }
                };
                walk(0.5f, JPH::Vec3::sZero()); // settles
                const float start = walker.Feet().GetX();
                walk(2.0f, JPH::Vec3(1.6f, 0.0f, 0.0f));
                const float walked = walker.Feet().GetX() - start;
                detail += std::to_string(fps) + " fps walked " + std::to_string(walked) + " m of 3.2; ";
                if (std::fabs(walked - 3.2f) > 0.1f) { return Verdict::Fail; }

                // A kerb: 20 cm, in the way along z.
                addStatic(world, JPH::Vec3(walker.Feet().GetX() + 1.5f, 0.1f, 0.0f), JPH::Vec3(0.5f, 0.1f, 5.0f));
                walk(1.0f, JPH::Vec3(1.6f, 0.0f, 0.0f));
                const float onKerb = walker.Feet().GetY();
                detail += "on the kerb at y " + std::to_string(onKerb) + "; ";
                if (onKerb < 0.15f) { return Verdict::Fail; }

                // A wall: 3 m high.
                const float wallX = walker.Feet().GetX() + 2.0f;
                addStatic(world, JPH::Vec3(wallX + 0.25f, 1.5f, 0.0f), JPH::Vec3(0.25f, 1.5f, 20.0f));
                walk(4.0f, JPH::Vec3(1.6f, 0.0f, 0.0f));
                const float stopped = walker.Feet().GetX();
                detail += "wall at " + std::to_string(wallX) + " stopped it at " + std::to_string(stopped) + "; ";
                if (stopped > wallX - 0.25f || stopped < wallX - 0.45f) { return Verdict::Fail; }
            }

            // A box on the floor, pushed when the world runs and left alone when it does not.
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
                WalkingCharacter walker(world.system, JPH::Vec3(0.0f, 0.0f, 0.0f));
                for (int i = 0; i < 180; ++i)
                {
                    walker.Update(1.0f / 60.0f, JPH::Vec3(1.6f, 0.0f, 0.0f), false, push, world.temp);
                    world.Step(1);
                }
                const float x = world.system.GetBodyInterface().GetCenterOfMassPosition(crate).GetX();
                detail += std::string(push ? "pushed" : "not pushing") + ": the box at x " + std::to_string(x) + "; ";
                if (push ? x < 1.7f : std::fabs(x - 1.5f) > 0.02f) { return Verdict::Fail; }
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
            { "water", &GateWater },
            { "kinematic", &GateKinematic },
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
