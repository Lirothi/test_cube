#pragma once

#include <array>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "core/math/Frustum.h"

#include "rendering/renderables/RenderableObjectBase.h"

class InstancedDrawBatch;

class SceneRenderQueue
{
public:
    enum class BucketType { OpaqueSimple, OpaqueComplex, TransparentSimple, TransparentComplex };

    using ObjectBucket = std::vector<RenderableObjectBase*>;

    SceneRenderQueue();
    ~SceneRenderQueue();

    void Clear();
    void Bucketize(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects, uint32_t renderLayerMask, bool filterShadowCaster);
    void SortTransparent(const mat4& view);
    // Step 4: collapse contiguous runs of instanceable (mesh,material) objects in the
    // visible OPAQUE buckets into one InstancedDrawBatch each. Call AFTER SortOpaque (so
    // identical objects are contiguous) and after Cull (so only visible members instance).
    void BuildInstancedBatches();
    // Step 3: group the visible OPAQUE buckets into runs of identical pipeline state
    // (PSO, material, mesh) so the bind cache can elide redundant state changes. Safe
    // because opaque draws are depth-tested (order-independent); transparents are NOT
    // touched (their blend order is set by SortTransparent).
    void SortOpaque();
    void Cull(const Frustum& frustum);
    // Step 6e: cull `source`'s (already-bucketized) casters into THIS queue's visible
    // buckets — lets many views share one Bucketize. The single-arg overload culls own.
    void Cull(const Frustum& frustum, const SceneRenderQueue& source);

    const ObjectBucket& GetBucket(BucketType type) const;
    ObjectBucket& GetBucket(BucketType type);
    ObjectBucket& GetVisibleBucket(BucketType type);
    const std::array<ObjectBucket, 4>& Buckets() const { return buckets_; }
    const std::array<ObjectBucket, 4>& VisibleBuckets() const { return visibleBuckets_; }

private:
    struct TransparentEntry
    {
        RenderableObjectBase* base = nullptr;
        float depth = 0.0f;
    };

    static size_t ToIndex(BucketType type) { return static_cast<size_t>(type); }

    float ComputeDepth(const mat4& view, const TransparentEntry& entry) const;

    // Step 4: rewrite one visible opaque bucket, replacing instanceable runs with batches.
    void BuildInstancedBatchesForBucket(BucketType type, size_t& batchCursor);
    InstancedDrawBatch* AcquireBatch(size_t& cursor);

    std::array<ObjectBucket, 4> buckets_{};
    std::array<ObjectBucket, 4> visibleBuckets_{};
    std::array<std::vector<TransparentEntry>, 2> transparentEntries_{};

    // Pooled per-frame instanced batches (reused across frames; one queue per view).
    std::vector<std::unique_ptr<InstancedDrawBatch>> instancedBatches_;
};
