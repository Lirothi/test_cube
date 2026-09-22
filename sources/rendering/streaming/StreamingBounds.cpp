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

namespace streaming {

void BuildBounds(const SceneFrameData& frame, const Frustum& frustum, float now,
                 std::unordered_map<const void*, float>& lastSeen,
                 const std::vector<int>& textureSlot,
                 std::vector<BoundsEntry>& out)
{
    out.clear();
    if (!frame.objects) { return; }
    out.reserve(frame.objects->size());
    for (const auto& objPtr : *frame.objects)
    {
        const RenderableObjectBase* base = objPtr.get();
        if (!base) { continue; }
        const GBufferRenderable* gb = base->AsGBufferRenderable();
        const RenderableObject* ro = base->AsRenderableObject();
        if (!gb || !ro) { continue; }
        const AABB& box = base->GetWorldBounds();
        if (!box.IsValid()) { continue; }

        BoundsEntry e;
        e.center = box.GetCenter();
        e.halfExtents = box.GetHalfExtents();
        e.terrain = (base->GetRenderLayerMask() & RenderLayerMask(RenderLayer::Terrain)) != 0u;
        // Visibility: a CPU frustum test stands in for UE's per-primitive LastRenderTime.
        float& seen = lastSeen[base];
        if (frustum.Intersects(box)) { seen = now; }
        else if (seen == 0.0f) { seen = -1.0e6f; } // never seen
        e.lastSeenAge = now - seen;

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
                if (owner < 0 || static_cast<size_t>(owner) >= textureSlot.size()) { continue; }
                const int idx = textureSlot[static_cast<size_t>(owner)];
                if (idx < 0) { continue; }
                e.refs.push_back(BoundsEntry::Ref{ static_cast<std::uint32_t>(idx), texelFactor });
            }
        }
        if (!e.refs.empty()) { out.push_back(std::move(e)); }
    }
}

} // namespace streaming
