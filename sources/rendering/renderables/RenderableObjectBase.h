#pragma once

#include <vector>

#include "core/math/AABB.h"
#include "rendering/RenderLayers.h"
#include "rendering/core/RenderGraph.h"

class Renderer;
class Camera;
class Material;
class MaterialData;
class Mesh;
class IInstanceable;

// A renderable's "draw identity": two OPAQUE draws with the same key are interchangeable —
// same geometry (mesh), pipeline state (material = PSO + root sig, 1:1 with the PSO here),
// and textures (materialData). The queue sorts by it (group identical pipeline state for the
// bind cache) and collapses equal-key runs into one instanced draw. Expressing it as one
// value makes "these draws are identical" correct-by-construction — the Step 4 wrong-texture
// bug was a hand-assembled key that omitted materialData.
struct RenderBatchKey
{
    Mesh*         mesh = nullptr;
    Material*     material = nullptr;     // PSO + root signature
    MaterialData* materialData = nullptr; // textures

    bool operator==(const RenderBatchKey& o) const
    {
        return mesh == o.mesh && material == o.material && materialData == o.materialData;
    }
    bool operator!=(const RenderBatchKey& o) const { return !(*this == o); }
    bool operator<(const RenderBatchKey& o) const
    {
        if (material != o.material) { return material < o.material; }
        if (mesh != o.mesh) { return mesh < o.mesh; }
        return materialData < o.materialData;
    }
};

class RenderableObjectBase
{
public:
    RenderableObjectBase() = default;
    virtual ~RenderableObjectBase() noexcept = default;
    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) = 0;
    virtual void Tick(float /*dt*/) = 0;
    virtual void PostTick(float /*dt*/) {}
    // viewCB: GPU VA of the per-pass shared view/frame constant buffer bound at b1
    // (camera matrices for the gbuffer/transparent pass, light viewProj for shadows).
    virtual void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB) = 0;
    virtual void ExecuteCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) { (void)renderer; (void)cl; }
    virtual void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, D3D12_GPU_VIRTUAL_ADDRESS viewCB) = 0;
    virtual bool IsTransparent() const = 0;
    virtual bool IsSimpleRender() const = 0;
    virtual bool CastsShadow() const = 0;
    virtual void OnMaterialHotReload(Renderer* renderer) {}

    // Draw identity for opaque sorting + instanced-run grouping (Step 3/4). Default empty key
    // (mesh-less / transient renderables sort together). See RenderBatchKey above.
    virtual RenderBatchKey BatchKey() const { return {}; }

    // Step 4/5c: auto-instancing capability. Non-null only for renderables that have a
    // CPU-instanced shader variant. A run of objects sharing BatchKey() that all return
    // non-null collapses into one DrawInstanced per pass. Returned pointer is owned by the
    // renderable; valid for the object's lifetime. See IInstanceable.
    virtual const IInstanceable* AsInstanceable() const { return nullptr; }

    virtual const AABB& GetWorldBounds() const
    {
        static const AABB kInvalidBounds = AABB::Empty();
        return kInvalidBounds;
    }

    uint32_t GetRenderLayerMask() const { return renderLayerMask_; }
    void SetRenderLayerMask(uint32_t mask) { renderLayerMask_ = mask; }
    void SetRenderLayer(RenderLayer layer) { renderLayerMask_ = RenderLayerMask(layer); }
    void AddRenderLayer(RenderLayer layer) { EnableLayer(renderLayerMask_, layer); }
    void RemoveRenderLayer(RenderLayer layer) { DisableLayer(renderLayerMask_, layer); }

protected:
    uint32_t renderLayerMask_ = RenderLayerMask(RenderLayer::Default);
};
