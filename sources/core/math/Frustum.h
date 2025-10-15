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

    static Frustum FromViewProj(const Math::mat4& view, const Math::mat4& proj)
    {
        const Math::mat4 invView = Math::mat4::Inverse(view);
        return FromInvViewProj(invView, proj);
    }

    static Frustum FromInvViewProj(const Math::mat4& invView, const Math::mat4& proj)
    {
        Frustum frustum;
        frustum.Build(invView, proj);
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
    void Build(const Math::mat4& invView, const Math::mat4& proj)
    {
        DirectX::BoundingFrustum viewFrustum;
        DirectX::BoundingFrustum::CreateFromMatrix(viewFrustum, proj.xm());

        DirectX::BoundingFrustum worldFrustum;
        viewFrustum.Transform(worldFrustum, invView.xm());

        frustum_ = worldFrustum;
        valid_ = true;
    }

    DirectX::BoundingFrustum frustum_{};
    bool valid_ = false;
};

