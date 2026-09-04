#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include <memory>
#include <cstdint>
#include <vector>

#include "materials/Material.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/core/RenderContext.h"
#include "core/math/Math.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "core/math/AABB.h"

class Renderer;
class Camera;

class RenderableObject: public RenderableObjectBase {
public:
    class UniformBinder
    {
    public:
        virtual ~UniformBinder() = default;

        virtual void RebuildHandles(RenderableObject& /*owner*/) {}
        virtual void UpdateMainCB(RenderableObject& /*owner*/, Renderer* /*renderer*/, const Camera& /*camera*/, uint8_t* /*cbData*/) {}
        virtual void UpdateShadowCB(RenderableObject& /*owner*/, Renderer* /*renderer*/, const mat4& /*lightView*/, const mat4& /*lightProj*/, uint8_t* /*cbData*/) {}

    protected:
        template<typename T>
        bool UpdateUniform(RenderableObject& owner, const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData) const
        {
            return owner.UpdateUniform(handle, material, value, cbData);
        }

        template<typename T>
        bool UpdateUniform(RenderableObject& owner, const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData, uint32_t arrayIndex) const
        {
            return owner.UpdateUniform(handle, material, value, cbData, arrayIndex);
        }
    };

    RenderableObject(
        const std::string& inputLayout,
        const std::wstring& graphicsShader);
    virtual ~RenderableObject();

    RenderableObject* AsRenderableObject() override { return this; }
    const RenderableObject* AsRenderableObject() const override { return this; }

    // Lifecycle
    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    virtual void Tick(float /*dt*/) {}
    virtual void PostTick(float /*dt*/) override;
    void SyncSceneState(SceneObjectSyncReason reason) override;

    // Base renderer: Compute -> Graphics
    virtual void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB);
#if WITH_EDITOR
    void RenderSelectionStencil(Renderer* renderer, ID3D12GraphicsCommandList* cl, Material* material, const Camera& camera) override;
#endif
    void ExecuteCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    virtual void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, D3D12_GPU_VIRTUAL_ADDRESS viewCB, UINT lod, bool chunkCameraLods, const Frustum* chunkCullFrustum) override;
    virtual void OnMaterialHotReload(Renderer* renderer);

    // Transform
    const Math::mat4& GetModelMatrix() const { return modelMatrix_; }
    const Math::mat4& GetPreviousModelMatrix() const { return prevModelMatrix_; }

    // Rung 0 (Step 7 / Rung 1 Step 11): did this object's model matrix change during this
    // frame's SyncSceneState? Snapshotted before modelMatrixChangedThisTick_ is cleared, so it
    // survives to render time. Lets the GPU shadow path re-upload only movers (skip static
    // casters entirely) instead of re-checking every caster each frame.
    bool MovedThisFrame() const { return movedThisFrame_; }
    void ResetMotionHistory()
    {
        prevModelMatrix_ = modelMatrix_;
        prevModelMatrixValid_ = true;
        modelMatrixChangedThisTick_ = false;
    }
    void SetModelMatrix(const Math::mat4& m)
    {
        if (prevModelMatrixValid_)
        {
            prevModelMatrix_ = modelMatrix_;
        }
        else
        {
            prevModelMatrix_ = m;
            prevModelMatrixValid_ = true;
        }
        modelMatrix_ = m;
        transformDirty_ = false;
        modelMatrixChangedThisTick_ = true;
        MarkWorldBoundsDirty();
    }

    void SetPosition(const Math::float3& p);
    void SetScale(const Math::float3& s);
    void SetRotationEulerRad(const Math::float3& eulerXYZ);
    void SetRotationEulerDeg(const Math::float3& eulerDegXYZ);

    Math::float3 GetPosition() const { return pos_; }
    Math::float3 GetScale() const { return scale_; }
    Math::float3 GetRotationEulerRad() const { return rotEuler_; }
    Math::mat4 GetOrientationMatrix() const { return Math::mat4::RotationFromEulerXYZRad(rotEuler_); }

    // Mesh/material
    Mesh* GetMesh() { return mesh_.get(); }
    const Mesh* GetMesh() const { return mesh_.get(); }

    AABB GetLocalBounds() const;
    const AABB& GetWorldBounds() const override;

    // Step 6: radius of ONE drawn instance for LOD selection. Default = the object's world
    // radius (standalone). Cloud/instanced objects override to the single-mesh radius so they
    // don't stay at LOD 0 forever (their aggregate world bounds span the whole cloud).
    // The mesh's VERTEX-enclosing sphere, scaled to world. Unreal selects on Bounds.SphereRadius
    // for the same reason: AABB::GetRadius() is the radius of the box CORNER, so it charges for the
    // empty volume the box has and the geometry does not, and how much empty volume that is depends
    // on the SHAPE of the box rather than the size of the object. Measured over this project's own
    // assets, corner/sphere runs 1.000x on box.mesh, 1.16-1.26x on the palms, and 1.732x on
    // sphere.mesh -- a 73% spread in the selection metric produced by nothing but bounding-box
    // shape, which is exactly what makes one global curve impossible to tune for two shapes at once.
    // Falls back to the box radius only when there is no mesh to ask.
    virtual float GetLodRadius() const;

    // mesh.json "lodDistanceScale": multiplies the distance at which EVERY LOD switch of this asset
    // happens. 2 = each level starts twice as far away (keeps detail longer), 0.5 = half.
    //
    // This is the knob Unreal has and this engine did not. UE authors a ScreenSize PER LOD PER MESH
    // (FStaticMeshRenderData::ScreenSize, consumed by ComputeStaticMeshLOD) and multiplies it by a
    // per-component FactorScale, so a palm and a sphere never share one switch curve. Here the curve
    // is global (g_lodBound0/1/2, "distance / instance radius"), so tuning it for foliage mistunes
    // everything else -- which is exactly the "palms are right now but spheres switch too early"
    // case. A per-asset multiplier is the smallest thing that buys UE's separation without changing
    // the shape of the curve everyone is already tuned against.
    void SetLodDistanceScale(float s) { lodDistanceScale_ = s > 0.01f ? s : 0.01f; }
    float GetLodDistanceScale() const { return lodDistanceScale_; }

    // The mesh's own DERIVED scale (Mesh::GetLodAutoDistanceScale), or 1 when the derivation is
    // switched off. Multiplied with the authored lodDistanceScale above, so the manifest tunes ON
    // TOP of the automatic answer instead of replacing it.
    float LodAutoScale() const;

    // The exact radius LOD selection divides distance by — every per-asset factor folded in. The
    // ONE place this expression lives: SelectLod and the shadow path's receiver-LOD recompute
    // (ShadowGpuData::RefreshCasterLods) must agree to the bit, or caster and receiver drift.
    float LodSelectionRadius() const { return GetLodRadius() * lodDistanceScale_ * LodAutoScale(); }

    // The LOD tier the camera pass would select for this object RIGHT NOW, stateless apart from
    // the hysteresis seed. For an object SelectLod visited this frame it returns cameraLod_
    // exactly (the same function on the same inputs); for an OFF-SCREEN caster it returns current
    // truth instead of the stale stored value — which is what the shadow path needs, because a
    // caster keeps casting long after its receiver leaves the frustum.
    unsigned int ComputeReceiverLodTier(const Math::float3& cameraPos) const;

    // Step 6: camera LOD chosen in PrepareViews (with hysteresis), read at draw time.
    void SelectLod(const Camera& camera, const Frustum& cameraFrustum) override;
    unsigned int GetCameraLod() const override { return cameraLod_; }
    float GetCameraLodFade() const override { return cameraLodFade_; }
    // The fade value of the draw being RECORDED right now (0 outside transitions): -fade for
    // the outgoing-LOD draw, +fade for the incoming one. Read by the uniform binder into the
    // PerObject cbuffer's lodFade field; set/cleared by the Render implementations.
    float GetDrawLodFade() const { return drawLodFade_; }

    // Chunked-terrain LOD: one camera tier per chunk (submesh) of a chunked mesh, chosen in
    // SelectLod from the chunk's own world AABB (render::SelectChunkLodTier, hysteresis per
    // chunk). Empty for non-chunked meshes. CONSUMED BY THREE PATHS THAT MUST AGREE: the gbuffer
    // draw, the Legacy per-view shadow loop, and (via ShadowGpuData's per-group override) the VSM
    // page render — one array is what makes caster == receiver a construction, not a hope.
    const std::vector<std::uint8_t>& ChunkCameraLods() const { return chunkLods_; }
    // Occlusion plan S1: 1 = the chunk's world box meets the CAMERA frustum this frame (written by
    // SelectLod next to the tier, read by the gbuffer draw and the camera's visibility counters).
    // A MASK beside chunkLods_, not a filter of it: the tier of a camera-invisible chunk stays
    // selected because the shadow paths above still cast that chunk at that tier. Shadow views do
    // not read this -- they test their own frustum on the spot (RenderShadow / ChunkInFrustum).
    const std::vector<std::uint8_t>& ChunkCameraVisible() const { return chunkVisCamera_; }
    // S3a: the camera prepare ANDs the occlusion verdict of each chunk into the mask, on the
    // same task that wrote it. Nobody else writes it.
    std::vector<std::uint8_t>& ChunkCameraVisibleRef() { return chunkVisCamera_; }
    // THE chunk-vs-view predicate, shared by the camera mask, the cascade loop and the counters so
    // a number in the readout is the draw's decision and not a look-alike. Conservative like the
    // object cull (AABB positive vertex); honours the `vis.chunkMask` rollback (off = always true).
    static bool ChunkInFrustum(const AABB& worldBox, const Frustum& frustum);

    Material* GetGraphicsMaterial() const { return graphicsMaterial_.get(); }
    void SetGraphicsMaterial(Material* m);
    Material* GetShadowMaterial() const { return shadowMaterial_.get(); }

    virtual bool IsTransparent() const;

    bool CastsShadow() const override { return castsShadow_; }
    // Take an object OUT of the shadow-caster set without touching its visibility. One moving caster
    // is enough to set VSM's `forceAll` (ShadowGpuData::MoverCount() > 0), which marks EVERY resident
    // page dirty and re-renders the whole pool -- measured at ~195 ms per frame in Debug against a
    // 0.38 ms baseline while a mesh was being dragged. A transient ghost like the editor's spawn
    // preview has no business paying that, or making the rest of the scene pay it.
    void SetCastsShadow(bool v) { castsShadow_ = v; }

    // Draw identity (no MaterialData at this tier; GBufferRenderable adds textures).
    RenderBatchKey BatchKey() const override { return RenderBatchKey{ mesh_.get(), graphicsMaterial_.get(), nullptr }; }

    // RT (S5): a standalone opaque mesh contributes one TLAS instance at its CPU
    // world matrix. No material textures at this tier (GBufferRenderable adds the
    // albedo); GpuInstancedModels overrides back to false (GPU-driven transforms).
    bool GetRtInstance(RtInstanceDesc& out) const override
    {
        if (!mesh_ || mesh_->GetIndexCount() == 0 || IsTransparent())
        {
            return false;
        }
        out.mesh = mesh_.get();
        out.world = modelMatrix_;
        return true;
    }

protected:
    virtual void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) {}
    // Returns Material::Bind's verdict: false = the root signature declares a table this draw has
    // no handle for, so the CALLER must skip the draw (GBufferBindingGuard.h explains why).
    virtual bool RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData);

    // C1b: the material actually BOUND for the draw being recorded. Defaults to the object's
    // graphics material; GBufferRenderable overrides it to return the current slot's per-slot
    // PSO inside the multi-slot submesh loop. CB field handles still come from
    // GetGraphicsMaterial() (slot 0) — all slot permutations share the PerObject layout.
    virtual Material* CurrentGraphicsMaterial() const { return graphicsMaterial_.get(); }
    virtual void RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, RenderContext& ctx);
    // Forwards Material::Bind's verdict; see RecordGraphics.
    bool UpdateAndBindGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData);
    virtual void DrawGeometry(ID3D12GraphicsCommandList* cl, UINT lod = 0);

    void MarkTransformDirty();

    bool castsShadow_ = true; // see SetCastsShadow

    template<typename T>
    bool UpdateUniform(const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData)
    {
        if (!cbData) { return false; }
        if (!material) { return false; }
        if (!handle.field) { return false; }
        return material->UpdateCBField(handle, value, cbData);
    }

    template<typename T>
    bool UpdateUniform(const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData, uint32_t arrayIndex)
    {
        if (!cbData) { return false; }
        if (!material) { return false; }
        if (!handle.field) { return false; }
        return material->UpdateCBField(handle, value, cbData, arrayIndex);
    }

    const std::wstring& GetGraphicsShaderPath() const { return graphicsShader_; }

protected:
    Material::GraphicsDesc BuildGraphicsDesc(Renderer* renderer) const;
    Material::GraphicsDesc BuildShadowDesc(Renderer* renderer, const Material::GraphicsDesc& baseDesc) const;
    virtual void ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const;
    virtual void ConfigureShadowPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const;

protected:
    std::shared_ptr<Material>     graphicsMaterial_; // shader variant (PSO/RS)
    std::shared_ptr<Material>     shadowMaterial_;

    std::shared_ptr<Mesh> mesh_;
    Math::mat4 modelMatrix_;
    Math::mat4 prevModelMatrix_;

    // CB (upload, per-object)
    bool allowWireframe_ = true;

    void SetUniformBinder(std::unique_ptr<UniformBinder> binder);
    UniformBinder* GetUniformBinder() const { return uniformBinder_.get(); }

private:
    static std::wstring AppendSuffixBeforeExt(const std::wstring& file, const std::wstring& suffix);

    RenderableObject(const RenderableObject&) = delete;
    RenderableObject& operator=(const RenderableObject&) = delete;

    friend class UniformBinder;

    std::unique_ptr<UniformBinder> uniformBinder_;

    std::wstring graphicsShader_;
    std::string  inputLayoutKey_;

protected:
    void SetMesh(std::shared_ptr<Mesh> mesh);
    void MarkWorldBoundsDirty();
    // Crossfade recording state: a Render implementation sets the CURRENT draw's fade before
    // filling b0 (the uniform binder copies it into the cbuffer's lodFade) and clears it after.
    void SetDrawLodFade(float fade) { drawLodFade_ = fade; }

private:
    void RebuildModelMatrix();
    void UpdateWorldBoundsCache() const;

    mutable AABB worldBoundsCache_;
    mutable bool worldBoundsDirty_ = true;
    unsigned int cameraLod_ = 0u; // Step 6: camera LOD chosen in PrepareViews (persists for hysteresis)
    float cameraLodFade_ = 0.0f;  // crossfade weight to cameraLod_+1 (0 = solid), from PrepareViews
    float lodDistanceScale_ = 1.0f; // mesh.json "lodDistanceScale"; see SetLodDistanceScale
    float drawLodFade_ = 0.0f;    // transient: the fade of the draw being recorded (binder reads it)
    std::vector<std::uint8_t> chunkLods_; // per-chunk camera tier (chunked meshes only; hysteresis state)
    std::vector<std::uint8_t> chunkVisCamera_; // S1: per-chunk camera frustum mask (see ChunkCameraVisible)

    Math::float3 pos_{};
    Math::float3 scale_ = Math::float3(1.0f, 1.0f, 1.0f);
    Math::float3 rotEuler_{};
    bool transformDirty_ = true;
    bool prevModelMatrixValid_ = false;
    bool modelMatrixChangedThisTick_ = false;
    bool movedThisFrame_ = false; // render-visible snapshot of modelMatrixChangedThisTick_ (Step 7)
};
