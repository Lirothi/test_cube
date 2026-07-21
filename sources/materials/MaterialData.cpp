#include "materials/MaterialData.h"
#include "rendering/core/Renderer.h"
#include "rendering/descriptors/SamplerManager.h"
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
    return false;
}

bool MaterialData::LoadAlbedo(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                              std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
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
    Texture2D::CreateDesc d{};
    d.path  = path;
    d.usage = Texture2D::Usage::MetalRough;
    if (mr.CreateFromFile(r, upload, d, keepAlive)) { hasMR = true; return true; }
    return false;
}

bool MaterialData::LoadNormal(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                              std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
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
    eraseKey("USE_TBN");
    eraseKey("MR_LAYOUT_GLTF");
    eraseKey("SHADING_MODEL_ID");

    defs.emplace_back("NORMALMAP_IS_RG", normalIsRG   ? "1" : "0");
    defs.emplace_back("USE_TBN",         useTBN       ? "1" : "0");
    defs.emplace_back("MR_LAYOUT_GLTF",  mrLayoutGltf ? "1" : "0");
    defs.emplace_back("SHADING_MODEL_ID", std::to_string(static_cast<uint32_t>(shadingModel)));
}

size_t MaterialData::AppendGBufferSRVs(std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3>& dst, size_t offset) const
{
    size_t appended = 0;
    if (hasAlbedo) { dst[offset + appended++] = albedo.GetSRVCPU(); }
    if (hasMR)     { dst[offset + appended++] = mr.GetSRVCPU(); }
    if (hasNormal) { dst[offset + appended++] = normal.GetSRVCPU(); }
    return appended;
}

size_t MaterialData::AppendGBufferSRVs(D3D12_CPU_DESCRIPTOR_HANDLE* dst, size_t& inoutCount) const
{
    size_t appended = 0;
    if (hasAlbedo) { dst[inoutCount++] = albedo.GetSRVCPU(); ++appended; }
    if (hasMR)     { dst[inoutCount++] = mr.GetSRVCPU();     ++appended; }
    if (hasNormal) { dst[inoutCount++] = normal.GetSRVCPU(); ++appended; }
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
        size_t count = 0;
        if (hasAlbedo) { srvs[count] = albedo.GetSRVCPU(); ++count; }
        if (hasMR)     { srvs[count] = mr.GetSRVCPU(); ++count; }
        if (hasNormal) { srvs[count] = normal.GetSRVCPU(); ++count; }
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
