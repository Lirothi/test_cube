#pragma once

#include <array>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "core/math/Frustum.h"

#include "rendering/renderables/RenderableObjectBase.h"

class InstancedDrawBatch;

// Benchmark toggle (cleared via the "cull-nofuse" command line): when true, self-culling views
// use the fused BucketizeCull; when false, the split Bucketize()+Cull() path. For a live A/B.
inline bool g_useFusedBucketizeCull = true;

class SceneRenderQueue
{
public:
    enum class BucketType { OpaqueSimple, OpaqueComplex, TransparentSimple, TransparentComplex };

    using ObjectBucket = std::vector<RenderableObjectBase*>;

    SceneRenderQueue();
    ~SceneRenderQueue();

    void Clear();
    void Bucketize(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects, uint32_t renderLayerMask, bool filterShadowCaster);
    // Fused bucketize+cull for the self-culling views (camera/shore): one pass over `objects`
    // that classifies AND frustum-tests, writing only frustum-visible objects straight into the
    // visible buckets. Skips the intermediate pre-cull buckets_ (nothing reads them for these
    // views), avoiding a whole extra pass plus the push_backs of the culled-away objects — the
    // dominant cost of the camera view's prepare. Equivalent to Bucketize(...) + Cull(frustum).
    void BucketizeCull(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects, uint32_t renderLayerMask, bool filterShadowCaster, const Frustum& frustum);
    void SortTransparent(const mat4& view);
    // Step 6: choose each visible object's camera LOD (PrepareViews, camera view only) so it's
    // ready before batching + recording. Call AFTER Cull, BEFORE BuildInstancedBatches.
    void SelectLods(const Camera& camera);
    // Step 4: collapse contiguous runs of instanceable (mesh,material) objects in the
    // visible OPAQUE buckets into one InstancedDrawBatch each. Call AFTER SortOpaque (so
    // identical objects are contiguous) and after Cull (so only visible members instance).
    // computeLodBuckets: build each batch's per-tier camera-LOD buckets (camera view only).
    void BuildInstancedBatches(bool computeLodBuckets);
    // Step 3: group the visible OPAQUE buckets into runs of identical pipeline state
    // (PSO, material, mesh) so the bind cache can elide redundant state changes. Safe
    // because opaque draws are depth-tested (order-independent); transparents are NOT
    // touched (their blend order is set by SortTransparent).
    void SortOpaque();
    // Sort the pre-cull opaque buckets. Cull(frustum, source) preserves source order, so views
    // sharing one source can pay the BatchKey sort once instead of once per shadow view.
    void SortOpaqueSource();
    void Cull(const Frustum& frustum);
    // Step 6e: cull `source`'s (already-bucketized) casters into THIS queue's visible
    // buckets — lets many views share one Bucketize. The single-arg overload culls own.
    void Cull(const Frustum& frustum, const SceneRenderQueue& source);

    const ObjectBucket& GetBucket(BucketType type) const;
    ObjectBucket& GetBucket(BucketType type);

    // S0 (occlusion plan): object counts for the visibility readout. Source = what Bucketize
    // offered (after layer/caster filters); visible = what Cull kept. Both count OBJECTS, so read
    // them BEFORE BuildInstancedBatches replaces runs with one InstancedDrawBatch entry each.
    size_t SourceObjectCount() const
    {
        size_t n = 0;
        for (const ObjectBucket& b : buckets_) { n += b.size(); }
        return n;
    }
    size_t VisibleObjectCount() const
    {
        size_t n = 0;
        for (const ObjectBucket& b : visibleBuckets_) { n += b.size(); }
        return n;
    }
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
    static size_t OpaqueRewriteIndex(BucketType type) { return type == BucketType::OpaqueSimple ? 0u : 1u; }

    float ComputeDepth(const mat4& view, const TransparentEntry& entry) const;

    // Step 4: rewrite one visible opaque bucket, replacing instanceable runs with batches.
    // computeLodBuckets: build each batch's per-instance camera-LOD buckets (camera view only).
    void BuildInstancedBatchesForBucket(BucketType type, size_t& batchCursor, bool computeLodBuckets);
    InstancedDrawBatch* AcquireBatch(size_t& cursor);

    std::array<ObjectBucket, 4> buckets_{};
    std::array<ObjectBucket, 4> visibleBuckets_{};
    std::array<std::vector<TransparentEntry>, 2> transparentEntries_{};
    std::array<ObjectBucket, 2> instancingRewriteBuckets_{};

    // Pooled per-frame instanced batches (reused across frames; one queue per view).
    std::vector<std::unique_ptr<InstancedDrawBatch>> instancedBatches_;
};
