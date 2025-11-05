#include "app/scene/SceneRenderQueue.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/math/Math.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    constexpr std::array<SceneRenderQueue::BucketType, 2> kTransparentBuckets = {
        SceneRenderQueue::BucketType::TransparentSimple,
        SceneRenderQueue::BucketType::TransparentComplex,
    };

    constexpr size_t TransparentIndex(SceneRenderQueue::BucketType type)
    {
        return type == SceneRenderQueue::BucketType::TransparentSimple ? 0 : 1;
    }

    constexpr bool IsTransparentBucket(SceneRenderQueue::BucketType type)
    {
        return type == SceneRenderQueue::BucketType::TransparentSimple
            || type == SceneRenderQueue::BucketType::TransparentComplex;
    }
}

SceneRenderQueue::SceneRenderQueue() = default;

void SceneRenderQueue::Clear()
{
    for (auto& bucket : buckets_)
    {
        bucket.clear();
    }
    for (auto& bucket : visibleBuckets_)
    {
        bucket.clear();
    }
    for (auto& entries : transparentEntries_)
    {
        entries.clear();
    }
}

void SceneRenderQueue::Bucketize(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects, uint32_t renderLayerMask, bool filterShadowCaster)
{
    Clear();

    for (const auto& obj : objects)
    {
        if (!obj)
        {
            continue;
        }

        if ((obj->GetRenderLayerMask() & renderLayerMask) == 0)
        {
            continue;
        }

        if (filterShadowCaster && !obj->CastsShadow())
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

    }
}

float SceneRenderQueue::ComputeDepth(const mat4& view, const TransparentEntry& entry) const
{
    if (!entry.base)
    {
        return std::numeric_limits<float>::infinity();
    }

    const AABB& boundsWS = entry.base->GetWorldBounds();
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
        auto& visibleBucket = visibleBuckets_[ToIndex(bucketType)];
        auto& entries = transparentEntries_[TransparentIndex(bucketType)];

        if (entries.size() < 2)
        {
            if (!entries.empty())
            {
                entries[0].depth = ComputeDepth(view, entries[0]);
            }
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

        size_t writeIndex = 0;
        for (const auto& entry : entries)
        {
            visibleBucket[writeIndex++] = entry.base;
        }
    }
}

void SceneRenderQueue::Cull(const Frustum& frustum, bool clampDepthRange,
    const float3& cameraPosition, const float3& cameraDirection,
    float minDepth, float maxDepth)
{
    //CPU_SCOPE(ProfilerScopes::kService3);
    for (auto& bucket : visibleBuckets_)
    {
        bucket.clear();
    }
    for (auto& entries : transparentEntries_)
    {
        entries.clear();
    }

    for (size_t bucketIndex = 0; bucketIndex < buckets_.size(); ++bucketIndex)
    {
        const auto& bucket = buckets_[bucketIndex];
        auto& visibleBucket = visibleBuckets_[bucketIndex];
        visibleBucket.reserve(bucket.size());

        const BucketType bucketType = static_cast<BucketType>(bucketIndex);
        const bool transparentBucket = IsTransparentBucket(bucketType);
        std::vector<TransparentEntry>* transparentEntries = nullptr;
        if (transparentBucket)
        {
            transparentEntries = &transparentEntries_[TransparentIndex(bucketType)];
            transparentEntries->reserve(bucket.size());
        }

        for (auto* obj : bucket)
        {
            if (!obj)
            {
                continue;
            }

            const AABB& bounds = obj->GetWorldBounds();
            bool visible = !frustum.IsValid() || !bounds.IsValid() || frustum.Intersects(bounds);
            if (visible && clampDepthRange && bounds.IsValid())
            {
                const float3 center = bounds.GetCenter();
                const float radius = bounds.GetRadius();
                const float3 toCenter = center - cameraPosition;
                //const float depth = toCenter.Dot(cameraDirection);
                const float depth = toCenter.Length();
                const float objMinDepth = depth - radius;
                const float objMaxDepth = depth + radius;

                constexpr float minDepthDist = 50.0f;
                constexpr float overlap = 10.0f;

                if (objMinDepth >= minDepthDist && (objMaxDepth < (minDepth - overlap) || objMinDepth > (maxDepth + overlap)))
                {
                    visible = false;
                }
            }

            if (visible)
            {
                visibleBucket.push_back(obj);
                if (transparentEntries)
                {
                    transparentEntries->push_back({ obj, 0.0f });
                }
            }
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

SceneRenderQueue::ObjectBucket& SceneRenderQueue::GetVisibleBucket(BucketType type)
{
    return visibleBuckets_[ToIndex(type)];
}
