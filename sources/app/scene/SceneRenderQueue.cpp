#include "app/scene/SceneRenderQueue.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    constexpr std::array<SceneRenderQueue::BucketType, 2> kTransparentBuckets = {
        SceneRenderQueue::BucketType::TransparentSimple,
        SceneRenderQueue::BucketType::TransparentComplex,
    };

    constexpr size_t TransparentIndex(SceneRenderQueue::BucketType type)
    {
        return static_cast<size_t>(type) - static_cast<size_t>(SceneRenderQueue::BucketType::TransparentSimple);
    }
}

SceneRenderQueue::SceneRenderQueue() = default;

void SceneRenderQueue::Clear()
{
    for (auto& bucket : buckets_)
    {
        bucket.clear();
    }
    for (auto& entries : transparentEntries_)
    {
        entries.clear();
    }
}

void SceneRenderQueue::Bucketize(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects)
{
    Clear();

    for (const auto& obj : objects)
    {
        if (!obj)
        {
            continue;
        }

        const bool transparent = obj->IsTransparent();
        const bool simple = obj->IsSimpleRender();
        const BucketType type = transparent
            ? (simple ? BucketType::TransparentSimple : BucketType::TransparentComplex)
            : (simple ? BucketType::OpaqueSimple : BucketType::OpaqueComplex);

        auto& bucket = buckets_[ToIndex(type)];
        bucket.push_back(obj.get());

        if (transparent)
        {
            TransparentEntry entry{};
            entry.base = obj.get();
            transparentEntries_[TransparentIndex(type)].push_back(entry);
        }
    }
}

float SceneRenderQueue::ComputeDepth(const mat4& view, const TransparentEntry& entry) const
{
    if (!entry.base)
    {
        return -std::numeric_limits<float>::infinity();
    }

    const BoundingBox& boundsWS = entry.base->GetWorldBounds();
    if (!boundsWS.IsValid())
    {
        return std::numeric_limits<float>::infinity();
    }

    const Math::float3 centerVS = view.TransformPoint(boundsWS.GetCenter());
    return centerVS.z;
}

void SceneRenderQueue::SortTransparent(const mat4& view)
{
    for (const auto bucketType : kTransparentBuckets)
    {
        auto& entries = transparentEntries_[TransparentIndex(bucketType)];
        if (entries.empty())
        {
            continue;
        }

        for (auto& entry : entries)
        {
            entry.depth = ComputeDepth(view, entry);
        }

        std::sort(entries.begin(), entries.end(), [](const TransparentEntry& lhs, const TransparentEntry& rhs)
        {
            const float diff = lhs.depth - rhs.depth;
            if (std::fabs(diff) < 1e-4f)
            {
                return lhs.base < rhs.base;
            }
            return lhs.depth > rhs.depth;
        });

        auto& bucket = buckets_[ToIndex(bucketType)];
        for (size_t i = 0; i < entries.size(); ++i)
        {
            bucket[i] = entries[i].base;
        }
    }
}

const SceneRenderQueue::ObjectBucket& SceneRenderQueue::GetBucket(BucketType type) const
{
    return buckets_[ToIndex(type)];
}

SceneRenderQueue::ObjectBucket& SceneRenderQueue::GetBucket(BucketType type)
{
    return buckets_[ToIndex(type)];
}
