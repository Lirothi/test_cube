#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>
#include <d3d12.h>

#include "materials/Texture2D.h"
#include "rendering/core/RenderContext.h"
#include "materials/Material.h"
#include "core/math/Math.h"

class Renderer;

// Stored as a four-bit ID in GBAux.b (encoded as id/15 in an R8 UNORM channel). Keep these numeric
// values in sync with kShadingModel* in shaders/utils.hlsli.
enum class ShadingModel : uint8_t
{
    DefaultLit = 0,
    TwoSidedFoliage = 1,
    Terrain = 2,
    // Reserved for the ocean surface, which writes it from its own forward draw after compose. Not a
    // material's choice: TryParseShadingModel does not accept it, so no material file can select it.
    Water = 3,
    // UE's MSM_ThinTranslucent (glTF KHR_materials_transmission): glass, drawn by the forward
    // translucent pass (glass.hlsl GLASS_MATERIAL) and never written to the G-buffer. A slot with an
    // alpha test SPLITS -- the texels the cutout keeps are opaque and take the G-buffer as before,
    // the ones it removes are glass; a slot without one is all glass and leaves every opaque path
    // (G-buffer, shadows, RT). See MaterialData::IsGlassOnly.
    ThinTranslucent = 4
};
static_assert(static_cast<uint8_t>(ShadingModel::DefaultLit) == 0);
static_assert(static_cast<uint8_t>(ShadingModel::TwoSidedFoliage) == 1);
static_assert(static_cast<uint8_t>(ShadingModel::Terrain) == 2);
static_assert(static_cast<uint8_t>(ShadingModel::Water) == 3);
static_assert(static_cast<uint8_t>(ShadingModel::ThinTranslucent) < 16);

const char* ShadingModelToString(ShadingModel model);
bool TryParseShadingModel(std::string_view text, ShadingModel& outModel);

// Material-static surface payload written to GBAux/GB2. These values do not belong in the
// per-object/instance transform payload: every object using a material shares them.
struct MaterialSurfaceParams
{
    float3 subsurfaceColor = { 1.0f, 1.0f, 1.0f };
    float transmissionStrength = 0.0f;
    float ambientOcclusion = 1.0f;
    float indirectSpecularScale = 1.0f;
    // Treat linear albedo as a unit-distance transmission proxy: T = albedo^power.
    // Zero preserves the old uniform payload; larger values increase color/thickness contrast.
    float transmissionAlbedoPower = 0.6f;
    // Blend between broad thin-sheet wrap (0) and abs(N.L) projected-area weighting (1).
    float transmissionNormalWeight = 0.35f;
    // Terrain texture-bombing controls. Zone size is measured in repeats of the already transformed
    // source UV; rotation is authored in degrees and converted for the GPU upload.
    float terrainZoneSize = 4.0f;
    float terrainRotationDegrees = 180.0f;
    float terrainScaleVariation = 0.25f;
    float terrainBlend = 0.35f;
    // Continuous domain warp applied only to the procedural zone map. Breakup is measured in
    // zone-space; detail is the base noise frequency per zone. The shader caps breakup against
    // detail so the warped zone map stays injective (see TerrainWarpZoneMap): this default pair
    // sits just under that limit.
    float terrainEdgeBreakup = 0.09f;
    float terrainEdgeDetail = 3.5f;
};

// ---------------------
// Per-object parameters (in b0)
// ---------------------
struct MaterialParams
{
    // Linear values; the SRV handles sRGB sampling for albedo
    float4 baseColor   = {1.f, 1.f, 1.f, 1.f};  // .rgb — tint, .a — alpha factor (glTF)
    float2 metalRough  = {0.0f, 0.35f};         // x=metallic, y=roughness
    // C1 alpha test cutoff: fragment discarded when baseColor.a*albedo.a < cutoff. -1 disables
    // the test for this slot (the shader skips clip); set to the glTF alphaCutoff on MASK slots.
    float  alphaCutoff = -1.0f;
    // 0 = an enabled MR texture overrides metalRough; 1 = texture * metalRough.
    float  mrMultiply  = 0.0f;
    float4 texOffsScale = { 0.0f, 0.0f, 1.0f, 1.0f };
    // x=useAlbedo, y=useMR, z=useNormal, w=normalStrength (XY before reconstructing Z)
    float4 texFlags    = {1.f, 1.f, 1.f, 1.f};
    // D: self-illumination added to the emissive G-buffer target (RT2) and re-added at compose.
    // emissiveColor * emissiveStrength; default 0 => zero-cost for existing content.
    //
    // P16.7 -- `emissiveStrength` IS A LUMINANCE, IN cd/m2, and always was: the product goes
    // straight into scene colour, and scene colour has been in cd/m2 since P16 put the lights in
    // lux. There is no conversion to apply and none should be invented -- the same argument as the
    // sun's illuminance.
    //
    // What DID change is that the numbers content was authored with are now absurdly small. A
    // strength of 4 is four cd/m2, which is a dim indicator LED in a dark room; against a scene lit
    // at thousands it does not exist. Real values: a lit screen 300-500, an LED panel ~1000, a
    // neon sign ~5000, an incandescent filament 10^6. RT2 is R11G11B10_FLOAT and holds them.
    float3 emissiveColor    = {0.f, 0.f, 0.f};
    float  emissiveStrength = 0.f;
    // W3: per-object wind sway strength (0 = rigid). Authored per object and written UNIFORMLY to
    // every slot in GBufferRenderable::ResolveMaterialSlots (submesh sync). Read by the gbuffer VS
    // (W4) via the b0 PerObject CB / InstancePerObject; the ocean-force analogue for foliage.
    float  windStrength     = 0.f;
    // Per-SLOT foliage weight (0 = woody trunk, 1 = leaves). Authored per asset via mesh.json
    // "windFoliage": [..] (one entry per material slot); defaults to the slot's alpha-mask flag.
    float  windFoliage      = 0.f;

    float3 EmissiveLinear() const
    {
        return float3(emissiveColor.x * emissiveStrength,
                      emissiveColor.y * emissiveStrength,
                      emissiveColor.z * emissiveStrength);
    }

    void SetUseAlbedo(bool b){ texFlags.x = b ? 1.f : 0.f; }
    void SetUseMR(bool b)    { texFlags.y = b ? 1.f : 0.f; }
    void SetUseNormal(bool b){ texFlags.z = b ? 1.f : 0.f; }
    void SetNormalStrength(float s){ texFlags.w = s; }
    void SetMultiplyMR(bool b){ mrMultiply = b ? 1.f : 0.f; }
};

// ---------------------
// Material asset data: textures + static features
// ---------------------
class MaterialData {
public:
    // Feature flags (can be passed as defines when building shader permutations)
    bool normalIsRG = true; // RG/BC5 vs RGB(A)
    ShadingModel shadingModel = ShadingModel::DefaultLit;
    MaterialSurfaceParams surfaceParams;
    bool mrLayoutGltf = false; // true => RAW glTF preview: MR is glTF-packed + factors multiply; emits
                               // MR_LAYOUT_GLTF. Imported assets stay false — the importer bakes both
                               // the channel order AND the factors into the DDS (H6).

    // glTF auto-material (A3): when built from a glTF material, these carry the imported per-object
    // defaults (seeded into GBufferRenderable::matParams_ at Init) plus fields consumed later:
    // alpha* + doubleSided by Part C (masked/two-sided foliage), emissive* by Part D.
    bool          fromGltf = false;
    MaterialParams gltfDefaultParams;      // baseColor tint, metalRough, texFlags from the glTF material

    // I0: material FILES (data/materials/<name>.json) can carry parameter defaults. Seeded into a
    // slot's MaterialParams at Init only when the level JSON didn't override that slot (explicit
    // per-object params win — see GBufferRenderable::Init).
    bool           hasPresetParams = false;
    MaterialParams presetParams;

    // I0: optional gbuffer shader override from the material file ("shader" key). Applied to the
    // slot's graphics PSO (and, via the _csm suffix convention, its shadow PSO). Empty = object's
    // shader. Auto-instancing is skipped for objects using an overriding material.
    std::wstring shaderOverride;
    bool          alphaMask = false;       // alphaMode == MASK (Part C)
    float         alphaCutoff = 0.5f;      // (Part C)
    bool          doubleSided = false;     // (Part C)
    float3        emissiveFactor = {0.0f, 0.0f, 0.0f}; // (Part D)

    // Glass (ShadingModel::ThinTranslucent): glTF KHR_materials_transmission's factor, times the
    // transmission map's R where there is one -- the share of light that goes THROUGH (tinted by the
    // base colour) instead of being reflected diffusely. Not the foliage's `transmissionStrength`
    // (surfaceParams), which is a leaf's back-lighting. ior: KHR_materials_ior, 1.5 = glTF's default.
    float         glassTransmission = 1.0f;
    float         glassIor = 1.5f;
    bool IsThinTranslucent() const { return shadingModel == ShadingModel::ThinTranslucent; }
    // Glass with no alpha test: no texel of it is opaque, so it takes no opaque path at all.
    bool IsGlassOnly() const { return IsThinTranslucent() && !alphaMask; }
    // A split glass slot's cutoff in the SHADOW passes is raised by this (shadow_depth_common.hlsli
    // kGlassShadowCutoff): its glass texels then cast a dithered share equal to their coverage.
    static constexpr float kGlassShadowCutoff = 1.0f;
    float ShadowAlphaCutoff() const { return alphaCutoff + (IsThinTranslucent() ? kGlassShadowCutoff : 0.0f); }

    // Texture ownership
    // The shaders declare albedo/MR/normal/emissive at consecutive FIXED registers, so the table is
    // always this wide -- see GatherGBufferSRVs for why a skipped slot was a bug.
    static constexpr size_t kGBufferSrvCount = 4;
    size_t GatherGBufferSRVs(D3D12_CPU_DESCRIPTOR_HANDLE* dst) const;
    // A6: the same slots as bindless heap indices (albedo, MR, normal, emissive), with the same
    // filler rule for the first three -- an absent map takes a sibling's index, and a material with
    // no textures at all points every slot at the heap's null SRV. The EMISSIVE index is also the
    // shaders' flag: kNoEmissiveMap = no map (the emissive is the flat colour), anything else = the
    // map multiplies it -- in the table permutations it sits at the fourth table slot. Read fresh
    // every frame: a streaming swap or a fade clamp renames a texture's slot.
    static constexpr std::uint32_t kNoEmissiveMap = 0xFFFFFFFFu; // gbuffer_common.hlsli kNoEmissiveMap
    void GatherGBufferIndices(std::uint32_t dst[4]) const;

    bool      hasAlbedo = false;
    bool      hasMR     = false;
    bool      hasNormal = false;
    bool      hasEmissive = false;
    bool      hasTransmission = false;
    Texture2D albedo; // sRGB
    Texture2D mr;     // UNORM: R=metal, G=rough
    Texture2D normal; // UNORM: RG (or RGB if normalIsRG=false)
    Texture2D emissive; // sRGB: multiplies emissiveColor * emissiveStrength (a candle's flame mask)
    Texture2D transmission; // UNORM R: multiplies glassTransmission (read by the glass pass only)

    // The files this material was BUILT from, so a re-import can find the built materials it
    // invalidated (see MaterialDataManager::EvictCachedForTextures). Recorded even when the load
    // FAILED: a material naming a texture that did not exist yet is exactly the one an import has
    // to rebuild. These are the REQUESTED paths -- a preset may name x.png while the loader picks
    // up the imported x.dds sibling, and the eviction test knows to check both.
    std::wstring albedoSourcePath;
    std::wstring mrSourcePath;
    std::wstring normalSourcePath;
    std::wstring emissiveSourcePath;
    std::wstring transmissionSourcePath;
    // True when any recorded source path satisfies `pred`.
    bool UsesTexture(const std::function<bool(const std::wstring&)>& pred) const;

    // Loading helpers
    bool LoadAlbedo(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);
    bool LoadMR    (Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);
    bool LoadNormal(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);
    bool LoadEmissive(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                      std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);
    bool LoadTransmission(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                          std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

    // Configure sampling and shading-model defines for the GBuffer variant.
    void ConfigureDefinesForGBuffer(Material::GraphicsDesc& gd) const;

    // Assemble the SRV table and sampler for the standard GBuffer pass:
    // TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3)) + TABLE(SAMPLER(s0))
    // A6: `bindless` = the material's PSO is a GBUFFER_BINDLESS permutation (Material::IsBindless):
    // no SRV table exists, the textures are reached by the indices StageGBufferSurfaceParams
    // writes; only the sampler is staged. A textureless material binds the renderer's null texture
    // (table) or the heap's null slot (bindless) and draws on its tint.
    void StageGBufferBindings(Renderer* r, RenderContext& ctx,
                              UINT srvTableRegister = 0, UINT samplerTableRegister = 0,
                              bool bindless = false);

    // Stage the material-static SurfaceParams root CBV. Cached once per material per frame.
    void StageGBufferSurfaceParams(Renderer* r, RenderContext& ctx, UINT cbvRegister = 2);
    static void StageNeutralGBufferSurfaceParams(Renderer* r, RenderContext& ctx, UINT cbvRegister = 2);

    // For the instanced path (t0 = instances), append t1..t4 starting at the provided offset.
    // Returns the number of descriptors appended.
    size_t AppendGBufferSRVs(std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kGBufferSrvCount>& dst, size_t offset = 0) const;
    size_t AppendGBufferSRVs(D3D12_CPU_DESCRIPTOR_HANDLE* dst, size_t& inoutCount) const;

private:
    static void SetStreamable(Texture2D::CreateDesc& d); // A3: tail-only load when the manager is on
    struct SrvCache {
        // The descriptor allocator is reset every time a frame-resource slot is
        // reused. Cache against the monotonically increasing frame number, not
        // the 0..kFrameCount-1 swap-chain slot, so a recycled descriptor range
        // is never rebound as this material's table.
        uint64_t frameNumber = UINT64_MAX;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    } gbufferSrvCache_;
    struct SurfaceCbCache {
        uint64_t frameNumber = UINT64_MAX;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    } surfaceCbCache_;
    std::mutex cacheMtx_;
};
