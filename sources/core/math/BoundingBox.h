#pragma once

#include <cfloat>

#include "core/math/Math.h"

class BoundingBox
{
public:
    BoundingBox() noexcept
        : min_(Math::float3(+FLT_MAX, +FLT_MAX, +FLT_MAX))
        , max_(Math::float3(-FLT_MAX, -FLT_MAX, -FLT_MAX))
    {
    }

    BoundingBox(const Math::float3& minPt, const Math::float3& maxPt) noexcept
        : min_(minPt)
        , max_(maxPt)
    {
    }

    static BoundingBox Empty() noexcept
    {
        return BoundingBox();
    }

    void Reset() noexcept
    {
        min_ = Math::float3(+FLT_MAX, +FLT_MAX, +FLT_MAX);
        max_ = Math::float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    }

    bool IsValid() const noexcept
    {
        return min_.x <= max_.x && min_.y <= max_.y && min_.z <= max_.z;
    }

    void Expand(const Math::float3& point) noexcept
    {
        if (!IsValid())
        {
            min_ = point;
            max_ = point;
            return;
        }

        min_ = Math::float3::Min(min_, point);
        max_ = Math::float3::Max(max_, point);
    }

    void Expand(const BoundingBox& other) noexcept
    {
        if (!other.IsValid())
        {
            return;
        }
        if (!IsValid())
        {
            min_ = other.min_;
            max_ = other.max_;
            return;
        }

        min_ = Math::float3::Min(min_, other.min_);
        max_ = Math::float3::Max(max_, other.max_);
    }

    Math::float3 GetMin() const noexcept { return min_; }
    Math::float3 GetMax() const noexcept { return max_; }

    Math::float3 GetCenter() const noexcept
    {
        return (min_ + max_) * 0.5f;
    }

    Math::float3 GetHalfExtents() const noexcept
    {
        return (max_ - min_) * 0.5f;
    }

    Math::float3 GetExtents() const noexcept
    {
        return max_ - min_;
    }

    float GetRadius() const noexcept
    {
        return GetHalfExtents().Length();
    }

    void GetCorners(Math::float3(&corners)[8]) const noexcept
    {
        const Math::float3 min = min_;
        const Math::float3 max = max_;

        corners[0] = Math::float3(min.x, min.y, min.z);
        corners[1] = Math::float3(max.x, min.y, min.z);
        corners[2] = Math::float3(min.x, max.y, min.z);
        corners[3] = Math::float3(max.x, max.y, min.z);
        corners[4] = Math::float3(min.x, min.y, max.z);
        corners[5] = Math::float3(max.x, min.y, max.z);
        corners[6] = Math::float3(min.x, max.y, max.z);
        corners[7] = Math::float3(max.x, max.y, max.z);
    }

    BoundingBox Transform(const Math::mat4& transform) const noexcept
    {
        if (!IsValid())
        {
            return *this;
        }

        BoundingBox result = BoundingBox::Empty();
        Math::float3 corners[8];
        GetCorners(corners);
        for (const auto& corner : corners)
        {
            result.Expand(transform.TransformPoint(corner));
        }
        return result;
    }

private:
    Math::float3 min_;
    Math::float3 max_;
};
