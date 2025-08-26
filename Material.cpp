#include "Material.h"

#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

#include <d3d12shader.h>    // ID3D12ShaderReflection
#include <d3dcompiler.h>    // D3DReflect (DXBC)
#include <dxcapi.h>         // DXC reflection (DXIL)

// Опционально: если доступен заголовок с FourCC для DXIL
#if __has_include(<dxc/dxilcontainer.h>)
#include <dxc/dxilcontainer.h> // hlsl::DFCC_DXIL
#endif

#pragma comment(lib, "d3dcompiler.lib") // D3DReflect
#pragma comment(lib, "dxcompiler.lib")

#include "Helpers.h"
#include "RootSignatureLayout.h"
#include "RootSignatureParser.h"
#include "TaskSystem.h"

using Microsoft::WRL::ComPtr;

// ----------------------------------------
// Утилиты
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
    layout.flags = rsFlags;

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
            info.type = p.hasSamplerRanges ? Material::RootParameterInfo::TableSampler : Material::RootParameterInfo::Table;
            info.bindingRegister = p.ranges.empty() ? 0u : p.ranges.front().BaseShaderRegister;
            break;
        }
        outParams.push_back(info);
    }

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
        if (!f) { return E_FAIL; }

        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        char* mem = (char*)::malloc(bytes.size());
        if (!mem) { return E_OUTOFMEMORY; }
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

// ---- macros → D3D_SHADER_MACRO ----
static void BuildMacros(const Material::DefineList& defines,
    std::vector<D3D_SHADER_MACRO>& outMacros,
    std::vector<std::string>& storage) // для владения char*
{
    outMacros.clear();
    storage.clear();
    storage.reserve(defines.size() * 2);

    for (const auto& kv : defines) {
        storage.push_back(kv.first);
        storage.push_back(kv.second);
        D3D_SHADER_MACRO m{};
        m.Name = storage[storage.size() - 2].c_str();
        m.Definition = storage[storage.size() - 1].c_str();
        outMacros.push_back(m);
    }
    outMacros.push_back({ nullptr, nullptr }); // terminator
}

// ===== компиляция =====
HRESULT Material::CompileWithIncludes(const std::wstring& file,
    const char* entry, const char* target, UINT flags,
    const DefineList& defines,
    ComPtr<ID3DBlob>& outBlob,
    std::vector<std::wstring>& outIncludes)
{
    std::filesystem::path p = std::filesystem::path(file).lexically_normal();
    IncludeCapture inc(p.parent_path().wstring(), outIncludes);
    ComPtr<ID3DBlob> errs;

    std::vector<D3D_SHADER_MACRO> macros;
    std::vector<std::string>      macroStorage;
    BuildMacros(defines, macros, macroStorage);

    HRESULT hr = D3DCompileFromFile(p.c_str(), macros.empty() ? nullptr : macros.data(),
        &inc, entry, target, flags, 0, &outBlob, &errs);
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
    if (watchedFiles_.empty()) { return false; }

    bool changed = false;
    for (size_t i = 0; i < watchedFiles_.size(); ++i) {
        std::error_code ec{};
        auto t = std::filesystem::last_write_time(watchedFiles_[i], ec);
        if (!ec && i < watchedTimes_.size()) {
            if (t != watchedTimes_[i]) { changed = true; break; }
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

// ===== public API =====
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

void Material::CreateCompute(Renderer* r, const ComputeDesc& cd)
{
    isCompute_ = true;
    cachedCmpDesc_ = cd;

    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    std::vector<RootParameterInfo> params;
    std::vector<std::wstring> inc;

    if (!BuildComputePSO(r, cd, rs, pso, params, inc)) {
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
        ok = BuildComputePSO(r, cachedCmpDesc_, newRS, newPSO, newParams, inc);
    }
    else {
        ok = BuildGraphicsPSO(r, cachedGfxDesc_, newRS, newPSO, newParams, inc);
    }

    if (!ok) {
        return false; // оставим pending=true — попробуем на следующем тике
    }

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
    if (isCompute_) { cmdList->SetComputeRootSignature(rootSignature_.Get()); }
    else { cmdList->SetGraphicsRootSignature(rootSignature_.Get()); }

    cmdList->SetPipelineState(pipelineState_.Get());

    for (const auto& p : rootParams_) {
        uint32_t reg = p.bindingRegister;
        switch (p.type) {
        case RootParameterInfo::Constants:
        {
            auto it = ctx.constants.find(reg);
            if (it != ctx.constants.end() && !it->second.empty()) {
                if (isCompute_) { cmdList->SetComputeRoot32BitConstants(p.rootIndex, (UINT)it->second.size(), it->second.data(), 0); }
                else { cmdList->SetGraphicsRoot32BitConstants(p.rootIndex, (UINT)it->second.size(), it->second.data(), 0); }
            }
            break;
        }
        case RootParameterInfo::CBV:
        {
            auto it = ctx.cbv.find(reg);
            if (it != ctx.cbv.end()) {
                if (isCompute_) { cmdList->SetComputeRootConstantBufferView(p.rootIndex, it->second); }
                else { cmdList->SetGraphicsRootConstantBufferView(p.rootIndex, it->second); }
            }
            break;
        }
        case RootParameterInfo::SRV:
        {
            auto it = ctx.srv.find(reg);
            if (it != ctx.srv.end()) {
                if (isCompute_) { cmdList->SetComputeRootShaderResourceView(p.rootIndex, it->second); }
                else { cmdList->SetGraphicsRootShaderResourceView(p.rootIndex, it->second); }
            }
            break;
        }
        case RootParameterInfo::UAV:
        {
            auto it = ctx.uav.find(reg);
            if (it != ctx.uav.end()) {
                if (isCompute_) { cmdList->SetComputeRootUnorderedAccessView(p.rootIndex, it->second); }
                else { cmdList->SetGraphicsRootUnorderedAccessView(p.rootIndex, it->second); }
            }
            break;
        }
        case RootParameterInfo::Table:
        {
            auto it = ctx.table.find(reg);
            if (it != ctx.table.end()) {
                if (isCompute_) { cmdList->SetComputeRootDescriptorTable(p.rootIndex, it->second); }
                else { cmdList->SetGraphicsRootDescriptorTable(p.rootIndex, it->second); }
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

// ===== Общий билдер: Graphics =====
bool Material::BuildGraphicsPSO(Renderer* r, const GraphicsDesc& gd,
    ComPtr<ID3D12RootSignature>& outRS,
    ComPtr<ID3D12PipelineState>& outPSO,
    std::vector<RootParameterInfo>& outParams,
    std::vector<std::wstring>& outIncludes)
{
    RootSignatureLayout layoutParsed;
    {
        std::string src = ReadFileToString(gd.shaderFile);
        ParseRootSignatureFromSource(src, layoutParsed);
    }

    UINT cf =
#ifdef _DEBUG
    (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
#else
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> vs5, ps5;
    std::vector<std::wstring> incVS, incPS;
    if (FAILED(CompileWithIncludes(gd.shaderFile, gd.vsEntry, "vs_5_0", cf, gd.defines, vs5, incVS))) {
        return false;
    }
    if (FAILED(CompileWithIncludes(gd.shaderFile, gd.psEntry, "ps_5_0", cf, gd.defines, ps5, incPS))) {
        return false;
    }

    cbInfos_.clear();
    ReflectShaderBlob(vs5.Get(), cbInfos_);
    ReflectShaderBlob(ps5.Get(), cbInfos_);

    BuildRootFromLayout(r->GetDevice(), layoutParsed, gd.rsFlags, outRS, outParams);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = outRS.Get();

    if (gd.inputLayoutKey.empty()) {
        pso.InputLayout = { nullptr, 0 };
    }
    else {
        auto il = r->GetInputLayoutManager()->Get(gd.inputLayoutKey);
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

    outIncludes.clear();
    outIncludes.push_back(gd.shaderFile);
    outIncludes.insert(outIncludes.end(), incVS.begin(), incVS.end());
    outIncludes.insert(outIncludes.end(), incPS.begin(), incPS.end());
    std::sort(outIncludes.begin(), outIncludes.end());
    outIncludes.erase(std::unique(outIncludes.begin(), outIncludes.end()), outIncludes.end());

    return true;
}

// ===== Общий билдер: Compute =====
bool Material::BuildComputePSO(Renderer* r, const ComputeDesc& cd,
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
    if (FAILED(CompileWithIncludes(cd.shaderFile, cd.csEntry, "cs_5_0", cf, cd.defines, cs5, incCS))) {
        return false;
    }

    cbInfos_.clear();
    ReflectShaderBlob(cs5.Get(), cbInfos_);

    RootSignatureLayout layoutParsed;
    {
        std::string src = ReadFileToString(cd.shaderFile);
        ParseRootSignatureFromSource(src, layoutParsed);
    }
    BuildRootFromLayout(r->GetDevice(), layoutParsed, cd.rsFlags, outRS, outParams);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = outRS.Get();
    pso.CS = { cs5->GetBufferPointer(), cs5->GetBufferSize() };
    if (FAILED(r->GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&outPSO)))) {
        return false;
    }

    outIncludes = incCS;
    outIncludes.push_back(cd.shaderFile);
    std::sort(outIncludes.begin(), outIncludes.end());
    outIncludes.erase(std::unique(outIncludes.begin(), outIncludes.end()), outIncludes.end());

    return true;
}

// ====== Manager ======
static std::wstring JoinDefines(const Material::DefineList& defs) {
    std::vector<std::string> s;
    s.reserve(defs.size());
    for (auto& kv : defs) {
        s.push_back(kv.first + "=" + kv.second);
    }
    std::sort(s.begin(), s.end());
    std::wstring out;
    for (auto& e : s) {
        out += std::wstring(e.begin(), e.end());
        out += L";";
    }
    return out;
}

std::wstring MaterialManager::BuildKey(const Material::GraphicsDesc& gd)
{
    std::wstring fmts = L"";
    for (UINT i = 0; i < gd.numRT; ++i) {
        fmts += std::to_wstring((int)(gd.numRT == 1 ? gd.rtvFormat : gd.rtvFormats[i])) + L",";
    }
    std::wstring key = L"G2|" + gd.shaderFile + L"|" +
        std::wstring(gd.inputLayoutKey.begin(), gd.inputLayoutKey.end()) + L"|" +
        std::to_wstring((int)gd.topologyType) + L"|" +
        fmts + L"|" + std::to_wstring((int)gd.dsvFormat) + L"|" +
        JoinDefines(gd.defines);
    return key;
}

std::wstring MaterialManager::BuildKey(const Material::ComputeDesc& cd)
{
    std::wstring key = L"C2|" + cd.shaderFile + L"|" + std::wstring(cd.csEntry, cd.csEntry + strlen(cd.csEntry)) + L"|" + JoinDefines(cd.defines);
    return key;
}

std::shared_ptr<Material> MaterialManager::GetOrCreateGraphics(Renderer* r, const Material::GraphicsDesc& gd)
{
    std::wstring key = BuildKey(gd);
    auto it = materials_.find(key);
    if (it != materials_.end()) { return it->second; }
    auto m = std::make_shared<Material>();
    m->CreateGraphics(r, gd);
    materials_[key] = m;
    return m;
}

std::shared_ptr<Material> MaterialManager::GetOrCreateCompute(Renderer* r, const Material::ComputeDesc& cd)
{
    std::wstring key = BuildKey(cd);
    auto it = materials_.find(key);
    if (it != materials_.end()) { return it->second; }
    auto m = std::make_shared<Material>();
    m->CreateCompute(r, cd);
    materials_[key] = m;
    return m;
}

bool MaterialManager::RequestFSProbeAsync()
{
    bool expected = false;
    if (!fsProbeInFlight_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
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
                // лог: пересобрали
            }
            mat->CollectRetired(frameNumber, keepAliveFrames);
        }
    }
}

const Material::CBufferInfo* Material::GetCBInfo(UINT bRegister) const {
    auto it = cbInfos_.find(bRegister);
    return (it == cbInfos_.end()) ? nullptr : &it->second;
}
bool Material::GetCBFieldOffset(UINT bRegister, const std::string& name, UINT& outOffset, UINT& outSize) const {
    auto* cb = GetCBInfo(bRegister);
    if (!cb) return false;
    auto it = cb->fieldsByName.find(name);
    if (it == cb->fieldsByName.end()) return false;
    outOffset = it->second.offset;
    outSize = it->second.size;
    return true;
}

void Material::ProcessReflection(ID3D12ShaderReflection* refl,
    std::unordered_map<UINT, CBufferInfo>& io)
{
    if (!refl) return;

    D3D12_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd))) return;

    // cbuffer name -> bRegister (bN)
    std::unordered_map<std::string, UINT> bindOfCB;
    for (UINT r = 0; r < sd.BoundResources; ++r) {
        D3D12_SHADER_INPUT_BIND_DESC bd{};
        if (SUCCEEDED(refl->GetResourceBindingDesc(r, &bd))) {
            if (bd.Type == D3D_SIT_CBUFFER) {
                bindOfCB[bd.Name ? bd.Name : ""] = bd.BindPoint; // bN
            }
        }
    }

    for (UINT i = 0; i < sd.ConstantBuffers; ++i) {
        ID3D12ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
        D3D12_SHADER_BUFFER_DESC cbd{};
        if (FAILED(cb->GetDesc(&cbd))) continue;

        std::string cbName = cbd.Name ? cbd.Name : "";
        auto itBind = bindOfCB.find(cbName);
        if (itBind == bindOfCB.end()) continue; // пропускаем $Globals и пр.

        const UINT bReg = itBind->second;
        auto& dst = io[bReg];
        dst.bindRegister = bReg;
        dst.sizeBytes = std::max<UINT>(dst.sizeBytes, cbd.Size);

        for (UINT v = 0; v < cbd.Variables; ++v) {
            ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D12_SHADER_VARIABLE_DESC vd{};
            if (FAILED(var->GetDesc(&vd))) continue;

            CBufferField f{};
            f.name = vd.Name ? vd.Name : "";
            f.offset = vd.StartOffset;
            f.size = vd.Size;

            dst.fieldsByName[f.name] = f; // объединяем поля с других стадий, offset должен совпадать
        }
    }
}

#define DXIL_FOURCC(ch0, ch1, ch2, ch3)                                        \
  ((uint32_t)(uint8_t)(ch0) | (uint32_t)(uint8_t)(ch1) << 8 |                  \
   (uint32_t)(uint8_t)(ch2) << 16 | (uint32_t)(uint8_t)(ch3) << 24)

void Material::ReflectShaderBlob(ID3DBlob* blob,
    std::unordered_map<UINT, CBufferInfo>& io)
{
    if (!blob) return;

    // ===== Попытка №1: DXIL через DXC =====
    {
        Microsoft::WRL::ComPtr<IDxcContainerReflection> crefl;
        if (SUCCEEDED(DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&crefl)))) {
            if (SUCCEEDED(crefl->Load(reinterpret_cast<IDxcBlob*>(blob)))) {

                // FourCC DXIL (берём из заголовка, либо формируем сами)
                UINT32 fourccDXIL = DXIL_FOURCC('D', 'X', 'I', 'L');
//#if __has_include(<dxc/dxilcontainer.h>)
//                    hlsl::DFCC_DXIL;
//#else
//                    // MAKEFOURCC('D','X','I','L')
//                    ((UINT32)'D')
//                    | ((UINT32)'X' << 8)
//                    | ((UINT32)'I' << 16)
//                    | ((UINT32)'L' << 24);
//#endif

                UINT partIndex = 0;
                if (SUCCEEDED(crefl->FindFirstPartKind(fourccDXIL, &partIndex))) {
                    Microsoft::WRL::ComPtr<IDxcBlob> part;
                    if (SUCCEEDED(crefl->GetPartContent(partIndex, &part))) {
                        Microsoft::WRL::ComPtr<IDxcUtils> utils;
                        if (SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
                            DxcBuffer buf{};
                            buf.Ptr = part->GetBufferPointer();
                            buf.Size = part->GetBufferSize();
                            buf.Encoding = 0u; // бинарь: кодировка не важна, 0 (DXC_CP_ACP) ок

                            Microsoft::WRL::ComPtr<ID3D12ShaderReflection> refl;
                            if (SUCCEEDED(utils->CreateReflection(&buf, IID_PPV_ARGS(&refl)))) {
                                ProcessReflection(refl.Get(), io);
                                return; // успех: DXIL
                            }
                        }
                    }
                }
            }
        }
    }

    // ===== Попытка №2: DXBC через D3DReflect =====
    {
        Microsoft::WRL::ComPtr<ID3D12ShaderReflection> refl;
        if (SUCCEEDED(D3DReflect(blob->GetBufferPointer(),
            blob->GetBufferSize(),
            __uuidof(ID3D12ShaderReflection),
            (void**)refl.GetAddressOf())))
        {
            ProcessReflection(refl.Get(), io);
            return; // успех: DXBC
        }
    }

    // Если сюда дошли — рефлексия не получилась (оставляем io как есть)
}