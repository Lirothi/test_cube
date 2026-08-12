#include "rendering/renderables/GBufferRenderable.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "app/camera/Camera.h"
#include "rendering/core/Renderer.h"
#include "materials/MaterialDataManager.h"
#include "vfx/WindState.h" // W8: WindDistanceFade

namespace
{
uint32_t ToObjectId32(std::uint64_t id)
{
    return id > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(id);
}

class GBufferUniformBinder final : public RenderableObject::UniformBinder
{
public:
    GBufferUniformBinder() = default;

    void RebuildHandles(RenderableObject& owner) override
    {
        cbHandles_ = {};
        shadowHandles_ = {};

        if (Material* material = owner.GetGraphicsMaterial())
        {
            // Per-object only (b0). The view matrices now live in the shared
            // per-view CB (b1), filled once per pass by the renderer.
            cbHandles_.world = material->ComputeCBFieldHandle(0, "world");
            cbHandles_.prevWorld = material->ComputeCBFieldHandle(0, "prevWorld");
            cbHandles_.baseColor = material->ComputeCBFieldHandle(0, "baseColor");
            cbHandles_.metalRough = material->ComputeCBFieldHandle(0, "metalRough");
            cbHandles_.alphaCutoff = material->ComputeCBFieldHandle(0, "alphaCutoff");
            cbHandles_.mrMultiply = material->ComputeCBFieldHandle(0, "mrMultiply");
            cbHandles_.texOffsScale = material->ComputeCBFieldHandle(0, "texOffsScale");
            cbHandles_.texFlags = material->ComputeCBFieldHandle(0, "texFlags");
            cbHandles_.emissive = material->ComputeCBFieldHandle(0, "emissive");
            cbHandles_.windStrength = material->ComputeCBFieldHandle(0, "windStrength");
            cbHandles_.windFoliage = material->ComputeCBFieldHandle(0, "windFoliage");
            cbHandles_.windTrunkStiff = material->ComputeCBFieldHandle(0, "windTrunkStiff");
            cbHandles_.windLeafScale = material->ComputeCBFieldHandle(0, "windLeafScale");
            cbHandles_.objectId = material->ComputeCBFieldHandle(0, "objectId");
        }

        if (Material* shadowMaterial = owner.GetShadowMaterial())
        {
            // viewProj (light) now comes from the shared per-view CB (b1).
            shadowHandles_.world = shadowMaterial->ComputeCBFieldHandle(0, "world");
            shadowHandles_.windStrength = shadowMaterial->ComputeCBFieldHandle(0, "windStrength");
            shadowHandles_.windFoliage = shadowMaterial->ComputeCBFieldHandle(0, "windFoliage");
            shadowHandles_.windTrunkStiff = shadowMaterial->ComputeCBFieldHandle(0, "windTrunkStiff");
            shadowHandles_.windLeafScale = shadowMaterial->ComputeCBFieldHandle(0, "windLeafScale");
        }
    }

    void UpdateMainCB(RenderableObject& owner, Renderer* /*renderer*/, const Camera& /*camera*/, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material) { return; }

        UpdateUniform(owner, cbHandles_.world, material, owner.GetModelMatrix(), cbData);
        UpdateUniform(owner, cbHandles_.prevWorld, material, owner.GetPreviousModelMatrix(), cbData);

        // B2: pull the params of the slot being recorded (slot 0 outside the multi-slot loop).
        const GBufferRenderable* gb = owner.AsGBufferRenderable();
        const MaterialParams defaults{};
        const auto& p = gb ? gb->CurrentDrawParams() : defaults;
        UpdateUniform(owner, cbHandles_.baseColor, material, p.baseColor, cbData);
        UpdateUniform(owner, cbHandles_.metalRough, material, p.metalRough, cbData);
        UpdateUniform(owner, cbHandles_.alphaCutoff, material, p.alphaCutoff, cbData);
        UpdateUniform(owner, cbHandles_.mrMultiply, material, p.mrMultiply, cbData);
        UpdateUniform(owner, cbHandles_.texOffsScale, material, p.texOffsScale, cbData);
        UpdateUniform(owner, cbHandles_.texFlags, material, p.texFlags, cbData);
        UpdateUniform(owner, cbHandles_.emissive, material, p.EmissiveLinear(), cbData);
        UpdateUniform(owner, cbHandles_.windStrength, material,
                      gb ? gb->EffectiveWindStrength(p.windStrength) : p.windStrength, cbData);
        UpdateUniform(owner, cbHandles_.windFoliage, material, p.windFoliage, cbData); // per-slot
        UpdateUniform(owner, cbHandles_.windTrunkStiff, material,
                      gb ? gb->GetWindTrunkStiffness() : 1.0f, cbData);
        UpdateUniform(owner, cbHandles_.windLeafScale, material,
                      gb ? gb->GetWindLeafScaleWorld() : 0.0f, cbData);
        UpdateUniform(owner, cbHandles_.objectId, material, ToObjectId32(owner.GetEditorObjectId()), cbData);
    }

    void UpdateShadowCB(RenderableObject& owner, Renderer* /*renderer*/, const mat4& /*lightView*/, const mat4& /*lightProj*/, uint8_t* cbData) override
    {
        Material* material = owner.GetShadowMaterial();
        if (!material) { return; }

        // viewProj (light) is written once per cascade into the shared per-view CB (b1).
        UpdateUniform(owner, shadowHandles_.world, material, owner.GetModelMatrix(), cbData);
        // W5: the CPU-fallback shadow VS reads windStrength from this b0. RenderShadow does NOT
        // clear the dynamic CB allocation, so leaving it unwritten feeds the sway garbage from
        // recycled frame-ring memory (observed as a stray, wildly displaced shadow). Write it.
        const GBufferRenderable* gb = owner.AsGBufferRenderable();
        const MaterialParams shadowDefaults{};
        const auto& sp = gb ? gb->CurrentDrawParams() : shadowDefaults;
        UpdateUniform(owner, shadowHandles_.windStrength, material,
                      gb ? gb->EffectiveWindStrength(sp.windStrength) : sp.windStrength, cbData);
        UpdateUniform(owner, shadowHandles_.windFoliage, material, sp.windFoliage, cbData);
        UpdateUniform(owner, shadowHandles_.windTrunkStiff, material,
                      gb ? gb->GetWindTrunkStiffness() : 1.0f, cbData);
        UpdateUniform(owner, shadowHandles_.windLeafScale, material,
                      gb ? gb->GetWindLeafScaleWorld() : 0.0f, cbData);
    }

private:
    struct CBHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle prevWorld;
        Material::CBFieldHandle baseColor;
        Material::CBFieldHandle metalRough;
        Material::CBFieldHandle alphaCutoff;
        Material::CBFieldHandle mrMultiply;
        Material::CBFieldHandle texOffsScale;
        Material::CBFieldHandle texFlags;
        Material::CBFieldHandle emissive;
        Material::CBFieldHandle windStrength;
        Material::CBFieldHandle windFoliage;
        Material::CBFieldHandle windTrunkStiff;
        Material::CBFieldHandle windLeafScale;
        Material::CBFieldHandle objectId;
    } cbHandles_{};

    struct ShadowCBHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle windStrength;  // W5
        Material::CBFieldHandle windFoliage;
        Material::CBFieldHandle windTrunkStiff;
        Material::CBFieldHandle windLeafScale;
    } shadowHandles_{};
};
} // namespace

// B2b: batch-compat predicate for the queue's run extension (declared on IInstanceable; defined
// here for MaterialParams' definition). Multi-slot batches bind slot textures and upload slot
// params ONCE from the run's lead, so members must match it exactly. Single-slot pairs always
// pass — their params travel in the per-instance array instead.
bool IInstanceable::SameInstanceSlots(const IInstanceable& other) const
{
    const size_t n = InstanceSlotCount();
    if (n != other.InstanceSlotCount()) { return false; }
    if (n <= 1) { return true; }

    const std::uint64_t key = InstanceSlotsCompatibilityKey();
    const std::uint64_t otherKey = other.InstanceSlotsCompatibilityKey();
    if (key != 0 && otherKey != 0) { return key == otherKey; }

    const auto eq4 = [](const float4& a, const float4& b)
    { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; };
    const auto eq2 = [](const float2& a, const float2& b)
    { return a.x == b.x && a.y == b.y; };

    for (size_t i = 0; i < n; ++i)
    {
        if (InstanceSlotData(i) != other.InstanceSlotData(i)) { return false; }
        const MaterialParams* a = InstanceSlotParams(i);
        const MaterialParams* b = other.InstanceSlotParams(i);
        if (!a || !b) { if (a != b) { return false; } continue; }
        if (!eq4(a->baseColor, b->baseColor) || !eq2(a->metalRough, b->metalRough) ||
            a->mrMultiply != b->mrMultiply || !eq4(a->texOffsScale, b->texOffsScale) ||
            !eq4(a->texFlags, b->texFlags))
        {
            return false;
        }
    }
    return true;
}

std::uint64_t GBufferRenderable::InstanceSlotsCompatibilityKey() const
{
    std::uint64_t cached = instanceSlotsCompatibilityKey_.load(std::memory_order_relaxed);
    if (cached != 0) { return cached; }

    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t value) { h ^= value; h *= 1099511628211ull; };
    const auto mixFloat = [&mix](float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    };

    mix(static_cast<std::uint64_t>(matParamses_.size()));
    for (size_t i = 0; i < matParamses_.size(); ++i)
    {
        mix(reinterpret_cast<std::uint64_t>(GetMaterialDataForSlot(i)));
        const MaterialParams& p = matParamses_[i];
        mixFloat(p.baseColor.x); mixFloat(p.baseColor.y);
        mixFloat(p.baseColor.z); mixFloat(p.baseColor.w);
        mixFloat(p.metalRough.x); mixFloat(p.metalRough.y);
        mixFloat(p.mrMultiply);
        mixFloat(p.texOffsScale.x); mixFloat(p.texOffsScale.y);
        mixFloat(p.texOffsScale.z); mixFloat(p.texOffsScale.w);
        mixFloat(p.texFlags.x); mixFloat(p.texFlags.y);
        mixFloat(p.texFlags.z); mixFloat(p.texFlags.w);
    }

    if (h == 0) { h = 1; } // zero is the uncached/fallback sentinel
    instanceSlotsCompatibilityKey_.store(h, std::memory_order_relaxed);
    return h;
}

GBufferRenderable::GBufferRenderable(const std::string& matPreset,
    const std::string& inputLayout,
    const std::wstring& graphicsShader)
    : RenderableObject(inputLayout, graphicsShader)
{
    slotPresets_ = { matPreset };
    matParamses_.resize(1); // slot 0 exists pre-Init so the factory can write params
    SetUniformBinder(std::make_unique<GBufferUniformBinder>());
}

void GBufferRenderable::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!renderer)
    {
        return;
    }

    if (matDatas_.empty())
    {
        ResolveMaterialSlots(renderer, uploadCmdList, uploadKeepAlive);
    }

    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);

    BuildSlotMaterials(renderer);
    BuildInstancedMaterials(renderer);
}

void GBufferRenderable::ApplySlotPipelineOverrides(Material::GraphicsDesc& desc, size_t slot) const
{
    // C1b: a slot's pipeline identity. Sampling defines are erase+re-add (ConfigureDefines...),
    // so patching a desc that already carries another slot's values is safe. Pre-Init calls
    // (empty matDatas_, e.g. IsTransparent's speculative desc) leave the desc untouched — same
    // as the old null-matData path.
    if (slot >= matDatas_.size() || !matDatas_[slot])
    {
        return;
    }
    const MaterialData& md = *matDatas_[slot];
    md.ConfigureDefinesForGBuffer(desc); // sampling layout + SHADING_MODEL_ID

    auto& defs = desc.defines;
    defs.erase(std::remove_if(defs.begin(), defs.end(),
        [](const auto& p) { return p.first == "ALPHA_TEST"; }), defs.end());
    if (md.alphaMask)
    {
        defs.emplace_back("ALPHA_TEST", "1");
    }
    desc.raster.CullMode = md.doubleSided ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
}

void GBufferRenderable::BuildSlotMaterials(Renderer* renderer)
{
    slotGraphicsMaterials_.clear();
    if (!renderer || matDatas_.size() <= 1 || !GetGraphicsMaterial())
    {
        return; // single-slot: the base material path, byte-identical to pre-C1b
    }

    slotGraphicsMaterials_.resize(matDatas_.size());
    slotGraphicsMaterials_[0] = graphicsMaterial_; // slot 0 == the object's own material

    constexpr UINT kAlign = render::kConstantBufferAlignment;
    const UINT baseCbSize = graphicsMaterial_->GetCBSizeBytesAligned(0, kAlign);

    for (size_t i = 1; i < matDatas_.size(); ++i)
    {
        Material::GraphicsDesc gd = BuildGraphicsDesc(renderer); // slot-0 configured desc
        ApplySlotPipelineOverrides(gd, i);
        // I0: per-slot material shader override. The slot-0 desc may already carry slot 0's
        // override — reset to this slot's choice (its own override, else the object's shader).
        gd.shaderFile = (matDatas_[i] && !matDatas_[i]->shaderOverride.empty())
            ? matDatas_[i]->shaderOverride
            : GetGraphicsShaderPath();
        auto m = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
        // The uniform binder writes b0 through slot-0 field handles — every slot permutation
        // must share the PerObject layout (defines never change the cbuffer struct). Guard it:
        // a mismatching (or failed) slot PSO falls back to slot 0, i.e. the pre-C1b behavior.
        const bool usable = m && m->GetPipelineState() &&
            m->GetCBSizeBytesAligned(0, kAlign) == baseCbSize;
        if (!usable && m)
        {
            OutputDebugStringA("[gbuffer] slot material unusable (PSO/CB layout); falling back to slot 0\n");
        }
        slotGraphicsMaterials_[i] = usable ? m : graphicsMaterial_;
    }
}

void GBufferRenderable::MergeMaterialDefaults(size_t slot, const MaterialParams& defaults)
{
    if (slot >= matParamses_.size())
    {
        return;
    }

    // Effective object JSON only exposes slot-0 overrides. Other slots inherit their material
    // defaults wholesale. Keep the explicit values before replacing the destination, then restore
    // only fields whose JSON keys were actually present. An explicit value may equal the engine
    // default, so comparing values cannot represent override intent.
    if (slot != 0 || materialParamOverrideMask_ == 0)
    {
        matParamses_[slot] = defaults;
        return;
    }

    const MaterialParams explicitValues = matParamses_[slot];
    matParamses_[slot] = defaults;
    const auto overridden = [this](MaterialParamField field)
    {
        return (materialParamOverrideMask_ & static_cast<uint32_t>(field)) != 0;
    };

    if (overridden(MaterialParamField::TexOffsScale))
    {
        matParamses_[slot].texOffsScale = explicitValues.texOffsScale;
    }
    if (overridden(MaterialParamField::NormalStrength))
    {
        matParamses_[slot].texFlags.w = explicitValues.texFlags.w;
    }
    if (overridden(MaterialParamField::UseMR))
    {
        matParamses_[slot].texFlags.y = explicitValues.texFlags.y;
    }
    if (overridden(MaterialParamField::EmissiveColor))
    {
        matParamses_[slot].emissiveColor = explicitValues.emissiveColor;
    }
    if (overridden(MaterialParamField::EmissiveStrength))
    {
        matParamses_[slot].emissiveStrength = explicitValues.emissiveStrength;
    }
    if (overridden(MaterialParamField::MetalRough))
    {
        matParamses_[slot].metalRough = explicitValues.metalRough;
    }
}

void GBufferRenderable::ResolveMaterialSlots(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    MaterialDataManager* mgr = renderer->GetMaterialDataManager();
    const std::string gltfSrc = GetGltfMaterialSourcePath(); // "" unless the model is a glTF
    const Mesh* mesh = GetMesh();
    const size_t submeshCount = mesh ? std::max<size_t>(mesh->GetSubmeshCount(), 1u) : 1u;

    // glTF meshes get one slot per submesh (requires the mesh to be loaded BEFORE Init — see
    // StaticMesh::Init); everything else keeps the explicit preset list (usually 1).
    const size_t slotCount = !gltfSrc.empty()
        ? std::max<size_t>(submeshCount, slotPresets_.size())
        : std::max<size_t>(slotPresets_.size(), 1u);

    matDatas_.assign(slotCount, nullptr);
    matParamses_.resize(slotCount); // slot 0 keeps factory-applied values; new slots default

    for (size_t i = 0; i < slotCount; ++i)
    {
        const std::string name = i < slotPresets_.size() ? slotPresets_[i] : std::string("auto");
        const bool wantsGltf = !gltfSrc.empty() && (name.empty() || name == "auto");
        if (wantsGltf)
        {
            // Multi-submesh: ordinal i addresses submesh/group i. Single-submesh: honor the
            // selector embedded in the path (e.g. "#2") instead of forcing ordinal 0.
            const int ordinal = submeshCount > 1 ? static_cast<int>(i) : -1;
            matDatas_[i] = mgr->GetOrCreateFromGltf(renderer, uploadCmdList, uploadKeepAlive, gltfSrc, ordinal);
            if (matDatas_[i] && matDatas_[i]->fromGltf)
            {
                MergeMaterialDefaults(i, matDatas_[i]->gltfDefaultParams);
            }
            else if (!matDatas_[i])
            {
                // Null-material group: draw flat, sample nothing.
                matParamses_[i] = MaterialParams{};
                matParamses_[i].SetUseAlbedo(false);
                matParamses_[i].SetUseMR(false);
                matParamses_[i].SetUseNormal(false);
            }
        }
        else
        {
            matDatas_[i] = mgr->GetOrCreate(renderer, uploadCmdList, uploadKeepAlive, name);
            // Material files carry defaults; effective object JSON overrides merge per field.
            // Param-less materials leave the pre-Init values untouched.
            if (matDatas_[i] && matDatas_[i]->hasPresetParams)
            {
                MergeMaterialDefaults(i, matDatas_[i]->presetParams);
            }
            // A missing preset (including a material that was just created but not registered
            // yet) must render as a flat fallback. Leaving the default texture flags enabled
            // would sample the descriptor table from an unrelated preceding draw.
            matParamses_[i].SetUseAlbedo(matDatas_[i] && matDatas_[i]->hasAlbedo);
            const bool wantsMR = matParamses_[i].texFlags.y > 0.5f;
            matParamses_[i].SetUseMR(matDatas_[i] && matDatas_[i]->hasMR && wantsMR);
            matParamses_[i].SetUseNormal(matDatas_[i] && matDatas_[i]->hasNormal);
        }
    }

    // C1: per-slot alpha-test cutoff. Masked slots clip at the glTF alphaCutoff; every other slot
    // uses -1 so it never clips even when the object's PSO carries ALPHA_TEST (union across slots).
    for (size_t i = 0; i < slotCount; ++i)
    {
        const MaterialData* md = matDatas_[i].get();
        matParamses_[i].alphaCutoff = (md && md->alphaMask) ? md->alphaCutoff : -1.0f;
    }

    // W3: write the object's wind sway strength into EVERY slot (uniform across submeshes — the
    // submesh-sync anchor). glTF palms and preset meshes both flow through here, so a flagged tree
    // bends identically across its trunk + frond slots. This runs AFTER MergeMaterialDefaults so it
    // is never clobbered by the per-slot material reseed.
    for (size_t i = 0; i < slotCount; ++i)
    {
        matParamses_[i].windStrength = windStrength_;
        // Per-slot foliage weight. The authored list wins; otherwise fall back to the slot's
        // alpha-mask flag, which is right for cutout leaves but misses OPAQUE foliage (the staged
        // palm's frond bases) - exactly the gap the authored list exists to close.
        if (i < windFoliageWeights_.size())
        {
            matParamses_[i].windFoliage = windFoliageWeights_[i];
        }
        else
        {
            const MaterialData* md = matDatas_[i].get();
            matParamses_[i].windFoliage = (md && md->alphaMask) ? 1.0f : 0.0f;
        }
    }

    // A per-slot foliage weight scales a vertex DISPLACEMENT, so a step in it across a shared seam
    // used to pull the two sides apart (coconut_palm slots 2/3 share 36 welded vertices and the
    // authored 1.0/0.0 split tore every frond off its base by 0.94 m at strong wind). That is fixed
    // in the BAKE, not here: the along-limb weight is now a distance from the wood surface, so it is
    // exactly 0 wherever foliage meets wood and `foliage * b` is continuous whatever the artist
    // authored. Do not "helpfully" unify the weights across welded slots here -- an earlier attempt
    // did, and silently overriding the authored split promoted the petioles to full foliage, which
    // exposed a 1.3 m step at the petiole/blade junction instead.
    // C1b: mixed defines across slots are fully supported now — each slot gets its own PSO in
    // BuildSlotMaterials (the old "slot 0's PSO wins" warning is obsolete).
}

void GBufferRenderable::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    Mesh* mesh = GetMesh();
    if (!MultiSlotDraw())
    {
        currentDrawSlot_ = 0;
        RenderableObject::Render(renderer, cl, camera, viewCB);
        return;
    }
    if (!renderer || !cl || !GetGraphicsMaterial()) { return; }

    // Per-submesh recording: each submesh gets its own b0 slice (slot params), its slot's SRV
    // table, and a ranged draw. World/prevWorld repeat per slice — simple and correct; a shared
    // per-object CB split is a later optimization if palms ever multiply.
    const UINT lod = GetCameraLod();
    const auto& subs = mesh->SubmeshesForLod(lod);
    constexpr UINT kAlign = render::kConstantBufferAlignment;
    const UINT cbSizeBytes = GetGraphicsMaterial()->GetCBSizeBytesAligned(0, kAlign);

    for (size_t s = 0; s < subs.size(); ++s)
    {
        currentDrawSlot_ = subs[s].materialSlot < matDatas_.size()
            ? subs[s].materialSlot
            : static_cast<uint32_t>(matDatas_.size() - 1);

        auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSizeBytes, kAlign);
        uint8_t* cbData = static_cast<uint8_t*>(alloc.cpu);
        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = h.ref();
        ctx.cbv[0] = alloc.gpu;
        ctx.cbv[1] = viewCB;

        RecordGraphics(renderer, cl, ctx, camera, cbData); // stages the slot's SRVs + binds
        mesh->DrawSubmesh(cl, static_cast<UINT>(s), lod);
    }
    currentDrawSlot_ = 0;
}

void GBufferRenderable::BuildInstancedMaterials(Renderer* renderer)
{
    // Step 4: only the default gbuffer shader has cbuffer-array instanced counterparts
    // (gbuffer_instcb.hlsl + gbuffer_instcb_csm.hlsl). Build them with the SAME pipeline
    // config + material defines as the per-object materials so instanced draws match
    // pixel-for-pixel. MaterialManager caches by desc, so all objects of one material
    // share a single instanced PSO. Both gbuffer + shadow variants are required; if either
    // fails to compile we disable instancing for this object (no half-instanced state).
    if (!renderer) { return; }
    if (GetGraphicsShaderPath() != L"shaders/gbuffer.hlsl") { return; }
    // I0: material shader overrides have no instanced counterpart — a batch would silently draw
    // them with the plain instanced shader (no sway etc.). Disable instancing for such objects.
    for (const auto& md : matDatas_)
    {
        if (md && !md->shaderOverride.empty()) { return; }
    }

    Material::GraphicsDesc gd = BuildGraphicsDesc(renderer);
    gd.shaderFile = L"shaders/gbuffer_instcb.hlsl";

    std::shared_ptr<Material> shadow;
    if (CastsShadow())
    {
        // Shadow desc built BEFORE the slot-params define: depth-only ignores materials, so
        // multi- and single-slot objects share one instanced CSM PSO per define set.
        Material::GraphicsDesc sd = BuildShadowDesc(renderer, gd); // -> gbuffer_instcb_csm.hlsl
        shadow = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, sd);
        if (!shadow || !shadow->GetPipelineState()) { return; }
    }

    // B2b: multi-slot objects instance through the per-slot-CB variant (the batch loops
    // submeshes, binding each slot's textures + a b2 params slice — see InstancedDrawBatch).
    if (MultiSlotDraw())
    {
        gd.defines.emplace_back("INSTCB_SLOT_PARAMS", "1");
    }
    auto gfx = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    if (!gfx || !gfx->GetPipelineState()) { return; }

    instancedGraphicsMaterial_ = std::move(gfx);
    instancedShadowMaterial_ = std::move(shadow);

    // C1b: per-slot instanced PSOs (same desc, slot defines/cull patched per slot). The batch
    // binds these inside its submesh loop; a failed slot build falls back to the slot-0 variant.
    slotInstancedGraphicsMaterials_.clear();
    if (MultiSlotDraw())
    {
        slotInstancedGraphicsMaterials_.resize(matDatas_.size());
        slotInstancedGraphicsMaterials_[0] = instancedGraphicsMaterial_;
        for (size_t i = 1; i < matDatas_.size(); ++i)
        {
            Material::GraphicsDesc sgd = gd; // instcb + INSTCB_SLOT_PARAMS, slot-0 configured
            ApplySlotPipelineOverrides(sgd, i);
            auto sm = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, sgd);
            slotInstancedGraphicsMaterials_[i] =
                (sm && sm->GetPipelineState()) ? sm : instancedGraphicsMaterial_;
        }
    }
}

void GBufferRenderable::FillInstanceData(render::InstancePerObject& out) const
{
    out.world = GetModelMatrix().m;
    out.prevWorld = GetPreviousModelMatrix().m;

    // Slot 0 params. Multi-slot instanced draws (B2b) ignore these material fields — the PS
    // reads the per-slot CB (b2) instead — but world/prevWorld/objectId stay per-instance.
    const MaterialParams& p = matParamses_[0];
    out.baseColor = DirectX::XMFLOAT4(p.baseColor.x, p.baseColor.y, p.baseColor.z, p.baseColor.w);
    out.metalRough = DirectX::XMFLOAT2(p.metalRough.x, p.metalRough.y);
    out.alphaCutoff = p.alphaCutoff; // C1 (single-slot instanced; multi-slot uses b2)
    out.mrMultiply = p.mrMultiply;
    out.texOffsScale = DirectX::XMFLOAT4(p.texOffsScale.x, p.texOffsScale.y, p.texOffsScale.z, p.texOffsScale.w);
    out.texFlags = DirectX::XMFLOAT4(p.texFlags.x, p.texFlags.y, p.texFlags.z, p.texFlags.w);
    out.objectId = ToObjectId32(GetEditorObjectId());
    const auto e = p.EmissiveLinear();
    out.emissive = DirectX::XMFLOAT3(e.x, e.y, e.z);
    out.windStrength = EffectiveWindStrength(p.windStrength); // W3 uniform per object; W8 distance fade
    out.windLeafScale = GetWindLeafScaleWorld();
    // Slot 0's foliage weight. ShadowGpuData overwrites this PER SLOT (its caster ids are already
    // one per slot) and the multi-slot instanced gbuffer reads its own from the slot CB.
    out.windFoliage = p.windFoliage;
    out.windTrunkStiff = windTrunkStiffness_;
}

float GBufferRenderable::EffectiveWindStrength(float rawWindStrength) const
{
    if (rawWindStrength <= 0.0f) { return 0.0f; } // keep the exact no-wind early-out
    return rawWindStrength * vfx::WindDistanceFade(GetPosition());
}

float GBufferRenderable::GetWindLeafScaleWorld() const
{
    const Mesh* mesh = GetMesh();
    if (!mesh) { return 0.0f; }
    const float objScale = mesh->GetWindLeafScale();
    if (!(objScale > 0.0f)) { return 0.0f; }
    const Math::float3 s = GetScale();
    return objScale * std::max({ std::abs(s.x), std::abs(s.y), std::abs(s.z) });
}

void GBufferRenderable::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData)
{
    if (!renderer)
    {
        return;
    }

    if (MaterialData* md = GetMaterialDataForSlot(currentDrawSlot_))
    {
        md->StageGBufferBindings(renderer, ctx, 0, 0);
        md->StageGBufferSurfaceParams(renderer, ctx, 2);
    }
    else
    {
        MaterialData::StageNeutralGBufferSurfaceParams(renderer, ctx, 2);
    }

    RenderableObject::RecordGraphics(renderer, cl, ctx, camera, cbData);
}

void GBufferRenderable::ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const
{
    RenderableObject::ConfigureGraphicsPipeline(renderer, desc);

#if WITH_EDITOR
    desc.numRT = 6;
#else
    desc.numRT = 5;
#endif
    if (renderer)
    {
        desc.rtvFormats[0] = renderer->GetGBuffer0Format();
        desc.rtvFormats[1] = renderer->GetGBuffer1Format();
        desc.rtvFormats[2] = renderer->GetGBuffer2Format();
        desc.rtvFormats[3] = renderer->GetGBufferVelocityFormat();
#if WITH_EDITOR
        desc.rtvFormats[4] = renderer->GetObjectIdFormat();
        desc.rtvFormats[5] = renderer->GetGBufferAuxFormat();
#else
        desc.rtvFormats[4] = renderer->GetGBufferAuxFormat();
#endif
        desc.dsvFormat = renderer->GetDeferredDepthFormat();
    }

    // C1b: the object's base material IS slot 0's pipeline — sampling defines + ALPHA_TEST +
    // cull mode come from slot 0's MaterialData (per-slot PSOs replace C1's union-of-flags;
    // slots 1+ get their own materials in BuildSlotMaterials).
    ApplySlotPipelineOverrides(desc, 0);
#if WITH_EDITOR
    desc.defines.emplace_back("EDITOR_OBJECT_ID", "1");
#endif
    // I0: a material file can override the gbuffer shader ("shader" key — vegetation sway etc.).
    // Applied here, NOT in ApplySlotPipelineOverrides — the instanced descs reuse that helper
    // with their own shader. The shadow PSO follows automatically: BuildShadowDesc derives
    // <shader>_csm.hlsl from this desc's shaderFile.
    if (!matDatas_.empty() && matDatas_[0] && !matDatas_[0]->shaderOverride.empty())
    {
        desc.shaderFile = matDatas_[0]->shaderOverride;
    }

    desc.depth.StencilEnable = TRUE;
    desc.depth.StencilReadMask = 0x80;
    desc.depth.StencilWriteMask = 0x80;
    desc.depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.depth.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    desc.depth.BackFace = desc.depth.FrontFace;
}
