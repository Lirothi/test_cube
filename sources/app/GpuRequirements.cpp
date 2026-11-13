#include "app/GpuRequirements.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace gpu
{
namespace
{
    std::string Narrow(const wchar_t* text)
    {
        char out[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof(out) - 1, nullptr, nullptr);
        return out;
    }

    std::wstring Widen(const std::string& text)
    {
        if (text.empty()) { return {}; }
        const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
        return out;
    }
}

RequirementCheck CheckRequirements()
{
    using Microsoft::WRL::ComPtr;
    RequirementCheck check;
    ComPtr<ID3D12Device> device;
    // The adapter the renderer takes (GraphicsDevice: D3D12CreateDevice(nullptr, ...)): the default one.
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || !device)
    {
        check.missing.push_back("Direct3D 12 (no device could be created on the default graphics adapter)");
        return check;
    }
    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            check.adapter = Narrow(desc.Description);
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
            {
                char version[64];
                std::snprintf(version, sizeof(version), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                              HIWORD(umd.LowPart), LOWORD(umd.LowPart));
                check.driver = version;
            }
        }
    }

    D3D12_FEATURE_DATA_SHADER_MODEL model{ D3D_SHADER_MODEL_6_6 };
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))))
    {
        model.HighestShaderModel = D3D_SHADER_MODEL_5_1;
    }
    if (model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
    {
        char text[96];
        std::snprintf(text, sizeof(text), "shader model 6.6 (it has %u.%u)", (model.HighestShaderModel >> 4) & 0xF,
                      model.HighestShaderModel & 0xF);
        check.missing.push_back(text);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
    {
        check.missing.push_back("resource binding tier 3 (bindless resources)");
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    {
        check.missing.push_back("DirectX Raytracing 1.1 (hardware ray tracing)");
    }
    check.ok = check.missing.empty();
    return check;
}

std::wstring RequirementMessage(const RequirementCheck& check, const wchar_t* product)
{
    std::wstring text = std::wstring(product) + L" cannot run on this graphics card";
    if (!check.adapter.empty()) { text += L" (" + Widen(check.adapter) + L")"; }
    text += L". It needs:\n";
    for (const std::string& m : check.missing) { text += L"\n  - " + Widen(m); }
    text += L"\n\nThat is a DirectX 12 card with hardware ray tracing: NVIDIA GeForce RTX 20 series or newer, "
            L"AMD Radeon RX 6000 series or newer, Intel Arc. If the card is one of those, update its driver";
    if (!check.driver.empty()) { text += L" (installed: " + Widen(check.driver) + L")"; }
    text += L" and Windows.";
    return text;
}
}
