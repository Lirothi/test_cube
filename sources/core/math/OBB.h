#pragma once

#include <cstddef>
#include <cmath>

#include "core/math/AABB.h"
#include "core/math/Math.h"

class OBB
{
public:
    OBB() noexcept = default;

    OBB(const Math::float3& center, const Math::float3 (&axes)[3], const Math::float3& halfExtents) noexcept
    {
        Set(center, axes, halfExtents);
    }

    void Reset() noexcept
    {
        center_ = Math::float3(0.0f, 0.0f, 0.0f);
        axes_[0] = Math::float3(1.0f, 0.0f, 0.0f);
        axes_[1] = Math::float3(0.0f, 1.0f, 0.0f);
        axes_[2] = Math::float3(0.0f, 0.0f, 1.0f);
        halfExtents_ = Math::float3(0.0f, 0.0f, 0.0f);
        valid_ = false;
    }

    void Set(const Math::float3& center, const Math::float3 (&axes)[3], const Math::float3& halfExtents) noexcept
    {
        center_ = center;
        valid_ = true;
        for (std::size_t i = 0; i < 3; ++i)
        {
            const Math::float3 axis = axes[i].Length() > Math::EPS ? axes[i].Normalized() : Math::float3(0.0f, 0.0f, 0.0f);
            if (axis.Length() <= Math::EPS)
            {
                valid_ = false;
            }
            axes_[i] = axis;
        }
        halfExtents_ = halfExtents;
        if (halfExtents_.x < Math::EPS || halfExtents_.y < Math::EPS || halfExtents_.z < Math::EPS)
        {
            valid_ = false;
        }
    }

    bool IsValid() const noexcept { return valid_; }

    const Math::float3& GetCenter() const noexcept { return center_; }
    const Math::float3& GetHalfExtents() const noexcept { return halfExtents_; }
    const Math::float3& GetAxis(std::size_t index) const noexcept { return axes_[index % 3]; }
    const Math::float3* GetAxes() const noexcept { return axes_; }

    bool Intersects(const AABB& bounds) const noexcept
    {
        if (!valid_ || !bounds.IsValid())
        {
            return false;
        }

        const Math::float3 aCenter = bounds.GetCenter();
        const Math::float3 aHalf = bounds.GetHalfExtents();
        const Math::float3 bHalf = halfExtents_;

        const float a[3] = { aHalf.x, aHalf.y, aHalf.z };
        const float b[3] = { bHalf.x, bHalf.y, bHalf.z };

        // World axes for the axis-aligned box.
        const Math::float3 aAxes[3] = {
            Math::float3(1.0f, 0.0f, 0.0f),
            Math::float3(0.0f, 1.0f, 0.0f),
            Math::float3(0.0f, 0.0f, 1.0f)
        };

        // Rotation matrix that expresses the OBB axes in the AABB basis.
        float R[3][3];
        float AbsR[3][3];
        constexpr float kEpsilon = 1e-5f;
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                R[i][j] = aAxes[i].Dot(axes_[j]);
                AbsR[i][j] = std::fabs(R[i][j]) + kEpsilon;
            }
        }

        // Translation of the OBB center expressed in the AABB basis.
        const Math::float3 diff = center_ - aCenter;
        const float t[3] = {
            diff.Dot(aAxes[0]),
            diff.Dot(aAxes[1]),
            diff.Dot(aAxes[2])
        };

        for (int i = 0; i < 3; ++i)
        {
            const float ra = a[i];
            const float rb = b[0] * AbsR[i][0] + b[1] * AbsR[i][1] + b[2] * AbsR[i][2];
            if (std::fabs(t[i]) > ra + rb)
            {
                return false;
            }
        }

        for (int j = 0; j < 3; ++j)
        {
            const float ra = a[0] * AbsR[0][j] + a[1] * AbsR[1][j] + a[2] * AbsR[2][j];
            const float rb = b[j];
            const float proj = t[0] * R[0][j] + t[1] * R[1][j] + t[2] * R[2][j];
            if (std::fabs(proj) > ra + rb)
            {
                return false;
            }
        }

        for (int i = 0; i < 3; ++i)
        {
            const int i1 = (i + 1) % 3;
            const int i2 = (i + 2) % 3;
            for (int j = 0; j < 3; ++j)
            {
                const int j1 = (j + 1) % 3;
                const int j2 = (j + 2) % 3;
                const float ra = a[i1] * AbsR[i2][j] + a[i2] * AbsR[i1][j];
                const float rb = b[j1] * AbsR[i][j2] + b[j2] * AbsR[i][j1];
                const float term = t[i2] * R[i1][j] - t[i1] * R[i2][j];
                if (std::fabs(term) > ra + rb)
                {
                    return false;
                }
            }
        }

        return true;
    }

private:
    Math::float3 center_ = Math::float3(0.0f, 0.0f, 0.0f);
    Math::float3 axes_[3] = {
        Math::float3(1.0f, 0.0f, 0.0f),
        Math::float3(0.0f, 1.0f, 0.0f),
        Math::float3(0.0f, 0.0f, 1.0f)
    };
    Math::float3 halfExtents_ = Math::float3(0.0f, 0.0f, 0.0f);
    bool valid_ = false;
};

