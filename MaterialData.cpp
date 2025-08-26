#include "MaterialData.h"
#include "Renderer.h"
#include "SamplerManager.h"
#include <algorithm>

using Microsoft::WRL::ComPtr;

bool MaterialData::LoadAlbedo(Renderer* r, ID3D12GraphicsCommandList* upload, const std::wstring& path,
                              std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    Texture2D::CreateDesc d{};
    d.path  = path;
    d.usage = Texture2D::Usage::AlbedoSRGB;
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

    defs.emplace_back("NORMALMAP_IS_RG", normalIsRG ? "1" : "0");
    defs.emplace_back("USE_TBN",         useTBN     ? "1" : "0");
}

void MaterialData::AppendGBufferSRVs(std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>& dst) const
{
    dst.push_back(albedo.GetSRVCPU());
    dst.push_back(mr.GetSRVCPU());
    dst.push_back(normal.GetSRVCPU());
}

void MaterialData::StageGBufferBindings(Renderer* r, RenderContext& ctx,
                                        UINT srvTableRegister, UINT samplerTableRegister)
{
    const UINT fi = r->GetCurrentFrameIndex();
    std::lock_guard lck(cacheMtx_);
    if (gbufferSrvCache_.frame == fi && gbufferSrvCache_.gpu.ptr != 0) {
        ctx.table[srvTableRegister] = gbufferSrvCache_.gpu;
    }
    else {
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvs;
        srvs.reserve(3);
        if (hasAlbedo) srvs.push_back(albedo.GetSRVCPU());
        if (hasMR)     srvs.push_back(mr.GetSRVCPU());
        if (hasNormal) srvs.push_back(normal.GetSRVCPU());
        if (!srvs.empty()) {
            auto tbl = r->StageSrvUavTable(srvs);
            ctx.table[srvTableRegister] = tbl.gpu;
            gbufferSrvCache_.frame = fi;
            gbufferSrvCache_.gpu = tbl.gpu;
        }
    }
    auto aniso = SamplerManager::AnisoWrap(16);
    ctx.samplerTable[samplerTableRegister] = r->GetSamplerManager().Get(r, aniso);
}