#include "Material.h"

#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "Helpers.h"
#include "RootSignatureLayout.h"
#include "RootSignatureParser.h"
#include "TaskSystem.h"

// ----------------------------------------
// Общие утилиты для обоих Create* методов
// ----------------------------------------
static std::string ReadFileToString(const std::wstring& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open shader file!");
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static void BuildRootFromLayout(
    ID3D12Device* device,
    const RootSignatureLayout& inLayout,
    D3D12_ROOT_SIGNATURE_FLAGS rsFlags,
    ComPtr<ID3D12RootSignature>& outRS,
    std::vector<Material::RootParameterInfo>& outParams)
{
    RootSignatureLayout layout = inLayout;
    // flags из аргумента имеют приоритет
    layout.flags = rsFlags;

    // Сформировать RootParameterInfo для Bind()
    outParams.clear();
    outParams.reserve(layout.params.size());
    for (size_t i = 0; i < layout.params.size(); ++i) {
        const auto& p = layout.params[i];
        Material::RootParameterInfo info{};
        info.rootIndex = (UINT)i;
        switch (p.type) {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            info.type = Material::RootParameterInfo::Constants;
            info.constantsCount = p.num32BitValues;
            info.bindingRegister = p.shaderRegister; // bN
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
            info.type = Material::RootParameterInfo::CBV;
            info.bindingRegister = p.shaderRegister; // bN
            break;
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
            info.type = Material::RootParameterInfo::SRV;
            info.bindingRegister = p.shaderRegister; // tN
            break;
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            info.type = Material::RootParameterInfo::UAV;
            info.bindingRegister = p.shaderRegister; // uN
            break;
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            // Ключ таблицы — base регистр первой полосы (см. наш layout.AddTable)
            info.type = p.hasSamplerRanges ? Material::RootParameterInfo::TableSampler : Material::RootParameterInfo::Table;
            info.bindingRegister = p.ranges.empty() ? 0u : p.ranges.front().BaseShaderRegister;
            break;
        }
        outParams.push_back(info);
    }

    // Сериализовать и создать RS
    D3D12_ROOT_SIGNATURE_DESC desc = MakeRootSignatureDesc(layout);
    ComPtr<ID3DBlob> sigBlob, errBlob;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob));
    ThrowIfFailed(device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&outRS)));
}

struct IncludeCapture : public ID3DInclude {
    std::wstring rootDir;
    std::vector<std::wstring>& outFiles;
    std::unordered_map<const void*, std::wstring> dirOfData;

    IncludeCapture(const std::wstring& base, std::vector<std::wstring>& out)
        : rootDir(base), outFiles(out) {
    }

    static std::wstring Widen(const char* s) {
        std::string a = (s ? s : "");
        return std::wstring(a.begin(), a.end());
    }

    HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID pParentData,
        LPCVOID* ppData, UINT* pBytes) override
    {
        std::filesystem::path base = rootDir;
        if (pParentData) {
            auto it = dirOfData.find(pParentData);
            if (it != dirOfData.end()) {
                base = it->second;
            }
        }

        std::filesystem::path req = Widen(pFileName);
        std::filesystem::path full = (req.is_absolute() ? req : (base / req)).lexically_normal();

        std::ifstream f(full, std::ios::binary);
        if (!f) {
            return E_FAIL;
        }

        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        char* mem = (char*)::malloc(bytes.size());
        if (!mem) {
            return E_OUTOFMEMORY;
        }
        std::memcpy(mem, bytes.data(), bytes.size());

        *ppData = mem;
        *pBytes = (UINT)bytes.size();
        outFiles.push_back(full.wstring());
        dirOfData[mem] = full.parent_path().wstring();
        return S_OK;
    }

    HRESULT Close(LPCVOID pData) override
    {
        dirOfData.erase(pData);
        ::free((void*)pData);
        return S_OK;
    }
};

void Material::CreateGraphics(Renderer* r, const GraphicsDesc& gd)
{
    isCompute_ = false;
    cachedGfxDesc_ = gd;

    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    std::vector<RootParameterInfo> params;
    std::vector<std::wstring> inc;

    if (!BuildGraphicsPSO(r, gd, rs, pso, params, inc)) {
        OutputDebugStringA("[Material] CreateGraphics failed\n");
        return;
    }

    rootSignature_ = rs;
    pipelineState_ = pso;
    rootParams_ = std::move(params);

    {
        std::lock_guard<std::mutex> lk(watchMtx_);
        watchedFiles_ = std::move(inc);
    }
    RefreshWatchTimes_();
}

void Material::CreateCompute(Renderer* r, const std::wstring& csFile)
{
    isCompute_ = true;
    shaderFileCS_ = csFile;
    csEntry_ = "main";

    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    std::vector<RootParameterInfo> params;
    std::vector<std::wstring> inc;

    if (!BuildComputePSO(r, csFile, csEntry_.c_str(), rs, pso, params, inc)) {
        OutputDebugStringA("[Material] CreateCompute failed\n");
        return;
    }

    rootSignature_ = rs;
    pipelineState_ = pso;
    rootParams_ = std::move(params);

    {
        std::lock_guard<std::mutex> lk(watchMtx_);
        watchedFiles_ = std::move(inc);
    }
    RefreshWatchTimes_();
}

bool Material::HotReloadIfPending(Renderer* r, uint64_t frameNumber, uint64_t keepAliveFrames)
{
    if (!pendingReload_.load(std::memory_order_acquire)) {
        return false;
    }

    ComPtr<ID3D12RootSignature> newRS;
    ComPtr<ID3D12PipelineState> newPSO;
    std::vector<RootParameterInfo> newParams;
    std::vector<std::wstring> inc;

    bool ok = false;
    if (isCompute_) {
        ok = BuildComputePSO(r, shaderFileCS_, csEntry_.c_str(), newRS, newPSO, newParams, inc);
    }
    else {
        ok = BuildGraphicsPSO(r, cachedGfxDesc_, newRS, newPSO, newParams, inc);
    }

    if (!ok) {
        // оставим pending=true — попробуем на следующем тике
        return false;
    }

    // отложенная утилизация
    retired_.push_back({ pipelineState_, rootSignature_, frameNumber });

    pipelineState_ = newPSO;
    rootSignature_ = newRS;
    rootParams_ = std::move(newParams);

    {
        std::lock_guard<std::mutex> lk(watchMtx_);
        watchedFiles_ = std::move(inc);
    }
    RefreshWatchTimes_();

    pendingReload_.store(false, std::memory_order_release);
    return true;
}

void Material::CollectRetired(uint64_t frameNumber, uint64_t keepAliveFrames)
{
    auto it = retired_.begin();
    while (it != retired_.end()) {
        if (frameNumber - it->retireFrame > keepAliveFrames) {
            it = retired_.erase(it);
        }
        else {
            ++it;
        }
    }
}

void Material::Bind(ID3D12GraphicsCommandList* cmdList, const RenderContext& ctx) const
{
    if (isCompute_) {
        cmdList->SetComputeRootSignature(rootSignature_.Get());
    }
    else {
        cmdList->SetGraphicsRootSignature(rootSignature_.Get());
    }

    cmdList->SetPipelineState(pipelineState_.Get());

    for (const auto& p : rootParams_) {
        uint32_t reg = p.bindingRegister;
        switch (p.type) {
        case RootParameterInfo::Constants:
        {
            auto it = ctx.constants.find(reg);
            if (it != ctx.constants.end() && !it->second.empty()) {
                if (isCompute_) {
                    cmdList->SetComputeRoot32BitConstants(p.rootIndex, (UINT)it->second.size(), it->second.data(), 0);
                }
                else {
                    cmdList->SetGraphicsRoot32BitConstants(p.rootIndex, (UINT)it->second.size(), it->second.data(), 0);
                }
            }
            break;
        }
        case RootParameterInfo::CBV:
        {
            auto it = ctx.cbv.find(reg);
            if (it != ctx.cbv.end()) {
                if (isCompute_) {
                    cmdList->SetComputeRootConstantBufferView(p.rootIndex, it->second);
                }
                else {
                    cmdList->SetGraphicsRootConstantBufferView(p.rootIndex, it->second);
                }
            }
            break;
        }
        case RootParameterInfo::SRV:
        {
            auto it = ctx.srv.find(reg);
            if (it != ctx.srv.end()) {
                if (isCompute_) {
                    cmdList->SetComputeRootShaderResourceView(p.rootIndex, it->second);
                }
                else {
                    cmdList->SetGraphicsRootShaderResourceView(p.rootIndex, it->second);
                }
            }
            break;
        }
        case RootParameterInfo::UAV:
        {
            auto it = ctx.uav.find(reg);
            if (it != ctx.uav.end()) {
                if (isCompute_) {
                    cmdList->SetComputeRootUnorderedAccessView(p.rootIndex, it->second);
                }
                else {
                    cmdList->SetGraphicsRootUnorderedAccessView(p.rootIndex, it->second);
                }
            }
            break;
        }
        case RootParameterInfo::Table:
        {
            auto it = ctx.table.find(reg);
            if (it != ctx.table.end()) {
                if (isCompute_) {
                    cmdList->SetComputeRootDescriptorTable(p.rootIndex, it->second);
                }
                else {
                    cmdList->SetGraphicsRootDescriptorTable(p.rootIndex, it->second);
                }
            }
            break;
        }
        case RootParameterInfo::TableSampler:
        {
            auto it = ctx.samplerTable.find(reg);
            if (it != ctx.samplerTable.end()) {
                if (isCompute_) { cmdList->SetComputeRootDescriptorTable(p.rootIndex, it->second); }
                else { cmdList->SetGraphicsRootDescriptorTable(p.rootIndex, it->second); }
            }
            break;
        }
        }
    }
}

// ===== Компиляция с include’ами =====
HRESULT Material::CompileWithIncludes(const std::wstring& file,
    const char* entry, const char* target,
    UINT flags,
    ComPtr<ID3DBlob>& outBlob,
    std::vector<std::wstring>& outIncludes)
{
    std::filesystem::path p = std::filesystem::path(file).lexically_normal();
    IncludeCapture inc(p.parent_path().wstring(), outIncludes);
    ComPtr<ID3DBlob> errs;

    HRESULT hr = D3DCompileFromFile(p.c_str(), nullptr, &inc, entry, target, flags, 0, &outBlob, &errs);
    if (FAILED(hr) && errs) {
        OutputDebugStringA((const char*)errs->GetBufferPointer());
    }
    return hr;
}

// ===== watch utils =====
void Material::RefreshWatchTimes_()
{
    std::lock_guard<std::mutex> lk(watchMtx_);
    watchedTimes_.resize(watchedFiles_.size());
    for (size_t i = 0; i < watchedFiles_.size(); ++i) {
        std::error_code ec{};
        watchedTimes_[i] = std::filesystem::last_write_time(watchedFiles_[i], ec);
    }
}

bool Material::FSProbeAndFlagPending()
{
    std::lock_guard<std::mutex> lk(watchMtx_);
    if (watchedFiles_.empty()) {
        return false;
    }

    bool changed = false;
    for (size_t i = 0; i < watchedFiles_.size(); ++i) {
        std::error_code ec{};
        auto t = std::filesystem::last_write_time(watchedFiles_[i], ec);
        if (!ec && i < watchedTimes_.size()) {
            if (t != watchedTimes_[i]) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        pendingReload_.store(true, std::memory_order_release);
        return true;
    }
    else {
        return false;
    }
}

// ===== Общий билдер: Graphics =====
bool Material::BuildGraphicsPSO(Renderer* r, const GraphicsDesc& gd,
    ComPtr<ID3D12RootSignature>& outRS,
    ComPtr<ID3D12PipelineState>& outPSO,
    std::vector<RootParameterInfo>& outParams,
    std::vector<std::wstring>& outIncludes)
{
    // 1) RS из исходника
    RootSignatureLayout layoutParsed;
    {
        std::string src = ReadFileToString(gd.shaderFile);
        ParseRootSignatureFromSource(src, layoutParsed);
    }

    // 2) компиляция
    UINT cf =
#ifdef _DEBUG
    (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
#else
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> vs5, ps5;
    std::vector<std::wstring> incVS, incPS;
    if (FAILED(CompileWithIncludes(gd.shaderFile, gd.vsEntry, "vs_5_0", cf, vs5, incVS))) {
        return false;
    }
    if (FAILED(CompileWithIncludes(gd.shaderFile, gd.psEntry, "ps_5_0", cf, ps5, incPS))) {
        return false;
    }

    // 3) RS
    BuildRootFromLayout(r->GetDevice(), layoutParsed, gd.rsFlags, outRS, outParams);

    // 4) PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = outRS.Get();

    if (gd.inputLayoutKey.empty()) {
        pso.InputLayout = { nullptr, 0 };
    }
    else {
        auto il = r->GetInputLayoutManager().Get(gd.inputLayoutKey);
        pso.InputLayout = { il.desc, il.count };
    }

    pso.VS = { vs5->GetBufferPointer(), vs5->GetBufferSize() };
    pso.PS = { ps5->GetBufferPointer(), ps5->GetBufferSize() };
    pso.RasterizerState = gd.raster;
    pso.BlendState = gd.blend;
    pso.DepthStencilState = gd.depth;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = gd.topologyType;
    pso.NumRenderTargets = gd.numRT;
    for (UINT i = 0; i < gd.numRT; ++i) {
        pso.RTVFormats[i] = (gd.numRT == 1 ? gd.rtvFormat : gd.rtvFormats[i]);
    }
    pso.DSVFormat = gd.dsvFormat;
    pso.SampleDesc.Count = gd.sampleCount;

    if (FAILED(r->GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&outPSO)))) {
        return false;
    }

    // 5) watch-лист
    outIncludes.clear();
    outIncludes.push_back(gd.shaderFile);
    outIncludes.insert(outIncludes.end(), incVS.begin(), incVS.end());
    outIncludes.insert(outIncludes.end(), incPS.begin(), incPS.end());
    std::sort(outIncludes.begin(), outIncludes.end());
    outIncludes.erase(std::unique(outIncludes.begin(), outIncludes.end()), outIncludes.end());

    return true;
}

// ===== Общий билдер: Compute =====
bool Material::BuildComputePSO(Renderer* r, const std::wstring& csFile, const char* csEntry,
    ComPtr<ID3D12RootSignature>& outRS,
    ComPtr<ID3D12PipelineState>& outPSO,
    std::vector<RootParameterInfo>& outParams,
    std::vector<std::wstring>& outIncludes)
{
    UINT cf =
#ifdef _DEBUG
    (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
#else
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> cs5;
    std::vector<std::wstring> incCS;
    if (FAILED(CompileWithIncludes(csFile, csEntry, "cs_5_0", cf, cs5, incCS))) {
        return false;
    }

    RootSignatureLayout layoutParsed;
    {
        std::string src = ReadFileToString(csFile);
        ParseRootSignatureFromSource(src, layoutParsed);
    }
    BuildRootFromLayout(r->GetDevice(), layoutParsed, D3D12_ROOT_SIGNATURE_FLAG_NONE, outRS, outParams);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = outRS.Get();
    pso.CS = { cs5->GetBufferPointer(), cs5->GetBufferSize() };
    if (FAILED(r->GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&outPSO)))) {
        return false;
    }

    outIncludes = incCS;
    outIncludes.push_back(csFile);
    std::sort(outIncludes.begin(), outIncludes.end());
    outIncludes.erase(std::unique(outIncludes.begin(), outIncludes.end()), outIncludes.end());

    return true;
}

bool MaterialManager::RequestFSProbeAsync()
{
    bool expected = false;
    if (!fsProbeInFlight_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false; // уже идёт
    }

    TaskSystem::Get().Submit([this]() {
        for (auto& kv : materials_) {
            auto& mat = kv.second;
            if (mat) {
                (void)mat->FSProbeAndFlagPending();
            }
        }
        fsProbeInFlight_.store(false, std::memory_order_release);
        });

    return true;
}

void MaterialManager::ApplyPendingHotReloads(Renderer* r, uint64_t frameNumber, uint64_t keepAliveFrames)
{
    for (auto& kv : materials_) {
        auto& mat = kv.second;
        if (mat) {
            if (mat->HotReloadIfPending(r, frameNumber, keepAliveFrames)) {
                // можно залогировать успех
            }
            mat->CollectRetired(frameNumber, keepAliveFrames);
        }
    }
}