#pragma once
#include <array>
#include <cstdint>
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
    TwoSidedFoliage = 1
};
static_assert(static_cast<uint8_t>(ShadingModel::DefaultLit) == 0);
static_assert(static_cast<uint8_t>(ShadingModel::TwoSidedFoliage) == 1);
static_assert(static_cast<uint8_t>(ShadingModel::TwoSidedFoliage) < 16);

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
    std::string   emissiveTexPath;         // resolved path, empty if none (Part D loads it)

    // Texture ownership
    bool      hasAlbedo = false;
    bool      hasMR     = false;
    bool      hasNormal = false;
    Texture2D albedo; // sRGB
    Texture2D mr;     // UNORM: R=metal, G=rough
    Texture2D normal; // UNORM: RG (or RGB if normalIsRG=false)

    // Loading helpers
    bool LoadAlbedo(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);
    bool LoadMR    (Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);
    bool LoadNormal(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

    // Configure sampling and shading-model defines for the GBuffer variant.
    void ConfigureDefinesForGBuffer(Material::GraphicsDesc& gd) const;

    // Assemble the SRV table and sampler for the standard GBuffer pass:
    // TABLE(SRV(t0) SRV(t1) SRV(t2)) + TABLE(SAMPLER(s0))
    void StageGBufferBindings(Renderer* r, RenderContext& ctx,
                              UINT srvTableRegister = 0, UINT samplerTableRegister = 0);

    // Stage the material-static SurfaceParams root CBV. Cached once per material per frame.
    void StageGBufferSurfaceParams(Renderer* r, RenderContext& ctx, UINT cbvRegister = 2);
    static void StageNeutralGBufferSurfaceParams(Renderer* r, RenderContext& ctx, UINT cbvRegister = 2);

    // For the instanced path (t0 = instances), append t1..t3 starting at the provided offset.
    // Returns the number of descriptors appended.
    size_t AppendGBufferSRVs(std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3>& dst, size_t offset = 0) const;
    size_t AppendGBufferSRVs(D3D12_CPU_DESCRIPTOR_HANDLE* dst, size_t& inoutCount) const;

private:
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
