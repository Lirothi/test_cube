#pragma once
#include "rendering/renderables/InstanceTypes.h"

class Material;

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

    // Write this object's per-instance payload (world/prevWorld + material params).
    virtual void FillInstanceData(render::InstancePerObject& out) const = 0;
};
