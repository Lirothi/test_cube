#include "physics/PhysicsInternal.h"

#include <algorithm>
#include <cmath>

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
        if (requested == ShapeKind::Auto) { return dynamicBody ? ShapeKind::Hull : ShapeKind::Mesh; }
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
        if (kind == ShapeKind::None || geometry.indices.size() < 3) { return out; }

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

    JPH::RefConst<JPH::Shape> ScaledInstance(const JPH::Shape& shape, const Math::float3& scale, std::string& error)
    {
        constexpr float kSame = 1.0e-4f;
        if (std::fabs(scale.x - 1.0f) < kSame && std::fabs(scale.y - 1.0f) < kSame && std::fabs(scale.z - 1.0f) < kSame)
        {
            return &shape;
        }
        const JPH::Vec3 requested = ToJolt(scale);
        // Not every shape takes every scale (a sphere takes a uniform one): the nearest it does.
        const JPH::Vec3 valid = shape.IsValidScale(requested) ? requested : shape.MakeScaleValid(requested);
        const JPH::Shape::ShapeResult result = shape.ScaleShape(valid);
        if (result.HasError())
        {
            error = result.GetError().c_str();
            return nullptr;
        }
        return result.Get();
    }
} // namespace physics
