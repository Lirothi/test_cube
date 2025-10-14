#pragma once

#include <array>
#include <memory>
#include <vector>

#include "core/math/Math.h"

#include "rendering/renderables/RenderableObjectBase.h"

class SceneRenderQueue
{
public:
    enum class BucketType { OpaqueSimple, OpaqueComplex, TransparentSimple, TransparentComplex };

    using ObjectBucket = std::vector<RenderableObjectBase*>;

    struct TransparentEntry
    {
        RenderableObjectBase* base = nullptr;
        float depth = 0.0f;
    };

    SceneRenderQueue();

    void Clear();
    void Bucketize(const std::vector<std::unique_ptr<RenderableObjectBase>>& objects);
    void SortTransparent(const mat4& view);

    const ObjectBucket& GetBucket(BucketType type) const;
    ObjectBucket& GetBucket(BucketType type);
    const std::array<ObjectBucket, 4>& Buckets() const { return buckets_; }

private:
    static size_t ToIndex(BucketType type) { return static_cast<size_t>(type); }

    float ComputeDepth(const mat4& view, const TransparentEntry& entry) const;

    std::array<ObjectBucket, 4> buckets_{};
    std::array<std::vector<TransparentEntry>, 2> transparentEntries_{};
};
