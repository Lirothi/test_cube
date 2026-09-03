#include "app/scene/SceneRenderQueue.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/math/Math.h"
#include "app/camera/Camera.h"
#include "rendering/meshes/LodSelect.h" // render::UpdateLodFovScale (per-frame FOV normalization)
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/renderables/InstancedDrawBatch.h"
#include "rendering/renderables/IInstanceable.h"
#include "rendering/renderables/InstanceTypes.h"

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
SceneRenderQueue::~SceneRenderQueue() = default;

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
    for (auto& bucket : instancingRewriteBuckets_)
    {
        bucket.clear();
    }
}

void SceneRenderQueue::Bucketize(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects, uint32_t renderLayerMask, bool filterShadowCaster)
{
    CPU_SCOPE(ProfilerScopes::kSceneRenderQueueBucketize);

    Clear();

    for (const auto& obj : objects)
    {
        if (!obj)
        {
            continue;
        }

        if (!obj->IsVisible())
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

void SceneRenderQueue::BucketizeCull(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects, uint32_t renderLayerMask, bool filterShadowCaster, const Frustum& frustum)
{
    CPU_SCOPE(ProfilerScopes::kSceneRenderQueueBucketizeCull);

    Clear();

    const bool frustumValid = frustum.IsValid();
    for (const auto& objPtr : objects)
    {
        RenderableObjectBase* obj = objPtr.get();
        if (!obj || !obj->IsVisible())
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

        // Frustum cull in the same pass (matches Cull's predicate exactly): keep if the frustum
        // is absent, the bounds are absent, or the bounds intersect. Only survivors are stored,
        // so the culled-away majority never hits a bucket.
        const AABB& bounds = obj->GetWorldBounds();
        if (frustumValid && bounds.IsValid() && !frustum.Intersects(bounds))
        {
            continue;
        }

        const bool transparent = obj->IsTransparent();
        const bool simple = obj->IsSimpleRender();
        const BucketType type = transparent
            ? (simple ? BucketType::TransparentSimple : BucketType::TransparentComplex)
            : (simple ? BucketType::OpaqueSimple : BucketType::OpaqueComplex);

        visibleBuckets_[ToIndex(type)].push_back(obj);
        if (IsTransparentBucket(type))
        {
            transparentEntries_[TransparentIndex(type)].push_back({ obj, 0.0f });
        }
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
    CPU_SCOPE(ProfilerScopes::kSceneRenderQueueSortTransparent);
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

void SceneRenderQueue::SortOpaque()
{
    CPU_SCOPE(ProfilerScopes::kSceneRenderQueueSortOpaque);
    // Sort by the single draw-identity key (mesh + material/PSO + textures). Groups identical
    // pipeline state for the bind cache AND makes instanceable runs contiguous.
    auto cmp = [](RenderableObjectBase* a, RenderableObjectBase* b)
    {
        return a->BatchKey() < b->BatchKey();
    };
    std::sort(visibleBuckets_[ToIndex(BucketType::OpaqueSimple)].begin(),
              visibleBuckets_[ToIndex(BucketType::OpaqueSimple)].end(), cmp);
    std::sort(visibleBuckets_[ToIndex(BucketType::OpaqueComplex)].begin(),
              visibleBuckets_[ToIndex(BucketType::OpaqueComplex)].end(), cmp);
}

void SceneRenderQueue::SortOpaqueSource()
{
    CPU_SCOPE(ProfilerScopes::kSceneRenderQueueSortOpaque);
    auto cmp = [](RenderableObjectBase* a, RenderableObjectBase* b)
    {
        return a->BatchKey() < b->BatchKey();
    };
    std::sort(buckets_[ToIndex(BucketType::OpaqueSimple)].begin(),
              buckets_[ToIndex(BucketType::OpaqueSimple)].end(), cmp);
    std::sort(buckets_[ToIndex(BucketType::OpaqueComplex)].begin(),
              buckets_[ToIndex(BucketType::OpaqueComplex)].end(), cmp);
}

InstancedDrawBatch* SceneRenderQueue::AcquireBatch(size_t& cursor)
{
    if (cursor >= instancedBatches_.size())
    {
        instancedBatches_.push_back(std::make_unique<InstancedDrawBatch>());
    }
    return instancedBatches_[cursor++].get();
}

void SceneRenderQueue::SelectLods(const Camera& camera, const Frustum& cameraFrustum)
{
    CPU_SCOPE(ProfilerScopes::kSceneRenderQueueSelectLods);
    // Once per frame, ahead of every SelectLod: the ratio bounds are a screen size only at the
    // reference FOV, so the factor that carries the current one has to be current too. Set here
    // rather than in the camera because this is the single point every LOD decision passes through.
    render::UpdateLodFovScale(camera.GetHFov());
    for (auto& bucket : visibleBuckets_)
    {
        for (RenderableObjectBase* obj : bucket)
        {
            if (obj) { obj->SelectLod(camera, cameraFrustum); }
        }
    }
}

void SceneRenderQueue::BuildInstancedBatchesForBucket(BucketType type, size_t& batchCursor, bool computeLodBuckets)
{
    CPU_SCOPE(ProfilerScopes::kSceneRenderQueueBuildInstancedBatchesForBucket);

    ObjectBucket& bucket = visibleBuckets_[ToIndex(type)];
    if (bucket.size() < render::kInstancingThreshold)
    {
        return;
    }

    const bool simple = (type == BucketType::OpaqueSimple);
    ObjectBucket& rewritten = instancingRewriteBuckets_[OpaqueRewriteIndex(type)];
    rewritten.clear();
    rewritten.reserve(bucket.size());

    size_t i = 0;
    while (i < bucket.size())
    {
        RenderableObjectBase* lead = bucket[i];

        // Extend a run of identical instanceable objects. The bucket is pre-sorted by
        // BatchKey(), so objects with the same draw identity (mesh + material + textures)
        // are contiguous. BatchKey only carries slot 0, so multi-slot objects (B2b) must
        // additionally match the lead's full slot set (textures + params bind once per batch).
        const RenderBatchKey leadKey = lead ? lead->BatchKey() : RenderBatchKey{};
        const IInstanceable* leadInst = lead ? lead->AsInstanceable() : nullptr;
        size_t j = i + 1;
        if (leadInst)
        {
            while (j < bucket.size())
            {
                RenderableObjectBase* o = bucket[j];
                const IInstanceable* oInst = o ? o->AsInstanceable() : nullptr;
                if (!oInst || o->BatchKey() != leadKey || !leadInst->SameInstanceSlots(*oInst))
                {
                    break;
                }
                ++j;
            }
        }

        const size_t runLen = j - i;
        if (leadInst && runLen >= render::kInstancingThreshold)
        {
            InstancedDrawBatch* batch = AcquireBatch(batchCursor);
            batch->Configure(bucket.begin() + i,
                             bucket.begin() + j,
                             leadInst->InstancedGraphicsMaterial(),
                             leadInst->InstancedShadowMaterial(),
                             leadKey.materialData,
                             leadKey.mesh,
                             simple);
            if (computeLodBuckets) { batch->BuildLodBuckets(); } // camera view: per-instance LOD
            rewritten.push_back(batch);
        }
        else
        {
            for (size_t k = i; k < j; ++k)
            {
                rewritten.push_back(bucket[k]);
            }
        }
        i = j;
    }

    bucket.swap(rewritten);
}

void SceneRenderQueue::BuildInstancedBatches(bool computeLodBuckets)
{
    if (!render::g_instancingEnabled)
    {
        return;
    }
    size_t batchCursor = 0;
    BuildInstancedBatchesForBucket(BucketType::OpaqueSimple, batchCursor, computeLodBuckets);
    BuildInstancedBatchesForBucket(BucketType::OpaqueComplex, batchCursor, computeLodBuckets);
}

void SceneRenderQueue::Cull(const Frustum& frustum)
{
    Cull(frustum, *this);
}

void SceneRenderQueue::Cull(const Frustum& frustum, const SceneRenderQueue& source)
{
    CPU_SCOPE(ProfilerScopes::kSceneRenderQueueCull);
    const bool frustumValid = frustum.IsValid();

    for (auto& bucket : visibleBuckets_)
    {
        bucket.clear();
    }
    for (auto& entries : transparentEntries_)
    {
        entries.clear();
    }

    for (size_t bucketIndex = 0; bucketIndex < source.buckets_.size(); ++bucketIndex)
    {
        const auto& bucket = source.buckets_[bucketIndex];
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
            const bool visible = !frustumValid || !bounds.IsValid() || frustum.Intersects(bounds);

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
