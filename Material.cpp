#include "Material.h"

#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "Helpers.h"
#include "RootSignatureLayout.h"
#include "RootSignatureParser.h"

#include <d3dcompiler.h>        // для ID3D12ShaderReflection


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

void Material::CreateGraphics(Renderer* renderer, const GraphicsDesc& gd) {
    isCompute_ = false;

    // 1) Парсим RS из исходника
    RootSignatureLayout layoutParsed;
    {
        std::string src = ReadFileToString(gd.shaderFile);
        ParseRootSignatureFromSource(src, layoutParsed);
    }

    // 2) Компиляция FXC (vs_5_0/ps_5_0) — точки входа из gd
    ComPtr<ID3DBlob> vs5, ps5, err;
    UINT cf = 0;
#ifdef _DEBUG
    cf = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    cf = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    HRESULT hr = D3DCompileFromFile(gd.shaderFile.c_str(), nullptr, nullptr, gd.vsEntry, "vs_5_0", cf, 0, &vs5, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((char*)err->GetBufferPointer()); ThrowIfFailed(hr); }
    hr = D3DCompileFromFile(gd.shaderFile.c_str(), nullptr, nullptr, gd.psEntry, "ps_5_0", cf, 0, &ps5, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((char*)err->GetBufferPointer()); ThrowIfFailed(hr); }

    // 3) RootSignature с флагами из gd
    BuildRootFromLayout(renderer->GetDevice(), layoutParsed, gd.rsFlags, rootSignature_, rootParams_);

    // 4) PSO
    auto inLayout = renderer->GetInputLayoutManager().Get(gd.inputLayoutKey);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = rootSignature_.Get();
    pso.InputLayout = { inLayout.desc, inLayout.count };
    pso.VS = { vs5->GetBufferPointer(), vs5->GetBufferSize() };
    pso.PS = { ps5->GetBufferPointer(), ps5->GetBufferSize() };
    pso.RasterizerState = gd.raster;
    pso.BlendState = gd.blend;
    pso.DepthStencilState = gd.depth;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = gd.topologyType;
    pso.NumRenderTargets = gd.numRT;
    pso.RTVFormats[0] = gd.rtvFormat;
    pso.DSVFormat = gd.dsvFormat;
    pso.SampleDesc.Count = gd.sampleCount;

    ThrowIfFailed(renderer->GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipelineState_)));
}

void Material::CreateCompute(Renderer* renderer, const std::wstring& shaderFile) {
    isCompute_ = true;

    // 1) Парсим RS из источника (если есть)
    RootSignatureLayout layoutParsed;
    {
        std::string src = ReadFileToString(shaderFile);
        ParseRootSignatureFromSource(src, layoutParsed);
    }

    // 2) Компиляция (DXC → FXC fallback)
    ComPtr<ID3DBlob> cs5, err;
    UINT cf = 0;
#ifdef _DEBUG
    cf = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    cf = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    HRESULT hr = D3DCompileFromFile(shaderFile.c_str(), nullptr, nullptr, "main", "cs_5_0", cf, 0, &cs5, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((char*)err->GetBufferPointer()); ThrowIfFailed(hr); }

    // 3) Сбор RS: compute-флаги
    const D3D12_ROOT_SIGNATURE_FLAGS csFlags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    BuildRootFromLayout(renderer->GetDevice(), layoutParsed, csFlags, rootSignature_, rootParams_);

    // 4) PSO
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = rootSignature_.Get();
    pso.CS.pShaderBytecode = cs5->GetBufferPointer();
    pso.CS.BytecodeLength = cs5->GetBufferSize();
    ThrowIfFailed(renderer->GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipelineState_)));
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