#pragma once

#include <algorithm>

#include <DirectXCollision.h>

#include "core/math/Math.h"
#include "core/math/AABB.h"

// Lightweight wrapper around DirectX collision primitives that exposes
// intersection tests against the engine's AABB type.
class Frustum
{
public:
    Frustum() = default;

    static Frustum FromViewProj(
        const Math::mat4& view,
        const Math::mat4& proj,
        float nearPlane = 0.0f,
        float farPlane = 0.0f)
    {
        const Math::mat4 invView = Math::mat4::Inverse(view);
        return FromInvViewProj(invView, proj, nearPlane, farPlane);
    }

    static Frustum FromInvViewProj(
        const Math::mat4& invView,
        const Math::mat4& proj,
        float nearPlane = 0.0f,
        float farPlane = 0.0f)
    {
        Frustum frustum;
        frustum.Build(invView, proj, nearPlane, farPlane);
        return frustum;
    }

    static Frustum FromOrthoBounds(
        const Math::mat4& invView,
        float minX,
        float maxX,
        float minY,
        float maxY,
        float nearPlane,
        float farPlane)
    {
        Frustum frustum;
	    frustum.BuildOrthoLs(invView, minX, maxX, minY, maxY, nearPlane, farPlane);
        return frustum;
    }

    static Frustum FromOrthoBounds(
        const Math::mat4& invView,
        float halfX,
        float halfY,
        float halfZ,
		const float3& centerWS)
    {
        Frustum frustum;
        frustum.BuildOrthoWs(invView, halfX, halfY, halfZ, centerWS);
        return frustum;
    }

    bool IsValid() const { return valid_; }

    bool GetCorners(Math::float3 outCorners[8]) const
    {
        if (!outCorners || !valid_)
        {
            return false;
        }

        DirectX::XMFLOAT3 corners[8] = {};
        switch (type_)
        {
        case Type::Perspective:
            frustum_.GetCorners(corners);
            break;
        case Type::OrthoBox:
            orthoBox_.GetCorners(corners);
            break;
        default:
            return false;
        }

        for (int i = 0; i < 8; ++i)
        {
            outCorners[i] = Math::float3(corners[i]);
        }

        return true;
    }

    bool Intersects(const AABB& bounds) const
    {
        if (!valid_)
        {
            return true;
        }

        if (!bounds.IsValid())
        {
            return true;
        }

        // Conservative AABB-vs-planes test against the 6 precomputed inward-facing planes
        // (unit normals, "inside" == n·p + d >= 0). Pure scalar float: no DirectXMath and no
        // per-call BoundingBox construction, so it is roughly an order of magnitude cheaper
        // than BoundingOrientedBox/BoundingFrustum::Intersects in unoptimized/debug builds,
        // where the DirectXMath path is neither inlined nor vectorized. A box is the
        // intersection of its 6 slabs and a perspective frustum of its 6 half-spaces, so the
        // positive-vertex test is exact up to the usual AABB over-inclusion — it never culls
        // an object that should be visible.
        const Math::float3 c = bounds.GetCenter();
        const Math::float3 e = bounds.GetHalfExtents();
        for (const Math::float4& plane : planes_)
        {
            const float signedDist = plane.x * c.x + plane.y * c.y + plane.z * c.z + plane.w;
            const float projRadius = e.x * std::fabs(plane.x) + e.y * std::fabs(plane.y) + e.z * std::fabs(plane.z);
            if (signedDist + projRadius < 0.0f)
            {
                return false;
            }
        }

        return true;
    }

    // Benchmark-only: the pre-optimization DirectXMath path, kept so cull-benchmark can A/B
    // it against the plane test in one binary on identical data. Not used by the engine.
    bool IntersectsLegacy(const AABB& bounds) const
    {
        if (!valid_)
        {
            return true;
        }
        if (!bounds.IsValid())
        {
            return true;
        }

        DirectX::BoundingBox box{};
        box.Center = bounds.GetCenter().xf();
        box.Extents = bounds.GetHalfExtents().xf();

        if (type_ == Type::Perspective)
        {
            return frustum_.Intersects(box);
        }
        if (type_ == Type::OrthoBox)
        {
            return orthoBox_.Intersects(box);
        }
        return true;
    }

    // Sphere-vs-frustum test. Used to cull a light's influence volume: a light
    // whose reach sphere does not touch the view frustum cannot affect any visible
    // pixel, so it never needs a shadow. An invalid frustum conservatively passes.
    bool Intersects(const Math::float3& center, float radius) const
    {
        if (!valid_)
        {
            return true;
        }

        // Sphere-vs-planes against the same precomputed unit-normal planes (see the AABB
        // overload). Cheap scalar path, conservative in the same way.
        for (const Math::float4& plane : planes_)
        {
            const float signedDist = plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
            if (signedDist + radius < 0.0f)
            {
                return false;
            }
        }

        return true;
    }

private:
    enum class Type
    {
        Invalid,
        Perspective,
        OrthoBox,
    };

    void Build(
        const Math::mat4& invView,
        const Math::mat4& proj,
        float nearPlane,
        float farPlane)
    {
        DirectX::BoundingFrustum viewFrustum;
        DirectX::BoundingFrustum::CreateFromMatrix(viewFrustum, proj.xm());

        if (nearPlane > 0.0f && farPlane > nearPlane)
        {
            viewFrustum.Near = nearPlane;
            viewFrustum.Far = farPlane;
        }

        DirectX::BoundingFrustum worldFrustum;
        viewFrustum.Transform(worldFrustum, invView.xm());

        frustum_ = worldFrustum;
        type_ = Type::Perspective;
        valid_ = true;
        BuildPlanes();
    }

    void BuildOrthoLs(
        const Math::mat4& invView,
        float minX,
        float maxX,
        float minY,
        float maxY,
        float nearPlane,
        float farPlane)
    {
        const Math::float3 centerLS(
            0.5f * (minX + maxX),
            0.5f * (minY + maxY),
            0.5f * (nearPlane + farPlane));

        const Math::float3 halfExtents(
            0.5f * std::max(0.0f, maxX - minX),
            0.5f * std::max(0.0f, maxY - minY),
            0.5f * std::max(0.0f, farPlane - nearPlane));

        // Transform center into world space.
        const Math::float3 centerWS = (invView * Math::float4(centerLS, 1.0f)).xyz();

        DirectX::XMVECTOR scale{};
        DirectX::XMVECTOR rotation{};
        DirectX::XMVECTOR translation{};
        DirectX::XMMatrixDecompose(&scale, &rotation, &translation, invView.xm());
        rotation = DirectX::XMQuaternionNormalize(rotation);

        orthoBox_.Center = centerWS.xf();
        orthoBox_.Extents = halfExtents.xf();
        DirectX::XMStoreFloat4(&orthoBox_.Orientation, rotation);

        type_ = Type::OrthoBox;
        valid_ = true;
        BuildPlanes();
    }

    void BuildOrthoWs(
        const Math::mat4& invView,
        float halfX,
        float halfY,
        float halfZ,
        const float3& centerWS)
    {
        const Math::float3 halfExtents(halfX, halfY, halfZ);

        DirectX::XMVECTOR scale{};
        DirectX::XMVECTOR rotation{};
        DirectX::XMVECTOR translation{};
        DirectX::XMMatrixDecompose(&scale, &rotation, &translation, invView.xm());
        rotation = DirectX::XMQuaternionNormalize(rotation);

        orthoBox_.Center = centerWS.xf();
        orthoBox_.Extents = halfExtents.xf();
        DirectX::XMStoreFloat4(&orthoBox_.Orientation, rotation);

        type_ = Type::OrthoBox;
        valid_ = true;
        BuildPlanes();
    }

    // Extract the 6 bounding planes once at build time so per-object culling is a handful of
    // scalar dot products instead of a DirectXMath OBB/frustum test. Planes are stored with
    // unit normals pointing inward (inside == n·p + d >= 0); a point known to be inside (the
    // corner centroid) is used to orient them regardless of DirectX's winding.
    void BuildPlanes()
    {
        DirectX::XMFLOAT3 dxCorners[8] = {};
        if (type_ == Type::Perspective)
        {
            frustum_.GetCorners(dxCorners);
        }
        else if (type_ == Type::OrthoBox)
        {
            orthoBox_.GetCorners(dxCorners);
        }
        else
        {
            return;
        }

        Math::float3 inside(0.0f, 0.0f, 0.0f);
        for (const DirectX::XMFLOAT3& corner : dxCorners)
        {
            inside += Math::float3(corner);
        }
        inside = inside * (1.0f / 8.0f);

        Math::float4 raw[6] = {};
        if (type_ == Type::Perspective)
        {
            DirectX::XMVECTOR p[6] = {};
            frustum_.GetPlanes(&p[0], &p[1], &p[2], &p[3], &p[4], &p[5]);
            for (int i = 0; i < 6; ++i)
            {
                DirectX::XMFLOAT4 f{};
                DirectX::XMStoreFloat4(&f, p[i]);
                raw[i] = Math::float4(f.x, f.y, f.z, f.w);
            }
        }
        else
        {
            const DirectX::XMVECTOR orientation = DirectX::XMLoadFloat4(&orthoBox_.Orientation);
            const Math::float3 center(orthoBox_.Center);
            const float ext[3] = { orthoBox_.Extents.x, orthoBox_.Extents.y, orthoBox_.Extents.z };
            const DirectX::XMVECTOR localAxes[3] = {
                DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
                DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
                DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
            };
            for (int axis = 0; axis < 3; ++axis)
            {
                const Math::float3 n = Math::float3::FromXM(DirectX::XMVector3Rotate(localAxes[axis], orientation));
                // Both faces of this slab share the axis normal; the centroid flip below points each inward.
                raw[axis * 2 + 0] = Math::float4(n, -n.Dot(center + n * ext[axis]));
                raw[axis * 2 + 1] = Math::float4(n, -n.Dot(center - n * ext[axis]));
            }
        }

        for (int i = 0; i < 6; ++i)
        {
            Math::float3 n(raw[i].x, raw[i].y, raw[i].z);
            float d = raw[i].w;
            const float len = n.Length();
            if (len > 1e-8f)
            {
                const float inv = 1.0f / len;
                n = n * inv;
                d *= inv;
            }
            if (n.Dot(inside) + d < 0.0f)
            {
                n = n * -1.0f;
                d = -d;
            }
            planes_[i] = Math::float4(n, d);
        }
    }

    DirectX::BoundingFrustum frustum_{};
    DirectX::BoundingOrientedBox orthoBox_{};
    Type type_ = Type::Invalid;
    bool valid_ = false;
    Math::float4 planes_[6] = {};
};

