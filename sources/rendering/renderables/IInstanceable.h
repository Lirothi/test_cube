#pragma once
#include "rendering/renderables/InstanceTypes.h"

class Material;
class MaterialData;
struct MaterialParams;

// Optional capability (Step 5c): a renderable that can be drawn instanced exposes this via
// RenderableObjectBase::AsInstanceable(). Keeps the instancing surface off the base
// interface — only renderables with a CPU-instanced shader variant implement it. A run of
// objects with the same RenderBatchKey() that all return non-null AsInstanceable() collapses
// into one DrawInstanced per pass.
class IInstanceable
{
public:
    virtual ~IInstanceable() = default;

    // Instanced (cbuffer-array) material variants of this object's gbuffer + shadow materials.
    virtual Material* InstancedGraphicsMaterial() const = 0;
    virtual Material* InstancedShadowMaterial() const = 0;

    // C1b: per-slot instanced gbuffer PSO (slot defines: sampling swizzles, ALPHA_TEST, cull).
    // Default = the single instanced material (single-slot objects and non-slot renderables).
    virtual Material* InstancedGraphicsMaterialForSlot(size_t /*slot*/) const
    {
        return InstancedGraphicsMaterial();
    }

    // Write this object's per-instance payload (world/prevWorld + material params).
    virtual void FillInstanceData(render::InstancePerObject& out) const = 0;

    // B2b: material-slot identity for multi-submesh batching. Single-slot objects keep the
    // defaults. RenderBatchKey only carries slot 0, and a multi-slot batch binds slot 1+
    // textures AND all slots' params from the run's LEAD (per-slot CB uploads once per batch,
    // not per instance) — so members whose slot sets or slot params differ must not merge.
    virtual size_t InstanceSlotCount() const { return 1; }
    virtual MaterialData* InstanceSlotData(size_t /*slot*/) const { return nullptr; }
    virtual const MaterialParams* InstanceSlotParams(size_t /*slot*/) const { return nullptr; }

    // True when `other` can join a run led by this object (slot sets + params identical).
    // Single-slot pairs always pass: their params ride the per-instance array instead.
    // Defined in GBufferRenderable.cpp (needs MaterialParams' definition).
    bool SameInstanceSlots(const IInstanceable& other) const;
};
