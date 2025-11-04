#pragma once

#include <algorithm>
#include <array>
#include <cmath>

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
        frustum.BuildOrtho(invView, minX, maxX, minY, maxY, nearPlane, farPlane);
        return frustum;
    }

    static Frustum FromCorners(const std::array<Math::float3, 8>& corners)
    {
        Frustum frustum;
        frustum.BuildFromCorners(corners);
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
        case Type::CornerPoints:
            for (int i = 0; i < 8; ++i)
            {
                corners[i] = cornerPoints_[i].xf();
            }
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

        if (type_ == Type::CornerPoints)
        {
            const Math::float3 center = bounds.GetCenter();
            const Math::float3 extents = bounds.GetHalfExtents();

            for (const auto& plane : planes_)
            {
                const Math::float3 normal(plane.x, plane.y, plane.z);
                const float distance = normal.Dot(center) + plane.w;
                const float radius = std::fabs(normal.x) * extents.x
                    + std::fabs(normal.y) * extents.y
                    + std::fabs(normal.z) * extents.z;

                if (distance + radius < 0.0f)
                {
                    return false;
                }
            }

            return true;
        }

        return true;
    }

private:
    enum class Type
    {
        Invalid,
        Perspective,
        OrthoBox,
        CornerPoints,
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
    }

    void BuildOrtho(
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
    }

    void BuildFromCorners(const std::array<Math::float3, 8>& corners)
    {
        cornerPoints_ = corners;

        Math::float3 center(0.0f, 0.0f, 0.0f);
        for (const auto& c : corners)
        {
            center += c;
        }
        center = center / static_cast<float>(corners.size());

        bool ok = true;
        planes_[0] = CreatePlane(corners[0], corners[2], corners[4], center, ok); // Near
        planes_[1] = CreatePlane(corners[1], corners[5], corners[3], center, ok); // Far
        planes_[2] = CreatePlane(corners[0], corners[6], corners[7], center, ok); // Left
        planes_[3] = CreatePlane(corners[2], corners[3], corners[5], center, ok); // Right
        planes_[4] = CreatePlane(corners[4], corners[7], corners[5], center, ok); // Top
        planes_[5] = CreatePlane(corners[0], corners[3], corners[1], center, ok); // Bottom

        if (ok)
        {
            type_ = Type::CornerPoints;
            valid_ = true;
        }
        else
        {
            type_ = Type::Invalid;
            valid_ = false;
        }
    }

    static Math::float4 CreatePlane(
        const Math::float3& a,
        const Math::float3& b,
        const Math::float3& c,
        const Math::float3& insidePoint,
        bool& ok)
    {
        if (!ok)
        {
            return Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
        }

        Math::float3 normal = (b - a).Cross(c - a);
        const float length = normal.Length();
        if (length < Math::EPS)
        {
            ok = false;
            return Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
        }

        normal = normal / length;
        float d = -normal.Dot(a);
        if (normal.Dot(insidePoint) + d < 0.0f)
        {
            normal = normal * -1.0f;
            d = -d;
        }

        return Math::float4(normal, d);
    }

    DirectX::BoundingFrustum frustum_{};
    DirectX::BoundingOrientedBox orthoBox_{};
    std::array<Math::float3, 8> cornerPoints_{};
    std::array<Math::float4, 6> planes_{};
    Type type_ = Type::Invalid;
    bool valid_ = false;
};

