#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>
#include <d3d12.h>

#include "materials/Texture2D.h"
#include "rendering/core/RenderContext.h"
#include "materials/Material.h"
#include "core/math/Math.h"

class Renderer;

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
    float4 texOffsScale = { 0.0f, 0.0f, 1.0f, 1.0f };
    // x=useAlbedo, y=useMR, z=useNormal, w=normalStrength (XY before reconstructing Z)
    float4 texFlags    = {1.f, 1.f, 1.f, 1.f};

    void SetUseAlbedo(bool b){ texFlags.x = b ? 1.f : 0.f; }
    void SetUseMR(bool b)    { texFlags.y = b ? 1.f : 0.f; }
    void SetUseNormal(bool b){ texFlags.z = b ? 1.f : 0.f; }
    void SetNormalStrength(float s){ texFlags.w = s; }
};

// ---------------------
// Material asset data: textures + static features
// ---------------------
class MaterialData {
public:
    // Feature flags (can be passed as defines when building shader permutations)
    bool normalIsRG = true; // RG/BC5 vs RGB(A)
    bool useTBN     = true; // TBN path (otherwise derivatives)
    bool mrLayoutGltf = false; // true => MR texture is glTF-packed (B=metal, G=rough); emits MR_LAYOUT_GLTF

    // glTF auto-material (A3): when built from a glTF material, these carry the imported per-object
    // defaults (seeded into GBufferRenderable::matParams_ at Init) plus fields consumed later:
    // alpha* + doubleSided by Part C (masked/two-sided foliage), emissive* by Part D.
    bool          fromGltf = false;
    MaterialParams gltfDefaultParams;      // baseColor tint, metalRough, texFlags from the glTF material
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

    // Configure defines for the GBuffer variant (NORMALMAP_IS_RG / USE_TBN)
    void ConfigureDefinesForGBuffer(Material::GraphicsDesc& gd) const;

    // Assemble the SRV table and sampler for the standard GBuffer pass:
    // TABLE(SRV(t0) SRV(t1) SRV(t2)) + TABLE(SAMPLER(s0))
    void StageGBufferBindings(Renderer* r, RenderContext& ctx,
                              UINT srvTableRegister = 0, UINT samplerTableRegister = 0);

    // For the instanced path (t0 = instances), append t1..t3 starting at the provided offset.
    // Returns the number of descriptors appended.
    size_t AppendGBufferSRVs(std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3>& dst, size_t offset = 0) const;
    size_t AppendGBufferSRVs(D3D12_CPU_DESCRIPTOR_HANDLE* dst, size_t& inoutCount) const;

private:
    struct SrvCache {
        UINT frame = UINT_MAX;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    } gbufferSrvCache_;
    std::mutex cacheMtx_;
};