#pragma once
#include "rendering/renderables/RenderableObjectBase.h"
#include <array>
#include <vector>

class IInstanceable;
class Material;
class MaterialData;
class Mesh;

// Step 4 auto-instancing. A transient, per-frame, per-view draw that collapses a run of
// identical (mesh, material) opaque objects into one DrawInstanced for the GBuffer pass and
// one per shadow cascade/spot. It is INJECTED into the visible bucket in place of the run
// (after culling + sort), so the normal passes record it via Render()/RenderShadow() with
// no change to the dispatch code. Members are non-owning pointers to the (already visible)
// scene objects; per-instance data is gathered into a root-CBV array (b0) each record.
class InstancedDrawBatch final : public RenderableObjectBase
{
public:
    // gfx/shadow are the instanced (cbuffer-array) materials; matData supplies the shared
    // GBuffer textures; mesh is the shared geometry. `simple` mirrors the run's bucket.
    void Configure(std::vector<RenderableObjectBase*>::const_iterator first,
                   std::vector<RenderableObjectBase*>::const_iterator last,
                   Material* gfx, Material* shadow, MaterialData* matData, Mesh* mesh,
                   bool simple);

    // Step 6: bucket members by their (PrepareViews-computed) camera LOD, for the camera pass.
    // Called once when the batch is built (PrepareViews), so Render stays side-effect-free.
    void BuildLodBuckets();

    void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB) override;
    void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, D3D12_GPU_VIRTUAL_ADDRESS viewCB, UINT lod) override;

    void Init(Renderer*, ID3D12GraphicsCommandList*, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>*) override {}
    void Tick(float) override {}
    bool IsTransparent() const override { return false; }
    bool IsSimpleRender() const override { return simple_; }
    bool CastsShadow() const override { return shadowMat_ != nullptr; }
    // Created post-cull; the empty sentinel keeps any accidental later cull conservative.
    const AABB& GetWorldBounds() const override { return bounds_; }

    size_t InstanceCount() const { return members_.size(); }

private:
    // Records instanced draws for a SUBSET of the run at one LOD (chunked to the shader's
    // instance-array capacity). Step 6d: the camera pass buckets members per-instance and
    // calls this once per LOD tier; the shadow pass calls it once with all members.
    void RecordInstanced(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                         Material* material, D3D12_GPU_VIRTUAL_ADDRESS viewCB, bool gbuffer, UINT lod,
                         const std::vector<RenderableObjectBase*>& members);

    static constexpr unsigned int kMaxLodTiers = 4; // base + 3 (matches SelectLodTier range)

    std::vector<RenderableObjectBase*> members_;
    Material* gfxMat_ = nullptr;
    Material* shadowMat_ = nullptr;
    MaterialData* matData_ = nullptr;
    Mesh* mesh_ = nullptr;
    bool simple_ = true;
    AABB bounds_ = AABB::Empty();
    // B2b: non-null when the run is multi-slot (multi-submesh mesh + >1 material slot) — the
    // gbuffer path then loops submeshes, binding each slot's textures + a b2 params slice from
    // this lead (run members are slot-identical by SameInstanceSlots). Shadows stay whole-mesh.
    const IInstanceable* leadInst_ = nullptr;
    // Per-instance LOD buckets, reused each Render (pooled batch -> retains capacity).
    std::array<std::vector<RenderableObjectBase*>, kMaxLodTiers> lodBuckets_;
    // B2b scratch: per-slot param CB GPU addresses for the current RecordInstanced call.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> slotCbScratch_;
};
