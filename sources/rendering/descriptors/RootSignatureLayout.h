#pragma once
#include <string>
#include <vector>
#include <d3d12.h>

struct RootSignatureParameter {
    D3D12_ROOT_PARAMETER_TYPE type;
    UINT shaderRegister = 0;
    UINT registerSpace = 0;
    UINT num32BitValues = 0; // for CONSTANTS
    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;

    // For TABLE:
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    bool hasSamplerRanges = false; // true when the table contains SAMPLER ranges
};

struct RootSignatureLayout {
    std::vector<RootSignatureParameter> params;

    // Internal buffers used during serialization
    std::vector<D3D12_ROOT_PARAMETER> paramsD3D12;
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> stableRanges;

    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // --- Root descriptors / constants ---
    void AddCBV(UINT reg, UINT space = 0, D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) {
        RootSignatureParameter p{};
        p.type = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.shaderRegister = reg;
        p.registerSpace = space;
        p.visibility = vis;
        params.push_back(p);
    }

    void AddSRV(UINT reg, UINT space = 0, D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) {
        RootSignatureParameter p{};
        p.type = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p.shaderRegister = reg;
        p.registerSpace = space;
        p.visibility = vis;
        params.push_back(p);
    }

    void AddUAV(UINT reg, UINT space = 0, D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) {
        RootSignatureParameter p{};
        p.type = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p.shaderRegister = reg;
        p.registerSpace = space;
        p.visibility = vis;
        params.push_back(p);
    }

    void AddConstants(UINT reg, UINT num32Bit, UINT space = 0, D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) {
        RootSignatureParameter p{};
        p.type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.shaderRegister = reg;
        p.registerSpace = space;
        p.num32BitValues = num32Bit;
        p.visibility = vis;
        params.push_back(p);
    }

    // --- Helpers for descriptor table ranges ---
    static D3D12_DESCRIPTOR_RANGE MakeRangeCBV(UINT baseReg, UINT num = 1, UINT space = 0) {
        D3D12_DESCRIPTOR_RANGE r{};
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        r.NumDescriptors = num;
        r.BaseShaderRegister = baseReg;
        r.RegisterSpace = space;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        return r;
    }

    static D3D12_DESCRIPTOR_RANGE MakeRangeSRV(UINT baseReg, UINT num = 1, UINT space = 0) {
        D3D12_DESCRIPTOR_RANGE r{};
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = num;
        r.BaseShaderRegister = baseReg;
        r.RegisterSpace = space;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        return r;
    }

    static D3D12_DESCRIPTOR_RANGE MakeRangeUAV(UINT baseReg, UINT num = 1, UINT space = 0) {
        D3D12_DESCRIPTOR_RANGE r{};
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        r.NumDescriptors = num;
        r.BaseShaderRegister = baseReg;
        r.RegisterSpace = space;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        return r;
    }

    static D3D12_DESCRIPTOR_RANGE MakeRangeSampler(UINT baseReg, UINT num = 1, UINT space = 0) {
        D3D12_DESCRIPTOR_RANGE r{};
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        r.NumDescriptors = num;
        r.BaseShaderRegister = baseReg;
        r.RegisterSpace = space;
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        return r;
    }

    // --- Tables (TABLE) ---
    void AddTable(const std::vector<D3D12_DESCRIPTOR_RANGE>& ranges,
        D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) {
        RootSignatureParameter p{};
        p.type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.visibility = vis;
        p.ranges = ranges;
        p.shaderRegister = ranges.empty() ? 0u : ranges.front().BaseShaderRegister;
        p.registerSpace = ranges.empty() ? 0u : ranges.front().RegisterSpace;

        p.hasSamplerRanges = false;
        for (const auto& r : ranges) {
            if (r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
                p.hasSamplerRanges = true;
                break;
            }
        }
        params.push_back(p);
    }

    // Convenient overload for a single range
    void AddTable(D3D12_DESCRIPTOR_RANGE range,
        D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) {
        RootSignatureParameter p{};
        p.type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.visibility = vis;
        p.ranges.push_back(range);
        p.shaderRegister = range.BaseShaderRegister;
        p.registerSpace = range.RegisterSpace;
        p.hasSamplerRanges = (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
        params.push_back(p);
    }

    // Handy shortcuts for single-range tables
    void AddTableCBV(UINT baseReg, UINT num = 1, UINT space = 0, D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) { AddTable(MakeRangeCBV(baseReg, num, space), vis); }
    void AddTableSRV(UINT baseReg, UINT num = 1, UINT space = 0, D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) { AddTable(MakeRangeSRV(baseReg, num, space), vis); }
    void AddTableUAV(UINT baseReg, UINT num = 1, UINT space = 0, D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) { AddTable(MakeRangeUAV(baseReg, num, space), vis); }
    void AddTableSampler(UINT baseReg, UINT num = 1, UINT space = 0, D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) { AddTable(MakeRangeSampler(baseReg, num, space), vis); }

    // --- IMPORTANT: single SAMPLER as a shorthand for a sampler table ---
    // This is what the parser needed: it calls layout.AddSampler(sN, space)
    void AddSampler(UINT reg, UINT space = 0, D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL) {
        AddTableSampler(reg, 1, space, vis);
    }
};

inline D3D12_ROOT_SIGNATURE_DESC MakeRootSignatureDesc(RootSignatureLayout& layout) {
    layout.paramsD3D12.clear();
    layout.stableRanges.clear();
    layout.paramsD3D12.reserve(layout.params.size());
    layout.stableRanges.reserve(layout.params.size());

    for (const auto& p : layout.params) {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = p.type;
        param.ShaderVisibility = p.visibility;

        if (p.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            layout.stableRanges.push_back(p.ranges); // Copy to keep a stable buffer
            param.DescriptorTable.NumDescriptorRanges = (UINT)layout.stableRanges.back().size();
            param.DescriptorTable.pDescriptorRanges = layout.stableRanges.back().data();
        }
        else if (p.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            param.Constants.ShaderRegister = p.shaderRegister;
            param.Constants.RegisterSpace = p.registerSpace;
            param.Constants.Num32BitValues = p.num32BitValues;
        }
        else {
            param.Descriptor.ShaderRegister = p.shaderRegister;
            param.Descriptor.RegisterSpace = p.registerSpace;
        }

        layout.paramsD3D12.push_back(param);
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = (UINT)layout.paramsD3D12.size();
    desc.pParameters = layout.paramsD3D12.data();
    desc.NumStaticSamplers = 0;
    desc.pStaticSamplers = nullptr;
    desc.Flags = layout.flags;
    return desc;
}