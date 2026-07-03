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

        DirectX::BoundingSphere sphere{ center.xf(), radius };
        if (type_ == Type::Perspective)
        {
            return frustum_.Intersects(sphere);
        }

        if (type_ == Type::OrthoBox)
        {
            return orthoBox_.Intersects(sphere);
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
    }

    DirectX::BoundingFrustum frustum_{};
    DirectX::BoundingOrientedBox orthoBox_{};
    Type type_ = Type::Invalid;
    bool valid_ = false;
};

