#include "rendering/streaming/StreamingBounds.h"

#include <algorithm>
#include <cmath>

#include "app/scene/SceneFrameData.h"
#include "core/math/AABB.h"
#include "core/math/Frustum.h"
#include "materials/MaterialData.h"
#include "materials/Texture2D.h"
#include "rendering/RenderLayers.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/streaming/TextureStreaming.h"

namespace streaming {

std::size_t BoundsBuilder::ObjectCount(const SceneFrameData& frame)
{
    return frame.objects ? frame.objects->size() : 0u;
}

bool BoundsBuilder::Step(const SceneFrameData& frame, const Frustum& frustum, float now, const TextureStreaming& ts, std::size_t maxObjects)
{
    const std::size_t count = ObjectCount(frame);
    const void* list = frame.objects ? static_cast<const void*>(frame.objects->data()) : nullptr;
    if (list != list_ || count != listSize_)
    {
        set_.Clear();
        cursor_ = 0;
        list_ = list;
        listSize_ = count;
    }
    const std::size_t end = maxObjects == 0 ? count : std::min(count, cursor_ + maxObjects);
    for (; cursor_ < end; ++cursor_)
    {
        const RenderableObjectBase* base = (*frame.objects)[cursor_].get();
        if (!base) { continue; }
        const GBufferRenderable* gb = base->AsGBufferRenderable();
        const RenderableObject* ro = base->AsRenderableObject();
        if (!gb || !ro) { continue; }
        const AABB& box = base->GetWorldBounds();
        if (!box.IsValid()) { continue; }

        BoundsEntry e;
        e.terrain = (base->GetRenderLayerMask() & RenderLayerMask(RenderLayer::Terrain)) != 0u;
        e.firstRef = static_cast<std::uint32_t>(set_.refs.size());

        const Math::float3 scale = ro->GetScale();
        const float objScale = std::max({ std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z), 1.0e-4f });
        const Math::float3 ext = box.GetExtents();
        const float boxSize = std::max({ ext.x, ext.y, ext.z, 1.0e-3f });

        const size_t slots = std::max<size_t>(gb->InstanceSlotCount(), 1u);
        for (size_t s = 0; s < slots; ++s)
        {
            const MaterialData* md = gb->InstanceSlotData(s);
            if (!md) { continue; }
            const MaterialParams* mp = gb->InstanceSlotParams(s);
            const float tiling = mp ? std::max({ std::fabs(mp->texOffsScale.z), std::fabs(mp->texOffsScale.w), 1.0e-3f }) : 1.0f;
            const float density = ro->UvDensityForSlot(s);
            // No measured density (manifest predates A1): the box itself is the unit-UV square.
            const float texelFactor = (density > 0.0f ? density * objScale : boxSize) / tiling;
            const Texture2D* texs[3] = { md->hasAlbedo ? &md->albedo : nullptr,
                                         md->hasMR ? &md->mr : nullptr,
                                         md->hasNormal ? &md->normal : nullptr };
            for (const Texture2D* t : texs)
            {
                if (!t) { continue; }
                const int owner = t->StreamingOwnerIndex();
                if (owner < 0) { continue; }
                const std::uint32_t entry = static_cast<std::uint32_t>(owner);
                set_.refs.push_back(BoundsRef{ entry, ts.EntryGen(entry), texelFactor });
            }
        }
        e.refCount = static_cast<std::uint32_t>(set_.refs.size()) - e.firstRef;
        if (e.refCount == 0) { continue; }

        // Visibility: a CPU frustum test stands in for UE's per-primitive LastRenderTime.
        // The age is taken at the slice's own time, so a hitch before the snapshot cannot age a box
        // that was in view when its slice ran.
        const auto seenAge = [this, now, &frustum](const void* key, const AABB& b)
        {
            float& seen = lastSeen_[key];
            if (frustum.Intersects(b)) { seen = now; }
            else if (seen == 0.0f) { seen = -1.0e6f; } // never seen
            return now - seen;
        };
        // A chunked mesh (terrain) is one box per chunk, the way a UE landscape is one primitive
        // per component: the island's whole-box distance was ~0 from anywhere above it.
        const Mesh* mesh = ro->GetMesh();
        if (mesh && mesh->IsChunkedSubmeshes() && !mesh->GetSubmeshBounds().empty())
        {
            const Math::mat4& model = ro->GetModelMatrix();
            const std::vector<AABB>& chunks = mesh->GetSubmeshBounds();
            for (size_t c = 0; c < chunks.size(); ++c)
            {
                if (!chunks[c].IsValid()) { continue; }
                const AABB w = chunks[c].Transform(model);
                BoundsEntry ce = e;
                ce.center = w.GetCenter();
                ce.halfExtents = w.GetHalfExtents();
                const void* key = reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(base) + c + 1u);
                ce.lastSeenAge = seenAge(key, w);
                set_.entries.push_back(ce);
            }
            continue;
        }
        e.center = box.GetCenter();
        e.halfExtents = box.GetHalfExtents();
        e.lastSeenAge = seenAge(base, box);
        set_.entries.push_back(e);
    }
    return cursor_ >= count;
}

void BoundsBuilder::TakeResolved(BoundsSet& out, const std::vector<int>& textureSlot, const TextureStreaming& ts)
{
    for (BoundsRef& r : set_.refs)
    {
        const int idx = r.texture < textureSlot.size() ? textureSlot[r.texture] : -1;
        r.texture = (idx >= 0 && ts.EntryGen(r.texture) == r.gen) ? static_cast<std::uint32_t>(idx) : BoundsRef::kNone;
    }
    std::swap(out, set_);
    set_.Clear();
    cursor_ = 0;
    list_ = nullptr;
    listSize_ = 0;
}

void BoundsBuilder::PruneLastSeen(float now)
{
    if (lastSeen_.size() <= 4096) { return; }
    for (auto it = lastSeen_.begin(); it != lastSeen_.end();)
    {
        if (now - it->second > 120.0f) { it = lastSeen_.erase(it); } else { ++it; }
    }
}

} // namespace streaming
