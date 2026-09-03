#pragma once

#include <cstdint>
#include <vector>
#include <d3d12.h>

#include "core/math/AABB.h"
#include "core/math/Math.h"
#include "rendering/RenderLayers.h"
#include "rendering/core/RenderGraph.h"

class Renderer;
class Frustum;

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
    bool mrMultiply = false;               // true = MR texture * metalRough; false = texture override
    // Part C alpha test: >= 0 marks the geometry MASKED — its BLAS entries are built non-opaque
    // and RT candidate hits are kept only when baseColor.a * albedo.a >= alphaCutoff (the raster
    // clip rule). < 0 = opaque geometry, zero traversal cost added.
    float alphaCutoff = -1.0f;
};
class Camera;
class Material;
class MaterialData;
class Mesh;
class IInstanceable;
class RenderableObject;
class InstancedDrawBatch;
class GBufferRenderable;
class TransparentStaticMesh;
class OceanRenderable;
class ParticleEmitterObject;

enum class SceneObjectSyncReason
{
    Frame,
    LevelLoad,
    RuntimeSpawn,
    EditorSpawn,
};

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
    virtual void SyncSceneState(SceneObjectSyncReason reason) { (void)reason; }
    // viewCB: GPU VA of the per-pass shared view/frame constant buffer bound at b1
    // (camera matrices for the gbuffer/transparent pass, light viewProj for shadows).
    virtual void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB) = 0;
#if WITH_EDITOR
    virtual void RenderSelectionStencil(Renderer* renderer, ID3D12GraphicsCommandList* cl, Material* material, const Camera& camera)
    {
        (void)renderer;
        (void)cl;
        (void)material;
        (void)camera;
    }
#endif
    virtual void ExecuteCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) { (void)renderer; (void)cl; }
    // Barrier plan step 5: register the states ExecuteCompute will transition to, in body order.
    // Over-registering costs a redundant barrier; MISSING one is a silent corruption, so mirror
    // the union of reachable branches, not the exact path.
    //
    // pass-flow S7b: RETURNS whether this object's compute will record anything this frame. The
    // Main_ObjectCompute builder collects exactly the objects that answer true and the record body
    // runs THAT list, so the two walks cannot select differently — the pass used to walk the whole
    // scene twice and rely on the two filters agreeing. Default false: an object whose compute
    // records nothing transitions nothing and is skipped by both sides.
    virtual bool PrepareCompute(RenderGraphPassContext& ctx) { (void)ctx; return false; }
    // Async-compute step 9: does this object's ExecuteCompute produce something `Main_ShadowCull`
    // consumes? The object-compute pass is split on this answer, because the two halves have
    // completely different slack: a shadow-cull input must finish two passes later, while the ocean
    // and particle sims are not read until Main_Transparent at the end of the frame — so only the
    // second half can ever move to the async queue.
    //
    // A dedicated question rather than reusing IsGpuInstancedCaster(): that one means "casts
    // shadows through GPU instancing", which happens to be true of the same class today but is a
    // statement about DRAWING. Tying the compute split to it would silently mis-split the moment
    // the two stop coinciding.
    virtual bool ComputeFeedsShadowCull() const { return false; }
    // Same contract for the graphics side: the states Render transitions to. Each pass that
    // draws objects walks its own half of the scene (opaque vs transparent) calling this.
    virtual void PrepareRender(RenderGraphPassContext& ctx) { (void)ctx; }
    // lod: per-pass LOD floor (Step 6 — e.g. the shadow cascade index); clamped to available LODs.
    // `chunkCameraLods`: a CHUNKED mesh's submeshes draw at their per-chunk CAMERA tiers instead of
    // `lod` — pass true ONLY from actual shadow views that want caster == receiver (the Legacy CSM
    // cascades). Data bakes that happen to reuse this entry point (the ocean's shore-depth/SDF
    // top-down renders) MUST leave it false: they need STABLE, camera-independent geometry, and
    // feeding them camera tiers made the baked waterline change with every camera move — the
    // "ocean blinks with wetness on" bug (2026-08-21).
    // `chunkCullFrustum` (occlusion plan S1): the caller's cull volume -- a chunked mesh skips the
    // chunks whose world box misses it, tested ON THE SPOT (never the camera's mask: a chunk behind
    // the camera still casts into the cascade). Null = draw every chunk; only the Legacy CSM
    // cascade loops pass one (their `view.frustum`, the S14 accurate volume).
    virtual void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, D3D12_GPU_VIRTUAL_ADDRESS viewCB, UINT lod = 0, bool chunkCameraLods = false, const Frustum* chunkCullFrustum = nullptr) = 0;
    virtual bool IsTransparent() const = 0;
    virtual bool IsSimpleRender() const = 0;
    // Editor: does the viewport ray-vs-bounds pick consider this object? False for helpers that
    // have no solid surface (e.g. particle emitters, whose swept culling AABB is huge and would
    // hijack drag-drop placement raycasts / click selection). Such objects are selected via the
    // outliner instead.
    virtual bool IsRaycastPickable() const { return true; }
    // RT/SSR (S15b): true only for renderables that SAMPLE the off-screen glass reflection
    // (glass / TransparentStaticMesh). The glass-reflection G-buffer prepass rasterizes only
    // these — NOT every transparent object (e.g. the ocean has its own reflection path and
    // would otherwise flood the glass G-buffer).
    virtual bool UsesGlassReflection() const { return false; }
    virtual bool CastsShadow() const = 0;

    // Rung 0 (Step 6): true for casters whose shadow can't be one entry in the per-caster
    // indirect instance buffer — i.e. GPU-instanced objects (one object → many GPU-driven
    // instances). Such casters are excluded from the GPU cull / ExecuteIndirect path and keep
    // drawing through their own RenderShadow even when indirect shadows are enabled.
    virtual bool IsGpuInstancedCaster() const { return false; }
    // Internal RTTI (the engine forbids dynamic_cast): non-null only for an InstancedDrawBatch,
    // the queue's stand-in for a run of identical objects. A count of queue ENTRIES sees one where
    // there are InstanceCount() casters; anything that counts casters has to look through it.
    virtual const InstancedDrawBatch* AsInstancedDrawBatch() const { return nullptr; }

    // GI→VSM (Step 1): access to a GPU-instanced caster's own per-instance transform buffer, so
    // the GI-scatter compute can fold each instance into the consolidated ShadowGpuData caster
    // set (unified DEFAULT-heap path). GetInstanceCasterSrv is a CPU SRV handle to that
    // StructuredBuffer<InstanceData>; GetInstanceCasterCount is its instance count. Meaningful
    // only for IsGpuInstancedCaster() objects; default null/0 (dormant — no consumer yet).
    virtual D3D12_CPU_DESCRIPTOR_HANDLE GetInstanceCasterSrv() const { return {}; }
    virtual UINT GetInstanceCasterCount() const { return 0; }
    // Occlusion plan S1: how many of those instances the CAMERA pass will draw this frame (after the
    // per-instance frustum test in SelectLod). Shadows keep casting all of them -- the caster count
    // above stays the truth for every shadow view. Read by the visibility counters only.
    virtual UINT GetCameraInstanceCount() const { return GetInstanceCasterCount(); }
    // The instance-caster buffer resource itself, so the GI-scatter pass can transition it to a
    // shader-read state at the call site. Null for non-GPU-instanced casters.
    virtual ID3D12Resource* GetInstanceCasterResource() const { return nullptr; }

    // Rung 1 (Step 10) foundation: does this caster ever move at runtime? Static casters can be
    // cached in a shadow atlas and re-rendered only on invalidation; dynamic ones re-render each
    // frame. Default static; movers (rotating/animated/GPU-driven) override. Content-based:
    // editor moves of a "static" object are caught separately by MovedThisFrame() + the scene's
    // static-set version (Step 11), so this stays about intrinsic motion, not editor state.
    // No consumer yet (Rung 1 caching / Rung 2 page invalidation will use it).
    virtual bool IsDynamicCaster() const { return false; }

    virtual void OnMaterialHotReload(Renderer* renderer) {}

    // Step 6: choose this object's camera/gbuffer LOD for the frame. Called once per visible
    // object in Scene::PrepareViews (NOT during recording) so the per-object/per-instance
    // state (incl. hysteresis) is updated outside the parallel record. Render() just reads it.
    // Shadow LOD is the cascade index, chosen per-pass by the renderer — not here.
    // `cameraFrustum` (occlusion plan S1): the camera view's cull frustum -- the same planes the
    // object-level cull just passed this object through -- for the finer tests below the object:
    // per terrain chunk (RenderableObject) and per GI instance (GpuInstancedModels).
    virtual void SelectLod(const Camera& /*camera*/, const Frustum& /*cameraFrustum*/) {}
    virtual unsigned int GetCameraLod() const { return 0u; }
    // Dithered LOD crossfade weight chosen with the tier in PrepareViews: 0 = solid at
    // GetCameraLod(); in (0,1) = that tier fades OUT and tier+1 fades IN with this weight
    // (both draws recorded — see RenderableObject::Render / InstancedDrawBatch).
    virtual float GetCameraLodFade() const { return 0.0f; }

    // Draw identity for opaque sorting + instanced-run grouping (Step 3/4). Default empty key
    // (mesh-less / transient renderables sort together). See RenderBatchKey above.
    virtual RenderBatchKey BatchKey() const { return {}; }

    // Step 4/5c: auto-instancing capability. Non-null only for renderables that have a
    // CPU-instanced shader variant. A run of objects sharing BatchKey() that all return
    // non-null collapses into one DrawInstanced per pass. Returned pointer is owned by the
    // renderable; valid for the object's lifetime. See IInstanceable.
    virtual const IInstanceable* AsInstanceable() const { return nullptr; }

    // Internal RTTI: checked downcast to RenderableObject without C++ runtime
    // type info (mirrors AsInstanceable). Null for renderables not derived from
    // RenderableObject.
    virtual RenderableObject* AsRenderableObject() { return nullptr; }
    virtual const RenderableObject* AsRenderableObject() const { return nullptr; }
    virtual GBufferRenderable* AsGBufferRenderable() { return nullptr; }
    virtual const GBufferRenderable* AsGBufferRenderable() const { return nullptr; }
    virtual TransparentStaticMesh* AsTransparentStaticMesh() { return nullptr; }
    virtual OceanRenderable* AsOceanRenderable() { return nullptr; }
    virtual ParticleEmitterObject* AsParticleEmitter() { return nullptr; } // E3: editor inspector

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

#if WITH_EDITOR
    void SetEditorObjectId(std::uint64_t id) { editorObjectId_ = id; }
#endif
    std::uint64_t GetEditorObjectId() const
    {
#if WITH_EDITOR
        return editorObjectId_;
#else
        return 0;
#endif
    }

    // Visibility: hidden objects are skipped during bucketization (not drawn in
    // any view) but stay in the scene. Default visible. Used by the editor's
    // "enabled" toggle.
    bool IsVisible() const { return visible_; }
    void SetVisible(bool visible) { visible_ = visible; }

protected:
    uint32_t renderLayerMask_ = RenderLayerMask(RenderLayer::Default);
#if WITH_EDITOR
    std::uint64_t editorObjectId_ = 0;
#endif
    bool visible_ = true;
};
