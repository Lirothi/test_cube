#include "materials/MaterialData.h"
#include "rendering/core/Renderer.h"
#include "rendering/descriptors/SamplerManager.h"
#include "rendering/renderables/InstanceTypes.h"
#include <algorithm>
#include <array>
#include <string>

using Microsoft::WRL::ComPtr;

const char* ShadingModelToString(ShadingModel model)
{
    switch (model)
    {
    case ShadingModel::DefaultLit: return "defaultLit";
    case ShadingModel::TwoSidedFoliage: return "twoSidedFoliage";
    case ShadingModel::Terrain: return "terrain";
    default: return "defaultLit";
    }
}

bool TryParseShadingModel(std::string_view text, ShadingModel& outModel)
{
    if (text == "defaultLit")
    {
        outModel = ShadingModel::DefaultLit;
        return true;
    }
    if (text == "twoSidedFoliage")
    {
        outModel = ShadingModel::TwoSidedFoliage;
        return true;
    }
    if (text == "terrain")
    {
        outModel = ShadingModel::Terrain;
        return true;
    }
    return false;
}

bool MaterialData::UsesTexture(const std::function<bool(const std::wstring&)>& pred) const
{
    if (!pred) { return false; }
    return (!albedoSourcePath.empty() && pred(albedoSourcePath)) ||
           (!mrSourcePath.empty()     && pred(mrSourcePath))     ||
           (!normalSourcePath.empty() && pred(normalSourcePath));
}

bool MaterialData::LoadAlbedo(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                              std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    albedoSourcePath = path;
    Texture2D::CreateDesc d{};
    d.path  = path;
    d.usage = Texture2D::Usage::AlbedoSRGB;
    // Masked slots: preserve alpha-test coverage across the WIC-built mip chain (requires the
    // caller to set alphaMask/alphaCutoff BEFORE loading — see MaterialDataManager).
    d.alphaCoverageCutoff = (alphaMask && alphaCutoff >= 0.0f) ? alphaCutoff : -1.0f;
    if (albedo.CreateFromFile(r, upload, d, keepAlive)) { hasAlbedo = true; return true; }
    return false;
}

bool MaterialData::LoadMR(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                          std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    mrSourcePath = path;
    Texture2D::CreateDesc d{};
    d.path  = path;
    d.usage = Texture2D::Usage::MetalRough;
    if (mr.CreateFromFile(r, upload, d, keepAlive)) { hasMR = true; return true; }
    return false;
}

bool MaterialData::LoadNormal(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                              std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    normalSourcePath = path;
    Texture2D::CreateDesc d{};
    d.path       = path;
    d.usage      = Texture2D::Usage::NormalMap;
    d.normalIsRG = normalIsRG;
    if (normal.CreateFromFile(r, upload, d, keepAlive)) { hasNormal = true; return true; }
    return false;
}

void MaterialData::ConfigureDefinesForGBuffer(Material::GraphicsDesc& gd) const
{
    auto& defs = gd.defines;
    auto eraseKey = [&](const char* k){
        defs.erase(std::remove_if(defs.begin(), defs.end(),
                                  [&](const auto& p){ return p.first == k; }), defs.end());
    };
    eraseKey("NORMALMAP_IS_RG");
    eraseKey("MR_LAYOUT_GLTF");
    eraseKey("SHADING_MODEL_ID");

    defs.emplace_back("NORMALMAP_IS_RG", normalIsRG   ? "1" : "0");
    defs.emplace_back("MR_LAYOUT_GLTF",  mrLayoutGltf ? "1" : "0");
    defs.emplace_back("SHADING_MODEL_ID", std::to_string(static_cast<uint32_t>(shadingModel)));
}

// The material textures in the FIXED order the shaders declare them (albedo, MR, normal) --
// gbuffer.hlsl binds t0/t1/t2 and gbuffer_inst.hlsl t1/t2/t3, and a register is a POSITION.
//
// This used to skip absent textures and pack densely, which silently shifted every later texture
// down a slot. A material with an albedo and a normal but NO MR -- the tent presets are exactly
// that, `useMR: false` with a hand-set roughness -- therefore bound its NORMAL map as gMR and left
// gNormalMap pointing at whatever the descriptor ring happened to hold. `texFlags` saved the MR
// read (it multiplies the sampled value by 0), but the normal branch is gated ON, so the shader
// sampled an unwritten descriptor: a different stale texture every frame, which is precisely the
// flicker that was reported.
//
// Absent slots are filled with a sibling's SRV rather than a dedicated dummy texture. The content
// is irrelevant -- `texFlags` guarantees the shader discards it -- and what actually matters is
// that the descriptor is VALID and the positions are right. A material with no textures at all
// still returns 0 and stages nothing, which is what it did before and is safe for the same reason.
size_t MaterialData::GatherGBufferSRVs(D3D12_CPU_DESCRIPTOR_HANDLE* dst) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE filler{};
    if (hasAlbedo)      { filler = albedo.GetSRVCPU(); }
    else if (hasNormal) { filler = normal.GetSRVCPU(); }
    else if (hasMR)     { filler = mr.GetSRVCPU(); }
    if (filler.ptr == 0)
    {
        return 0;
    }
    dst[0] = hasAlbedo ? albedo.GetSRVCPU() : filler;
    dst[1] = hasMR     ? mr.GetSRVCPU()     : filler;
    dst[2] = hasNormal ? normal.GetSRVCPU() : filler;
    return kGBufferSrvCount;
}

size_t MaterialData::AppendGBufferSRVs(std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3>& dst, size_t offset) const
{
    return GatherGBufferSRVs(dst.data() + offset);
}

size_t MaterialData::AppendGBufferSRVs(D3D12_CPU_DESCRIPTOR_HANDLE* dst, size_t& inoutCount) const
{
    const size_t appended = GatherGBufferSRVs(dst + inoutCount);
    inoutCount += appended;
    return appended;
}

void MaterialData::StageGBufferBindings(Renderer* r, RenderContext& ctx,
                                        UINT srvTableRegister, UINT samplerTableRegister)
{
    const uint64_t frameNumber = r->GetTotalFrameNumber();
    std::lock_guard lck(cacheMtx_);
    if (gbufferSrvCache_.frameNumber == frameNumber && gbufferSrvCache_.gpu.ptr != 0) {
        ctx.srvTable[srvTableRegister] = gbufferSrvCache_.gpu;
    }
    else {
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> srvs{};
        const size_t count = GatherGBufferSRVs(srvs.data());
        if (count > 0) {
            auto tbl = r->StageSrvUavTable(srvs, count);
            ctx.srvTable[srvTableRegister] = tbl.gpu;
            gbufferSrvCache_.frameNumber = frameNumber;
            gbufferSrvCache_.gpu = tbl.gpu;
        }
    }
    // DLSS: bias material sampling toward sharper mips when rendering below display res (the
    // sampler cache keys by desc; the bias is quantized so mode changes reuse a few variants).
    D3D12_SAMPLER_DESC aniso = *SamplerManager::AnisoWrap(16);
    aniso.MipLODBias = r->GetDlssMipBias();
    ctx.samplerTable[samplerTableRegister] = r->GetSamplerManager()->Get(r, aniso);
}

void MaterialData::StageGBufferSurfaceParams(Renderer* r, RenderContext& ctx, UINT cbvRegister)
{
    if (!r || cbvRegister >= RenderContext::kMaxBindings) { return; }

    const uint64_t frameNumber = r->GetTotalFrameNumber();
    std::lock_guard lck(cacheMtx_);
    if (surfaceCbCache_.frameNumber != frameNumber || surfaceCbCache_.gpu == 0)
    {
        auto cb = r->GetFrameResource()->AllocDynamic(
            sizeof(render::MaterialSurfaceParamsGpu), render::kConstantBufferAlignment);
        auto* dst = static_cast<render::MaterialSurfaceParamsGpu*>(cb.cpu);
        dst->subsurfaceColor = DirectX::XMFLOAT3(
            surfaceParams.subsurfaceColor.x, surfaceParams.subsurfaceColor.y, surfaceParams.subsurfaceColor.z);
        dst->transmissionStrength = surfaceParams.transmissionStrength;
        dst->ambientOcclusion = surfaceParams.ambientOcclusion;
        dst->indirectSpecularScale = surfaceParams.indirectSpecularScale;
        dst->transmissionAlbedoPower = surfaceParams.transmissionAlbedoPower;
        dst->transmissionNormalWeight = surfaceParams.transmissionNormalWeight;
        dst->terrainTiling = DirectX::XMFLOAT4(
            surfaceParams.terrainZoneSize,
            surfaceParams.terrainRotationDegrees * (3.14159265358979323846f / 180.0f),
            surfaceParams.terrainScaleVariation,
            surfaceParams.terrainBlend);
        dst->terrainEdgeParams = DirectX::XMFLOAT4(
            surfaceParams.terrainEdgeBreakup,
            surfaceParams.terrainEdgeDetail,
            0.0f,
            0.0f);
        surfaceCbCache_.frameNumber = frameNumber;
        surfaceCbCache_.gpu = cb.gpu;
    }
    ctx.cbv[cbvRegister] = surfaceCbCache_.gpu;
}

void MaterialData::StageNeutralGBufferSurfaceParams(Renderer* r, RenderContext& ctx, UINT cbvRegister)
{
    static MaterialData neutral;
    neutral.StageGBufferSurfaceParams(r, ctx, cbvRegister);
}
