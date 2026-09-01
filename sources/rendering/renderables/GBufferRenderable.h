#pragma once

#include <atomic>

#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/IInstanceable.h"
#include "materials/MaterialData.h"

class GBufferRenderable : public RenderableObject, public IInstanceable
{
public:
    GBufferRenderable(const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader);

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    GBufferRenderable* AsGBufferRenderable() override { return this; }
    const GBufferRenderable* AsGBufferRenderable() const override { return this; }

    // Slot-0 ("legacy single material") accessors — the factory/editor write these.
    MaterialParams& MaterialParamsRef()
    {
        instanceSlotsCompatibilityKey_.store(0, std::memory_order_relaxed);
        return matParamses_[0];
    }
    const MaterialParams& MaterialParamsRef() const { return matParamses_[0]; }

    // Fields explicitly supplied by the effective object JSON. Material-file defaults are merged
    // per field during Init, so overriding roughness does not accidentally discard emissive, tint,
    // UV transform, or other unrelated defaults from the selected material.
    enum class MaterialParamField : uint32_t
    {
        TexOffsScale = 1u << 0,
        NormalStrength = 1u << 1,
        UseMR = 1u << 2,
        EmissiveColor = 1u << 3,
        EmissiveStrength = 1u << 4,
        MetalRough = 1u << 5
    };
    void MarkMaterialParamOverride(MaterialParamField field)
    {
        materialParamOverrideMask_ |= static_cast<uint32_t>(field);
    }

    // W3: per-object wind sway strength (0 = rigid). Set pre-Init by the factory from the level
    // JSON "windStrength"; ResolveMaterialSlots writes it UNIFORMLY into every slot's MaterialParams
    // so all submeshes of a tree sway in lockstep (it is NOT a per-slot override). A post-Init call
    // (the editor inspector) re-propagates to every slot itself for the same lockstep reason —
    // touching only slot 0 would sway the fronds while the bark stands still.
    void SetWindStrength(float w)
    {
        windStrength_ = w;
        for (MaterialParams& mp : matParamses_) { mp.windStrength = w; }
    }
    float GetWindStrength() const { return windStrength_; }

    // Per-ASSET wind tuning (mesh.json, overridable per object in the level JSON).
    // windTrunkStiffness divides the main bend: >1 stiffer trunk, <1 whippier.
    // windFoliage is one weight PER MATERIAL SLOT (0 = woody, 1 = leaves). Left empty, each slot
    // falls back to its alpha-mask flag - right for cutout leaves, but it misses OPAQUE foliage
    // slots (the staged palm's frond bases are opaque), which is why the explicit list exists.
    void SetWindTrunkStiffness(float s) { windTrunkStiffness_ = s; }
    float GetWindTrunkStiffness() const { return windTrunkStiffness_; }
    // WORLD metres of arc that the baked COLOR_0.b == 1 stands for. The shader bounds a leaf's
    // streaming by this so the leaf never outruns its own length. 0 when the mesh has no wind bake.
    // Uses the largest axis scale: it is a length along the leaf, and over-estimating it only
    // loosens the bound, whereas under-estimating would visibly stiffen the foliage.
    float GetWindLeafScaleWorld() const;
    // W8: windStrength after the distance fade. Every path that feeds the sway — the per-object CB,
    // the instanced array, the shadow CB, the GI scatter — must use THIS, not the raw material
    // value, or the tree and its shadow fade by different amounts and the shadow detaches.
    float EffectiveWindStrength(float rawWindStrength) const;
    void SetWindFoliageWeights(std::vector<float> w) { windFoliageWeights_ = std::move(w); }
    // Resolved foliage weight of a material slot (slot 0 when the index is out of range).
    // ShadowGpuData uses this to give each per-slot caster id its own weight.
    float FoliageForSlot(size_t slot) const
    {
        if (matParamses_.empty()) { return 0.0f; }
        return matParamses_[slot < matParamses_.size() ? slot : 0].windFoliage;
    }

    MaterialData* GetMaterialData() const { return matDatas_.empty() ? nullptr : matDatas_[0].get(); }

    // B2: material slots (submesh i draws with slot subs[i].materialSlot). Single-slot objects
    // behave exactly as before.
    size_t SlotCount() const { return matDatas_.size(); }
    MaterialData* GetMaterialDataForSlot(size_t slot) const
    {
        return slot < matDatas_.size() ? matDatas_[slot].get() : nullptr;
    }
    // Params for the submesh currently being recorded (slot 0 outside the multi-slot loop) —
    // read by the uniform binder when filling b0.
    const MaterialParams& CurrentDrawParams() const
    {
        return matParamses_[currentDrawSlot_ < matParamses_.size() ? currentDrawSlot_ : 0];
    }
    // Pre-Init only: per-slot preset names from level JSON ("materials": [...]); "auto" = glTF.
    void SetSlotPresets(std::vector<std::string> presets)
    {
        if (!presets.empty()) { slotPresets_ = std::move(presets); }
    }

    // Resolved per-slot material name (the level/mesh-asset "materials" that produced this slot,
    // with the object's own override already folded in by ResolveMeshAsset); "auto" for a glTF
    // slot with no explicit preset. The editor Inspector shows this so a slot's *effective*
    // material — e.g. one promoted via the Mesh Editor's "Save as material", which lives on the
    // mesh asset and never touches the object's own JSON — displays correctly.
    std::string SlotPreset(size_t slot) const
    {
        return slot < slotPresets_.size() ? slotPresets_[slot] : std::string("auto");
    }

    // B2: multi-slot objects draw per-submesh (own CB slice + SRV table + ranged draw each).
    void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB) override;

    // RT (S10): augment the base instance with this object's albedo texture +
    // base-color tint so ray hits can be shaded with the real material color.
    bool GetRtInstance(RtInstanceDesc& out) const override
    {
        if (!RenderableObject::GetRtInstance(out))
        {
            return false;
        }
        // Slot 0 represents the object in RT (single albedo per TLAS instance; refine later).
        MaterialData* md = GetMaterialData();
        const MaterialParams& mp = matParamses_[0];
        if (md && md->hasAlbedo)
        {
            out.albedoTex = md->albedo.GetResource();
            out.albedoSrv = md->albedo.GetSRVCPU();
        }
        // Use the MR texture only when this material actually samples it (useMR);
        // the metal/rough grid sets useMR=false and drives metalRough flat instead.
        if (md && md->hasMR && mp.texFlags.y > 0.5f)
        {
            out.mrTex = md->mr.GetResource();
            out.mrSrv = md->mr.GetSRVCPU();
        }
        out.baseColor = mp.baseColor;
        out.metalRough = mp.metalRough; // x=metallic, y=roughness (flat fallback)
        out.mrMultiply = mp.mrMultiply > 0.5f;
        // Part C: masked geometry (foliage) must not be a solid quad in the BVH. The cutoff is
        // the same value the raster clip uses; it only means anything with an albedo to sample.
        out.alphaCutoff = (md && md->hasAlbedo) ? mp.alphaCutoff : -1.0f;
        return true;
    }

    // Draw identity includes the textures (MaterialData) — objects sharing a PSO but not
    // textures must NOT batch together (the single instanced draw binds one texture set).
    // Multi-slot objects key on slot 0: identical assets still sort adjacent; slot sets 1+ are
    // compared by SameInstanceSlots in the queue's run extension (B2b), so the key alone never
    // merges differing slot sets.
    RenderBatchKey BatchKey() const override { return RenderBatchKey{ mesh_.get(), GetGraphicsMaterial(), GetMaterialData() }; }

    // Step 5c: auto-instancing capability via IInstanceable. Non-null only when an instanced
    // shader variant was built at Init (i.e. the default gbuffer shader). B2b: multi-slot
    // objects instance too — their variant carries INSTCB_SLOT_PARAMS and the batch loops
    // submeshes (see InstancedDrawBatch::RecordInstanced).
    const IInstanceable* AsInstanceable() const override
    {
        return instancedGraphicsMaterial_ ? this : nullptr;
    }
    Material* InstancedGraphicsMaterial() const override { return instancedGraphicsMaterial_.get(); }
    Material* InstancedShadowMaterial() const override { return instancedShadowMaterial_.get(); }

    // C1b: per-slot instanced PSO (built in BuildInstancedMaterials for multi-slot objects).
    Material* InstancedGraphicsMaterialForSlot(size_t slot) const override
    {
        if (slot < slotInstancedGraphicsMaterials_.size() && slotInstancedGraphicsMaterials_[slot])
        {
            return slotInstancedGraphicsMaterials_[slot].get();
        }
        return InstancedGraphicsMaterial();
    }
    void FillInstanceData(render::InstancePerObject& out) const override;

    // B2b: slot identity consumed by the queue's batch-compat check and by the multi-slot
    // instanced draw (per-slot SRV staging + per-slot CB fills).
    size_t InstanceSlotCount() const override { return matDatas_.size(); }
    MaterialData* InstanceSlotData(size_t slot) const override { return GetMaterialDataForSlot(slot); }
    const MaterialParams* InstanceSlotParams(size_t slot) const override
    {
        return slot < matParamses_.size() ? &matParamses_[slot] : nullptr;
    }
    std::uint64_t InstanceSlotsCompatibilityKey() const override;

    // True when this object draws per-submesh with per-slot materials (B2). The same predicate
    // decides the instanced-variant flavor (INSTCB_SLOT_PARAMS) in BuildInstancedMaterials and
    // the batch's submesh loop — keep every call site on this one definition.
    bool MultiSlotDraw() const
    {
        const Mesh* m = GetMesh();
        return matDatas_.size() > 1 && m && m->GetSubmeshCount() > 1;
    }

    // A3: subclasses whose model is a glTF/GLB with "material":"auto" return the selector
    // ("path.gltf#N") here so Init builds a runtime auto-material from the glTF instead of a
    // named preset. Empty (default) => use matPreset_. Public so the editor (I3 "Save as
    // material") can read which glTF a still-"auto" slot resolves from.
    virtual std::string GetGltfMaterialSourcePath() const { return {}; }

protected:
    bool RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData) override;
    void ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const override;

    // C1b: bind the current slot's PSO inside the multi-slot submesh loop (slot 0 == the base
    // graphics material, so single-slot objects take the untouched default path).
    Material* CurrentGraphicsMaterial() const override
    {
        if (currentDrawSlot_ < slotGraphicsMaterials_.size() && slotGraphicsMaterials_[currentDrawSlot_])
        {
            return slotGraphicsMaterials_[currentDrawSlot_].get();
        }
        return RenderableObject::CurrentGraphicsMaterial();
    }

    const std::string& MatPreset() const { return slotPresets_[0]; }

private:
    void BuildInstancedMaterials(Renderer* renderer);
    void ResolveMaterialSlots(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void MergeMaterialDefaults(size_t slot, const MaterialParams& defaults);
    // C1b/F2: patch a graphics desc with slot i's pipeline identity: sampling defines,
    // SHADING_MODEL_ID, ALPHA_TEST, and cull mode (two-sided).
    void ApplySlotPipelineOverrides(Material::GraphicsDesc& desc, size_t slot) const;
    // C1b: build slotGraphicsMaterials_ (per-slot PSOs; [0] aliases graphicsMaterial_).
    void BuildSlotMaterials(Renderer* renderer);

    // B2: parallel per-slot arrays (size >= 1 after Init; matParamses_ always has slot 0 so the
    // factory can write params before Init). slotPresets_[i] = preset name or "auto" (glTF).
    std::vector<std::shared_ptr<MaterialData>> matDatas_;
    std::vector<MaterialParams> matParamses_;
    mutable std::atomic<std::uint64_t> instanceSlotsCompatibilityKey_{ 0 };
    std::vector<std::string> slotPresets_;
    uint32_t currentDrawSlot_ = 0;
    uint32_t materialParamOverrideMask_ = 0;
    float windStrength_ = 0.0f; // W3: per-object sway strength, applied to every slot at Init
    float windTrunkStiffness_ = 1.0f;       // per-object; divides the main bend
    std::vector<float> windFoliageWeights_; // per-slot; empty => derive from alphaMask

    // Instanced (cbuffer-array) variants of the gbuffer + shadow materials, built once at
    // Init when this object's graphics shader has an instanced counterpart. Shared/cached
    // across all objects of the same material by MaterialManager.
    std::shared_ptr<Material> instancedGraphicsMaterial_;
    std::shared_ptr<Material> instancedShadowMaterial_;

    // C1b: per-slot PSOs for multi-slot objects (empty for single-slot => base-material path).
    // [0] aliases graphicsMaterial_/instancedGraphicsMaterial_; a failed slot build falls back
    // to the slot-0 material (the pre-C1b behavior). MaterialManager desc-caching dedups these
    // across objects, so all palms share the same opaque + masked pipelines.
    std::vector<std::shared_ptr<Material>> slotGraphicsMaterials_;
    std::vector<std::shared_ptr<Material>> slotInstancedGraphicsMaterials_;
};
