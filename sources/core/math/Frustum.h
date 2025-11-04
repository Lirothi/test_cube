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
        frustum.BuildOrtho(invView, minX, maxX, minY, maxY, nearPlane, farPlane);
        return frustum;
    }

    bool IsValid() const { return valid_; }

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

    DirectX::BoundingFrustum frustum_{};
    DirectX::BoundingOrientedBox orthoBox_{};
    Type type_ = Type::Invalid;
    bool valid_ = false;
};

