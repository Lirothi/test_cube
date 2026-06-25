#pragma once

#include <vector>
#include <d3d12.h>

#include "core/math/AABB.h"
#include "core/math/Math.h"
#include "rendering/RenderLayers.h"
#include "rendering/core/RenderGraph.h"

class Renderer;

// One ray-traced instance's geometry + material, gathered for the TLAS/bindless
// table (S9/S10). albedoTex is null when the renderable has no albedo texture
// (then baseColor is the flat color); albedoSrv is a CPU SRV handle to copy into
// the bindless heap.
struct RtInstanceDesc
{
    Mesh* mesh = nullptr;
    Math::mat4 world;
    ID3D12Resource* albedoTex = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE albedoSrv{};
    ID3D12Resource* mrTex = nullptr;       // metal/rough texture (null = use flat metalRough)
    D3D12_CPU_DESCRIPTOR_HANDLE mrSrv{};
    Math::float4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    Math::float2 metalRough{ 0.0f, 1.0f }; // x=metallic, y=roughness (flat fallback when no MR texture)
};
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
    // lod: per-pass LOD floor (Step 6 — e.g. the shadow cascade index); clamped to available LODs.
    virtual void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, D3D12_GPU_VIRTUAL_ADDRESS viewCB, UINT lod = 0) = 0;
    virtual bool IsTransparent() const = 0;
    virtual bool IsSimpleRender() const = 0;
    virtual bool CastsShadow() const = 0;
    virtual void OnMaterialHotReload(Renderer* renderer) {}

    // Step 6: choose this object's camera/gbuffer LOD for the frame. Called once per visible
    // object in Scene::PrepareViews (NOT during recording) so the per-object/per-instance
    // state (incl. hysteresis) is updated outside the parallel record. Render() just reads it.
    // Shadow LOD is the cascade index, chosen per-pass by the renderer — not here.
    virtual void SelectLod(const Camera& /*camera*/) {}
    virtual unsigned int GetCameraLod() const { return 0u; }

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

    // RT (S5/S10): if this renderable is a single mesh placed by a CPU world
    // matrix, fill `out` (mesh, world, and material albedo/base color) and return
    // true. Instanced/GPU-driven, transformless or transparent renderables return
    // false (excluded from the ray-tracing TLAS for now — S13 defines their handling).
    virtual bool GetRtInstance(RtInstanceDesc& out) const
    {
        (void)out;
        return false;
    }

    // RT: append every ray-traced instance of this renderable to `out`, returning
    // the count added. Default: the single instance from GetRtInstance (covers all
    // single-mesh objects). GPU-instanced renderables (S14) override this to emit one
    // entry per instance (same mesh/material, per-instance world matrix).
    virtual size_t GetRtInstances(std::vector<RtInstanceDesc>& out) const
    {
        RtInstanceDesc d{};
        if (GetRtInstance(d)) { out.push_back(d); return 1; }
        return 0;
    }

    uint32_t GetRenderLayerMask() const { return renderLayerMask_; }
    void SetRenderLayerMask(uint32_t mask) { renderLayerMask_ = mask; }
    void SetRenderLayer(RenderLayer layer) { renderLayerMask_ = RenderLayerMask(layer); }
    void AddRenderLayer(RenderLayer layer) { EnableLayer(renderLayerMask_, layer); }
    void RemoveRenderLayer(RenderLayer layer) { DisableLayer(renderLayerMask_, layer); }

protected:
    uint32_t renderLayerMask_ = RenderLayerMask(RenderLayer::Default);
};
