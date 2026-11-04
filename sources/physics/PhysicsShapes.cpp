#include "physics/PhysicsInternal.h"

#include <algorithm>
#include <cmath>

#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include "core/logging/Log.h"
#include "physics/PhysicsWorld.h"
#include "rendering/meshes/MeshManager.h"

namespace physics
{
    bool ReadSolidGeometry(const std::string& geometryPath, int lod, const std::vector<int>& excludeSlots, SolidGeometry& out)
    {
        out = SolidGeometry{};
        std::vector<Math::float3> positions;
        std::vector<std::uint32_t> indices;
        std::vector<Mesh::Submesh> submeshes;

        MeshManager::CollisionGeometry baked;
        if (MeshManager::ReadCollisionGeometry(geometryPath, lod, baked))
        {
            positions = std::move(baked.positions);
            indices = std::move(baked.indices);
            submeshes = std::move(baked.submeshes);
            out.lodUsed = baked.lodUsed;
            out.lodCount = baked.lodCount;
        }
        else
        {
            // Not a .mesh.bin (an OBJ, a glTF node): the importer's own parse, LOD0, as the buoyancy's analysis does. A local
            // manager: its cache is the frame's.
            MeshCpuData cpu;
            MeshLoadOptions options;
            options.generateTangentSpace = false;
            MeshManager manager;
            if (geometryPath.empty() || !manager.ParseFileCpu(geometryPath, cpu, options) || cpu.indices.size() < 3)
            {
                return false;
            }
            positions.resize(cpu.vertices.size());
            for (size_t i = 0; i < cpu.vertices.size(); ++i)
            {
                positions[i] = Math::float3(cpu.vertices[i].position);
            }
            indices = std::move(cpu.indices);
            submeshes = std::move(cpu.submeshes);
            out.lodUsed = 0;
            out.lodCount = 1;
        }

        const auto excluded = [&excludeSlots](std::uint32_t slot)
        {
            return std::find(excludeSlots.begin(), excludeSlots.end(), static_cast<int>(slot)) != excludeSlots.end();
        };

        // The triangles left, and only the vertices they use: a bake's vertex buffer can carry LOD3 prune copies past LOD0's
        // range, and the excluded slots' vertices are no part of the shape.
        constexpr std::uint32_t kUnused = 0xFFFFFFFFu;
        std::vector<std::uint32_t> remap(positions.size(), kUnused);
        const auto take = [&](std::uint32_t first, std::uint32_t count)
        {
            for (std::uint32_t i = first; i + 2 < first + count && i + 2 < indices.size(); i += 3)
            {
                const std::uint32_t tri[3] = { indices[i], indices[i + 1], indices[i + 2] };
                if (tri[0] >= positions.size() || tri[1] >= positions.size() || tri[2] >= positions.size()) { continue; }
                for (const std::uint32_t v : tri)
                {
                    if (remap[v] == kUnused)
                    {
                        remap[v] = static_cast<std::uint32_t>(out.positions.size());
                        out.positions.push_back(positions[v]);
                    }
                    out.indices.push_back(remap[v]);
                }
            }
        };
        if (submeshes.empty())
        {
            take(0, static_cast<std::uint32_t>(indices.size()));
        }
        else
        {
            for (const Mesh::Submesh& submesh : submeshes)
            {
                if (!excluded(submesh.materialSlot)) { take(submesh.indexOffset, submesh.indexCount); }
            }
        }
        return true;
    }

    ShapeKind ResolveShapeKind(ShapeKind requested, bool dynamicBody)
    {
        if (requested == ShapeKind::Auto)
        {
            if (dynamicBody) { return ShapeKind::Hull; }
            return GameSettings().autoShapes ? ShapeKind::Auto : ShapeKind::Mesh; // physics.autoShapes:0 = every static asset's triangles, the A/B
        }
        if (requested == ShapeKind::Mesh && dynamicBody) { return ShapeKind::Hull; } // Jolt: no mesh against mesh
        return requested;
    }

    namespace
    {
        JPH::RefConst<JPH::Shape> Centred(const JPH::Shape* shape, const Math::float3& centre, JPH::QuatArg rotation, std::string& error)
        {
            JPH::RotatedTranslatedShapeSettings settings(ToJolt(centre), rotation, shape);
            settings.SetEmbedded();
            const JPH::ShapeSettings::ShapeResult result = settings.Create();
            if (result.HasError())
            {
                error = result.GetError().c_str();
                return nullptr;
            }
            return result.Get();
        }

        float RadiusFor(const CollisionSettings& settings, float smallestExtent)
        {
            if (settings.convexRadius >= 0.0f) { return settings.convexRadius; }
            // A thin object cannot shrink by Jolt's 5 cm: a stick is 3 cm across.
            return std::min(JPH::cDefaultConvexRadius, 0.25f * smallestExtent);
        }
    }

    BuiltShape BuildShape(const SolidGeometry& geometry, const CollisionSettings& settings, ShapeKind kind)
    {
        BuiltShape out;
        out.triangles = static_cast<std::uint32_t>(geometry.indices.size() / 3);
        if (kind == ShapeKind::None || kind == ShapeKind::Custom || geometry.indices.size() < 3) { return out; }

        Math::float3 lo(FLT_MAX, FLT_MAX, FLT_MAX);
        Math::float3 hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const Math::float3& p : geometry.positions)
        {
            lo = Math::float3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Math::float3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        out.boundsMin = lo;
        out.boundsMax = hi;
        const Math::float3 extent(hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
        const Math::float3 centre(0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z));
        out.smallestExtent = std::min({ extent.x, extent.y, extent.z });
        const float radius = RadiusFor(settings, out.smallestExtent);
        if (kind == ShapeKind::Auto) { kind = std::max({ extent.x, extent.y, extent.z }) < kAutoHullBelow ? ShapeKind::Hull : ShapeKind::Mesh; }
        out.kind = kind;

        if (kind == ShapeKind::Mesh)
        {
            JPH::VertexList vertices;
            vertices.reserve(geometry.positions.size());
            for (const Math::float3& p : geometry.positions)
            {
                vertices.push_back(JPH::Float3(p.x, p.y, p.z));
            }
            JPH::IndexedTriangleList triangles;
            triangles.reserve(geometry.indices.size() / 3 * (settings.twoSided ? 2 : 1));
            for (size_t i = 0; i + 2 < geometry.indices.size(); i += 3)
            {
                triangles.push_back(JPH::IndexedTriangle(geometry.indices[i], geometry.indices[i + 1], geometry.indices[i + 2]));
                if (settings.twoSided)
                {
                    triangles.push_back(JPH::IndexedTriangle(geometry.indices[i], geometry.indices[i + 2], geometry.indices[i + 1]));
                }
            }
            JPH::MeshShapeSettings mesh(std::move(vertices), std::move(triangles));
            mesh.SetEmbedded();
            const JPH::ShapeSettings::ShapeResult result = mesh.Create();
            if (result.HasError())
            {
                out.error = result.GetError().c_str();
                return out;
            }
            out.shape = result.Get();
            return out;
        }

        if (kind == ShapeKind::Hull)
        {
            JPH::Array<JPH::Vec3> points;
            points.reserve(geometry.positions.size());
            for (const Math::float3& p : geometry.positions)
            {
                points.push_back(ToJolt(p));
            }
            JPH::ConvexHullShapeSettings hull(points, radius);
            hull.mHullTolerance = std::max(hull.mHullTolerance, GameSettings().hullTolerance * out.smallestExtent);
            hull.SetEmbedded();
            hull.SetDensity(settings.density);
            const JPH::ShapeSettings::ShapeResult result = hull.Create();
            if (!result.HasError())
            {
                out.shape = result.Get();
                return out;
            }
            out.error = result.GetError().c_str(); // a flat or a degenerate mesh: the object must still fall, so its box
            kind = ShapeKind::Box;
            out.kind = kind;
        }

        const JPH::Quat none = JPH::Quat::sIdentity();
        if (kind == ShapeKind::Box)
        {
            const float pad = 0.001f; // a flat mesh's box still has a volume
            JPH::BoxShapeSettings box(JPH::Vec3(std::max(0.5f * extent.x, pad), std::max(0.5f * extent.y, pad), std::max(0.5f * extent.z, pad)),
                std::min(radius, std::max(0.5f * out.smallestExtent, 0.0f)));
            box.SetEmbedded();
            box.SetDensity(settings.density);
            const JPH::ShapeSettings::ShapeResult result = box.Create();
            if (result.HasError())
            {
                out.error = result.GetError().c_str();
                return out;
            }
            out.shape = Centred(result.Get().GetPtr(), centre, none, out.error);
            return out;
        }
        if (kind == ShapeKind::Sphere)
        {
            JPH::SphereShapeSettings sphere(std::max(0.5f * std::max({ extent.x, extent.y, extent.z }), 0.001f));
            sphere.SetEmbedded();
            sphere.SetDensity(settings.density);
            const JPH::ShapeSettings::ShapeResult result = sphere.Create();
            if (result.HasError())
            {
                out.error = result.GetError().c_str();
                return out;
            }
            out.shape = Centred(result.Get().GetPtr(), centre, none, out.error);
            return out;
        }

        // A capsule along the longest axis; Jolt's stands along y.
        const float longest = std::max({ extent.x, extent.y, extent.z });
        JPH::Quat rotation = none;
        float across = std::max(extent.x, extent.z);
        if (longest == extent.x)
        {
            rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), 0.5f * JPH::JPH_PI);
            across = std::max(extent.y, extent.z);
        }
        else if (longest != extent.y)
        {
            rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * JPH::JPH_PI);
            across = std::max(extent.x, extent.y);
        }
        const float capsuleRadius = std::max(0.5f * across, 0.001f);
        JPH::CapsuleShapeSettings capsule(std::max(0.5f * longest - capsuleRadius, 0.001f), capsuleRadius);
        capsule.SetEmbedded();
        capsule.SetDensity(settings.density);
        const JPH::ShapeSettings::ShapeResult result = capsule.Create();
        if (result.HasError())
        {
            out.error = result.GetError().c_str();
            return out;
        }
        out.shape = Centred(result.Get().GetPtr(), centre, rotation, out.error);
        return out;
    }

    namespace
    {
        // Custom: the primitives as one compound, what the asset's copies share.
        BuiltShape BuildPrimitives(const CollisionSettings& settings)
        {
            BuiltShape out;
            out.kind = ShapeKind::Custom;
            if (settings.primitives.empty()) { return out; }
            constexpr float kMinSize = 0.001f;
            constexpr float kToRad = 3.14159265358979f / 180.0f;
            Math::float3 lo(FLT_MAX, FLT_MAX, FLT_MAX);
            Math::float3 hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            float smallest = FLT_MAX;
            std::vector<JPH::RefConst<JPH::Shape>> parts;
            std::vector<JPH::Quat> rotations;
            parts.reserve(settings.primitives.size());
            for (const CollisionPrimitive& p : settings.primitives)
            {
                JPH::ShapeSettings::ShapeResult result;
                float reach = 0.0f;
                switch (p.kind)
                {
                case PrimitiveKind::Box:
                {
                    const JPH::Vec3 half(std::max(p.halfExtents.x, kMinSize), std::max(p.halfExtents.y, kMinSize), std::max(p.halfExtents.z, kMinSize));
                    const float least = half.ReduceMin();
                    JPH::BoxShapeSettings box(half, settings.convexRadius >= 0.0f ? std::min(settings.convexRadius, least) : std::min(JPH::cDefaultConvexRadius, 0.5f * least));
                    box.SetEmbedded();
                    box.SetDensity(settings.density);
                    result = box.Create();
                    reach = half.Length();
                    smallest = std::min(smallest, 2.0f * least);
                    break;
                }
                case PrimitiveKind::Sphere:
                {
                    JPH::SphereShapeSettings sphere(std::max(p.radius, kMinSize));
                    sphere.SetEmbedded();
                    sphere.SetDensity(settings.density);
                    result = sphere.Create();
                    reach = std::max(p.radius, kMinSize);
                    smallest = std::min(smallest, 2.0f * reach);
                    break;
                }
                case PrimitiveKind::Capsule:
                {
                    JPH::CapsuleShapeSettings capsule(std::max(p.halfHeight, kMinSize), std::max(p.radius, kMinSize));
                    capsule.SetEmbedded();
                    capsule.SetDensity(settings.density);
                    result = capsule.Create();
                    reach = std::max(p.halfHeight, kMinSize) + std::max(p.radius, kMinSize);
                    smallest = std::min(smallest, 2.0f * std::max(p.radius, kMinSize));
                    break;
                }
                case PrimitiveKind::Cylinder:
                {
                    const float halfHeight = std::max(p.halfHeight, kMinSize);
                    const float radius = std::max(p.radius, kMinSize);
                    const float least = std::min(halfHeight, radius);
                    JPH::CylinderShapeSettings cylinder(halfHeight, radius, settings.convexRadius >= 0.0f ? std::min(settings.convexRadius, least) : std::min(JPH::cDefaultConvexRadius, 0.5f * least));
                    cylinder.SetEmbedded();
                    cylinder.SetDensity(settings.density);
                    result = cylinder.Create();
                    reach = std::sqrt(halfHeight * halfHeight + radius * radius);
                    smallest = std::min(smallest, 2.0f * least);
                    break;
                }
                }
                if (result.HasError())
                {
                    out.error = result.GetError().c_str();
                    return out;
                }
                parts.push_back(result.Get());
                const Math::mat4 rotation = Math::mat4::RotationFromEulerXYZRad(Math::float3(p.rotationDeg.x * kToRad, p.rotationDeg.y * kToRad, p.rotationDeg.z * kToRad));
                rotations.push_back(ToJolt(Math::quat::FromXM(DirectX::XMQuaternionRotationMatrix(rotation.xm())).Normalized()));
                lo = Math::float3(std::min(lo.x, p.center.x - reach), std::min(lo.y, p.center.y - reach), std::min(lo.z, p.center.z - reach));
                hi = Math::float3(std::max(hi.x, p.center.x + reach), std::max(hi.y, p.center.y + reach), std::max(hi.z, p.center.z + reach));
            }
            out.boundsMin = lo;
            out.boundsMax = hi;
            out.smallestExtent = smallest;
            if (parts.size() == 1) // a compound asks for two parts at least
            {
                out.shape = Centred(parts[0].GetPtr(), settings.primitives[0].center, rotations[0], out.error);
                return out;
            }
            JPH::StaticCompoundShapeSettings compound;
            compound.SetEmbedded();
            for (size_t i = 0; i < parts.size(); ++i)
            {
                compound.AddShape(ToJolt(settings.primitives[i].center), rotations[i], parts[i].GetPtr());
            }
            const JPH::ShapeSettings::ShapeResult result = compound.Create();
            if (result.HasError())
            {
                out.error = result.GetError().c_str();
                return out;
            }
            out.shape = result.Get();
            return out;
        }
    }

    BuiltShape BuildAssetShape(const std::string& geometryPath, const CollisionSettings& settings, bool dynamicBody)
    {
        const ShapeKind kind = ResolveShapeKind(settings.shape, dynamicBody);
        if (kind == ShapeKind::None) { return BuiltShape{}; }
        if (kind == ShapeKind::Custom) { return BuildPrimitives(settings); }
        const Settings& knobs = GameSettings();
        int lod = settings.lod;
        const bool forced = lod < 0 && knobs.forceLod >= 0;
        if (lod < 0)
        {
            const bool leaves = !settings.excludeSlots.empty() && knobs.autoShapes && (kind == ShapeKind::Auto || kind == ShapeKind::Mesh);
            lod = forced ? knobs.forceLod : leaves ? kAutoFoliageLod : 0;
        }
        SolidGeometry geometry;
        if (!ReadSolidGeometry(geometryPath, lod, settings.excludeSlots, geometry))
        {
            BuiltShape out;
            out.error = "geometry not readable";
            return out;
        }
        if (forced && lod != 0)
        {
            // physics.forceLod, the plan's A/B: not on terrain, which stands under everything.
            float largest = 0.0f;
            Math::float3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            for (const Math::float3& p : geometry.positions)
            {
                lo = Math::float3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                hi = Math::float3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
            }
            largest = std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z });
            if (largest >= 100.0f) { ReadSolidGeometry(geometryPath, 0, settings.excludeSlots, geometry); }
        }
        BuiltShape out = BuildShape(geometry, settings, kind);
        out.lodUsed = geometry.lodUsed;
        out.lodCount = geometry.lodCount;
        return out;
    }

    SolidBounds MeasureSolid(const std::string& geometryPath, int lod, const std::vector<int>& excludeSlots)
    {
        SolidBounds out;
        SolidGeometry geometry;
        if (!ReadSolidGeometry(geometryPath, lod, excludeSlots, geometry) || geometry.positions.empty()) { return out; }
        Math::float3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const Math::float3& p : geometry.positions)
        {
            lo = Math::float3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Math::float3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        out.min = lo;
        out.max = hi;
        out.valid = true;
        return out;
    }

    void ApplyRollingResistance(JPH::BodyInterface& bodies, JPH::BodyID id, float resistance, float radius, float gravity, float dt, float& lastVelocityY)
    {
        JPH::Vec3 linear, angular;
        bodies.GetLinearAndAngularVelocity(id, linear, angular);
        const float pushed = linear.GetY() - lastVelocityY - gravity * dt; // what gravity alone does not explain
        lastVelocityY = linear.GetY();
        if (std::fabs(pushed) <= 0.5f * std::fabs(gravity * dt)) { return; } // in flight
        // A ball rolling down a slope gains 5/7 g sin(a) of speed: losing that much a second holds it at the slope whose sine is the resistance.
        const float lost = (5.0f / 7.0f) * resistance * std::fabs(gravity) * dt;
        const float speed = linear.Length();
        const float spin = angular.Length();
        const float lostSpin = lost / std::max(radius, 0.01f);
        const JPH::Vec3 keptLinear = speed > lost ? linear * (1.0f - lost / speed) : JPH::Vec3::sZero();
        const JPH::Vec3 keptAngular = spin > lostSpin ? angular * (1.0f - lostSpin / spin) : JPH::Vec3::sZero();
        bodies.SetLinearAndAngularVelocity(id, keptLinear, keptAngular);
        lastVelocityY = keptLinear.GetY();
    }

    JPH::RefConst<JPH::Shape> ScaledInstance(const JPH::Shape& shape, const Math::float3& scale, std::string& error)
    {
        constexpr float kSame = 1.0e-4f;
        if (std::fabs(scale.x - 1.0f) < kSame && std::fabs(scale.y - 1.0f) < kSame && std::fabs(scale.z - 1.0f) < kSame)
        {
            return &shape;
        }
        const JPH::Vec3 requested = ToJolt(scale);
        // Not every shape takes every scale (a sphere takes a uniform one): the nearest it does.
        const bool exact = shape.IsValidScale(requested);
        const JPH::Vec3 valid = exact ? requested : shape.MakeScaleValid(requested);
        if (!exact)
        {
            LOG_WARNING_ONCE_PER_MESSAGE(logging::LogCategory::Physics, "physics: a shape cannot take the scale {:.3f} {:.3f} {:.3f}, it collides at {:.3f} {:.3f} {:.3f}",
                scale.x, scale.y, scale.z, valid.GetX(), valid.GetY(), valid.GetZ());
        }
        const JPH::Shape::ShapeResult result = shape.ScaleShape(valid);
        if (result.HasError())
        {
            error = result.GetError().c_str();
            return nullptr;
        }
        return result.Get();
    }

    CollisionSettings BakeScale(const CollisionSettings& settings, const Math::float3& scale)
    {
        constexpr float kToRad = 3.14159265358979f / 180.0f;
        constexpr float kToDeg = 180.0f / 3.14159265358979f;
        const auto length = [](const Math::float3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
        CollisionSettings out = settings;
        for (CollisionPrimitive& p : out.primitives)
        {
            const Math::mat4 r = Math::mat4::RotationFromEulerXYZRad(Math::float3(p.rotationDeg.x * kToRad, p.rotationDeg.y * kToRad, p.rotationDeg.z * kToRad));
            // A row is where its axis goes (row vectors), and the scale acts after the rotation.
            const Math::float3 axisX(r.m._11 * scale.x, r.m._12 * scale.y, r.m._13 * scale.z);
            const Math::float3 axisY(r.m._21 * scale.x, r.m._22 * scale.y, r.m._23 * scale.z);
            const Math::float3 axisZ(r.m._31 * scale.x, r.m._32 * scale.y, r.m._33 * scale.z);
            const float lengthX = length(axisX), lengthY = length(axisY), lengthZ = length(axisZ);
            p.center = Math::float3(p.center.x * scale.x, p.center.y * scale.y, p.center.z * scale.z);
            const float across = std::max(lengthX, lengthZ);
            switch (p.kind)
            {
            case PrimitiveKind::Box: p.halfExtents = Math::float3(p.halfExtents.x * lengthX, p.halfExtents.y * lengthY, p.halfExtents.z * lengthZ); break;
            case PrimitiveKind::Sphere: p.radius *= std::max({ std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z) }); break;
            case PrimitiveKind::Capsule:
            {
                const float tip = (p.halfHeight + p.radius) * lengthY; // where the capsule ends, whatever its radius becomes
                p.radius *= across;
                p.halfHeight = std::max(tip - p.radius, 0.0f);
                break;
            }
            case PrimitiveKind::Cylinder:
                p.halfHeight *= lengthY;
                p.radius *= across;
                break;
            }
            if (lengthX < 1.0e-6f || lengthY < 1.0e-6f) { continue; } // flat: the size above is all there is to say
            // The primitive is symmetric, so a mirror needs no other form: its axis keeps its line (y first, it is a capsule's and a cylinder's), the rest squares up round it.
            const Math::float3 y(axisY.x / lengthY, axisY.y / lengthY, axisY.z / lengthY);
            const float along = axisX.x * y.x + axisX.y * y.y + axisX.z * y.z;
            const Math::float3 rest(axisX.x - y.x * along, axisX.y - y.y * along, axisX.z - y.z * along);
            const float restLength = length(rest);
            if (restLength < 1.0e-6f) { continue; }
            const Math::float3 x(rest.x / restLength, rest.y / restLength, rest.z / restLength);
            const Math::float3 z(x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x); // x cross y: a rotation whatever the mirror did
            Math::mat4 rotation = Math::mat4::Identity();
            rotation.m._11 = x.x; rotation.m._12 = x.y; rotation.m._13 = x.z;
            rotation.m._21 = y.x; rotation.m._22 = y.y; rotation.m._23 = y.z;
            rotation.m._31 = z.x; rotation.m._32 = z.y; rotation.m._33 = z.z;
            const Math::float3 euler = Math::mat4::EulerXYZRadFromRotation(rotation);
            p.rotationDeg = Math::float3(euler.x * kToDeg, euler.y * kToDeg, euler.z * kToDeg);
        }
        return out;
    }
} // namespace physics
