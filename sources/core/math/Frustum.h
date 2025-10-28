#pragma once

#include <DirectXCollision.h>

#include "core/math/Math.h"
#include "core/math/AABB.h"

// Lightweight wrapper around DirectX::BoundingFrustum that exposes
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
        return frustum_.Intersects(box);
    }

private:
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
        valid_ = true;
    }

    DirectX::BoundingFrustum frustum_{};
    bool valid_ = false;
};

