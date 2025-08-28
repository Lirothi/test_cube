#include "Material.h"

#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

#include <d3d12shader.h>    // ID3D12ShaderReflection
#include <d3dcompiler.h>    // D3DReflect (DXBC)
#include <dxcapi.h>         // DXC reflection (DXIL)

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

static D3D_SHADER_MODEL QueryMaxShaderModel(ID3D12Device* dev)
{
    D3D12_FEATURE_DATA_SHADER_MODEL data = { D3D_SHADER_MODEL_6_9 };
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data))))
    {
        // data.HighestShaderModel может быть 0x60..0x67 даже если в SDK нет enum-имен
        return data.HighestShaderModel;
    }
    return D3D_SHADER_MODEL_6_0; // самый безопасный fallback
}

static std::wstring BuildProfile(const char* stage4cc, D3D_SHADER_MODEL sm)
{
    // В D3D12 SM кодируют как 0xMN (6_0=0x60, 6_7=0x67)
    unsigned v = static_cast<unsigned>(sm);
    unsigned major = (v >> 4) & 0xF;   // 6
    unsigned minor = v & 0xF;          // 0..7
    // на всякий случай кламп
    if (major < 6) { major = 6; minor = 0; }
    if (minor > 9) { minor = 9; }

    wchar_t buf[16];
    swprintf_s(buf, L"%hs_%u_%u", stage4cc, major, minor);
    return std::wstring(buf);
}

// Include handler для DXC с захватом списка файлов
struct IncludeCaptureDXC : public IDxcIncludeHandler
{
    std::atomic<ULONG> refcnt{ 1 };
    std::filesystem::path baseDir;
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    std::vector<std::wstring>& outFiles;

    IncludeCaptureDXC(IDxcUtils* u, const std::filesystem::path& base, std::vector<std::wstring>& out)
        : baseDir(base), utils(u), outFiles(out) {
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IDxcIncludeHandler*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refcnt; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG v = --refcnt; if (v == 0) delete this; return v;
    }

    // IDxcIncludeHandler
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override
    {
        std::filesystem::path req(pFilename ? pFilename : L"");
        std::filesystem::path full = req.is_absolute() ? req : (baseDir / req);
        full = full.lexically_normal();
        outFiles.push_back(full.wstring());

        Microsoft::WRL::ComPtr<IDxcBlobEncoding> enc;
        HRESULT hr = utils->LoadFile(full.c_str(), nullptr, &enc);
        if (FAILED(hr)) return hr;
        *ppIncludeSource = enc.Detach();
        return S_OK;
    }
};

static HRESULT CompileDXC(const std::wstring& file,
    const char* entry,
    const char* stage4cc,   // "vs","ps","cs"
    ID3D12Device* device,
    const Material::DefineList& defines,
    ComPtr<ID3DBlob>& outBlob,
    std::vector<std::wstring>& outIncludes)
{
    outBlob.Reset();
    outIncludes.clear();

    // DXC utils/compiler
    ComPtr<IDxcUtils>     utils;
    ComPtr<IDxcCompiler3> compiler;
    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hr)) return hr;
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hr)) return hr;

    // Загружаем исходник
    std::filesystem::path path = std::filesystem::path(file).lexically_normal();
    ComPtr<IDxcBlobEncoding> src;
    hr = utils->LoadFile(path.c_str(), nullptr, &src);
    if (FAILED(hr)) return hr;

    // Include handler с захватом
    IncludeCaptureDXC* inc = new IncludeCaptureDXC(utils.Get(), path.parent_path(), outIncludes);

    // Target: max supported SM
    const D3D_SHADER_MODEL sm = QueryMaxShaderModel(device);
    const std::wstring target = BuildProfile(stage4cc, sm);

    // Строим устойчиво: все wchar-строки живут до конца функции
    std::wstring wEntry(entry, entry + std::strlen(entry));
    std::vector<std::wstring> owned; owned.reserve(16 + defines.size());

    // Базовые аргументы
    owned.push_back(L"-E");           // 0
    owned.push_back(wEntry);          // 1
    owned.push_back(L"-T");           // 2
    owned.push_back(target);          // 3
    owned.push_back(L"-Zpr");         // 4 (row-major)
    owned.push_back(L"-HV");          // 5
    owned.push_back(L"2021");         // 6
#ifdef _DEBUG
    owned.push_back(L"-Zi");          // 7
    owned.push_back(L"-Qembed_debug");// 8
    owned.push_back(L"-Od");          // 9
#else
    owned.push_back(L"-O3");
    owned.push_back(L"-Qstrip_debug");
    //owned.push_back(L"-Qstrip_reflect");
#endif

    // Дефайны
    for (auto& kv : defines) {
        std::wstring w = L"-D";
        w += std::wstring(kv.first.begin(), kv.first.end());
        if (!kv.second.empty()) {
            w += L"=" + std::wstring(kv.second.begin(), kv.second.end());
        }
        owned.push_back(std::move(w));
    }

    // Собираем массив LPCWSTR
    std::vector<LPCWSTR> args; args.reserve(owned.size());
    {
        for (auto& s : owned) args.push_back(s.c_str());
    }

    // Компиляция
    DxcBuffer buf{}; buf.Ptr = src->GetBufferPointer(); buf.Size = src->GetBufferSize(); buf.Encoding = 0;
    ComPtr<IDxcResult> result;
    hr = compiler->Compile(&buf, args.data(), (UINT)args.size(), inc, IID_PPV_ARGS(&result));
    inc->Release();

    if (FAILED(hr)) return hr;

    // errors
    ComPtr<IDxcBlobUtf8> errs;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr);
    if (errs && errs->GetStringLength() > 0) {
        OutputDebugStringA(errs->GetStringPointer());
    }

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) return status;

    // DXIL
    ComPtr<IDxcBlob> dxil;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxil), nullptr);
    if (!dxil) return E_FAIL;

    // Приводим к ID3DBlob через копию (чтобы остальной код не менять)
    ComPtr<ID3DBlob> blob;
    ThrowIfFailed(D3DCreateBlob(dxil->GetBufferSize(), &blob));
    std::memcpy(blob->GetBufferPointer(), dxil->GetBufferPointer(), dxil->GetBufferSize());
    outBlob = blob;

    return S_OK;
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

    std::vector<RootParameterInfo> params;
    std::vector<std::wstring> inc;

    if (!BuildGraphicsPSO(r, gd, rootSignature_, pipelineState_, pipelineStateWire_, params, inc)) {
        OutputDebugStringA("[Material] CreateGraphics failed\n");
        return;
    }

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
    ComPtr<ID3D12PipelineState> newPSOWire;
    std::vector<RootParameterInfo> newParams;
    std::vector<std::wstring> inc;

    bool ok = false;
    if (isCompute_) {
        ok = BuildComputePSO(r, cachedCmpDesc_, newRS, newPSO, newParams, inc);
    }
    else {
        ok = BuildGraphicsPSO(r, cachedGfxDesc_, newRS, newPSO, newPSOWire, newParams, inc);
    }

    if (!ok) {
        return false; // оставим pending=true — попробуем на следующем тике
    }

    retired_.push_back({ pipelineState_, rootSignature_, frameNumber });

    pipelineState_ = newPSO;
    pipelineStateWire_ = newPSOWire;
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

void Material::Bind(ID3D12GraphicsCommandList* cmdList, const RenderContext& ctx, bool wireframe) const
{
    if (isCompute_) { cmdList->SetComputeRootSignature(rootSignature_.Get()); }
    else { cmdList->SetGraphicsRootSignature(rootSignature_.Get()); }

    if (wireframe && pipelineStateWire_)
    {
        cmdList->SetPipelineState(pipelineStateWire_.Get());
    }else
    {
	    cmdList->SetPipelineState(pipelineState_.Get());
    }

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
    ComPtr<ID3D12PipelineState>& outPSOWire,
    std::vector<RootParameterInfo>& outParams,
    std::vector<std::wstring>& outIncludes)
{
    RootSignatureLayout layoutParsed;
    {
        std::string src = ReadFileToString(gd.shaderFile);
        ParseRootSignatureFromSource(src, layoutParsed);
    }

    ComPtr<ID3DBlob> vs, ps;
    std::vector<std::wstring> incVS, incPS;

    // SM6+ через DXC
    if (FAILED(CompileDXC(gd.shaderFile, gd.vsEntry, "vs", r->GetDevice(), gd.defines, vs, incVS))) {
        OutputDebugStringA("[Material] DXC VS failed, fallback to D3DCompile SM5\n");
        UINT cf =
#ifdef _DEBUG
        (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
#else
            D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        if (FAILED(CompileWithIncludes(gd.shaderFile, gd.vsEntry, "vs_5_0", cf, gd.defines, vs, incVS))) {
            return false;
        }
    }
    if (FAILED(CompileDXC(gd.shaderFile, gd.psEntry, "ps", r->GetDevice(), gd.defines, ps, incPS))) {
        OutputDebugStringA("[Material] DXC PS failed, fallback to D3DCompile SM5\n");
        UINT cf =
#ifdef _DEBUG
        (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
#else
            D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        if (FAILED(CompileWithIncludes(gd.shaderFile, gd.psEntry, "ps_5_0", cf, gd.defines, ps, incPS))) {
            return false;
        }
    }

    // рефлексия (работает для DXIL и DXBC)
    cbInfos_.clear();
    ReflectShaderBlob(vs.Get(), cbInfos_);
    ReflectShaderBlob(ps.Get(), cbInfos_);

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

    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.RasterizerState = gd.raster;
    pso.BlendState = gd.blend;
    pso.DepthStencilState = gd.depth;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = gd.topologyType;
    pso.NumRenderTargets = gd.numRT;
    for (UINT i = 0; i < gd.numRT; ++i) {
        pso.RTVFormats[i] = gd.rtvFormats[i];
    }
    pso.DSVFormat = gd.dsvFormat;
    pso.SampleDesc.Count = gd.sampleCount;

    if (FAILED(r->GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&outPSO)))) {
        return false;
    }

    if (pso.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
    {
	    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    	pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

    	if (FAILED(r->GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&outPSOWire)))) {
    		return false;
    	}
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
    ComPtr<ID3DBlob> cs;
    std::vector<std::wstring> incCS;
    if (FAILED(CompileDXC(cd.shaderFile, cd.csEntry, "cs", r->GetDevice(), cd.defines, cs, incCS))) {
        OutputDebugStringA("[Material] DXC CS failed, fallback to D3DCompile SM5\n");
        UINT cf =
#ifdef _DEBUG
        (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
#else
            D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        if (FAILED(CompileWithIncludes(cd.shaderFile, cd.csEntry, "cs_5_0", cf, cd.defines, cs, incCS))) {
            return false;
        }
    }

    cbInfos_.clear();
    ReflectShaderBlob(cs.Get(), cbInfos_);

    RootSignatureLayout layoutParsed;
    {
        std::string src = ReadFileToString(cd.shaderFile);
        ParseRootSignatureFromSource(src, layoutParsed);
    }
    BuildRootFromLayout(r->GetDevice(), layoutParsed, cd.rsFlags, outRS, outParams);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = outRS.Get();
    pso.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
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
        fmts += std::to_wstring((int)(gd.rtvFormats[i])) + L",";
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
    if (!cb)
    {
	    return false;
    }
    auto it = cb->fieldsByName.find(name);
    if (it == cb->fieldsByName.end())
    {
	    return false;
    }
    outOffset = it->second.offset;
    outSize = it->second.size;
    return true;
}

void Material::ProcessReflection(ID3D12ShaderReflection* refl,
    std::unordered_map<UINT, CBufferInfo>& io)
{
    if (!refl) {return;}

    D3D12_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd))) {return;}

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
        if (FAILED(cb->GetDesc(&cbd))) {continue;}

        std::string cbName = cbd.Name ? cbd.Name : "";
        auto itBind = bindOfCB.find(cbName);
        if (itBind == bindOfCB.end()) {continue;} // пропускаем $Globals и пр.

        const UINT bReg = itBind->second;
        auto& dst = io[bReg];
        dst.bindRegister = bReg;
        dst.sizeBytes = std::max<UINT>(dst.sizeBytes, cbd.Size);

        for (UINT v = 0; v < cbd.Variables; ++v) {
            ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D12_SHADER_VARIABLE_DESC vd{};
            if (FAILED(var->GetDesc(&vd))) {continue;}

            CBufferField f{};
            f.name = vd.Name ? vd.Name : "";
            f.offset = vd.StartOffset;
            f.size = vd.Size;

            dst.fieldsByName[f.name] = f; // объединяем поля с других стадий, offset должен совпадать
        }
    }
}

void Material::ReflectShaderBlob(ID3DBlob* blob,
    std::unordered_map<UINT, CBufferInfo>& io)
{
    if (!blob) return;

    // Сначала попробуем DXIL (DXC)
    {
        Microsoft::WRL::ComPtr<IDxcUtils> utils;
        if (SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
            DxcBuffer db{};
            db.Ptr = blob->GetBufferPointer();
            db.Size = blob->GetBufferSize();
            db.Encoding = 0;

            Microsoft::WRL::ComPtr<ID3D12ShaderReflection> refl;
            if (SUCCEEDED(utils->CreateReflection(&db, IID_PPV_ARGS(&refl))) && refl) {
                ProcessReflection(refl.Get(), io);
                return;
            }
        }
    }

    // Если это не DXIL, пробуем старый DXBC путь
    {
        Microsoft::WRL::ComPtr<ID3D12ShaderReflection> refl;
        if (SUCCEEDED(D3DReflect(blob->GetBufferPointer(),
            blob->GetBufferSize(),
            __uuidof(ID3D12ShaderReflection),
            (void**)refl.GetAddressOf())))
        {
            ProcessReflection(refl.Get(), io);
            return;
        }
    }
}