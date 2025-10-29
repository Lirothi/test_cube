#pragma once

#include <cstdint>
#include <string_view>

enum class RenderPass : uint16_t {
    Main_PrologueClear,
    Main_CSM,
    Main_SpotShadows,
    Main_GBuffer,
    Main_Lighting,
    Main_SpotLights,
    Main_PointLights,
    Main_Skybox,
    Main_SSR,
    Main_SSRBlur,
    Main_Compose,
    Main_Transparent,
    Main_DebugDraw,
    Main_Tonemap,
    Main_Debug,
    Main_Count,

    Epilogue_Overlay = Main_Count,
    Epilogue_Count,

    GBuffer_Driver = Epilogue_Count,
    GBuffer_OpaqueSimple,
    GBuffer_OpaqueComplex,
    GBuffer_Count,

    Transparent_Driver = GBuffer_Count,
    Transparent_Simple,
    Transparent_Complex,
    Transparent_Count,

    Count = Transparent_Count
};

inline std::wstring_view RenderPassToWString(RenderPass pass)
{
    switch (pass)
    {
    case RenderPass::Main_PrologueClear: return L"PrologueClear";
    case RenderPass::Main_CSM: return L"CSM";
    case RenderPass::Main_SpotShadows: return L"SpotShadows";
    case RenderPass::Main_GBuffer: return L"GBuffer";
    case RenderPass::Main_Lighting: return L"Lighting";
    case RenderPass::Main_SpotLights: return L"SpotLights";
    case RenderPass::Main_PointLights: return L"PointLights";
    case RenderPass::Main_Skybox: return L"Skybox";
    case RenderPass::Main_SSR: return L"SSR";
    case RenderPass::Main_SSRBlur: return L"SSR.Blur";
    case RenderPass::Main_Compose: return L"Compose";
    case RenderPass::Main_Transparent: return L"Transparent";
    case RenderPass::Main_DebugDraw: return L"DebugDraw";
    case RenderPass::Main_Tonemap: return L"Tonemap";
    case RenderPass::Main_Debug: return L"Debug";
    case RenderPass::Epilogue_Overlay: return L"Overlay";
    case RenderPass::GBuffer_Driver: return L"GBuffer.Driver";
    case RenderPass::GBuffer_OpaqueSimple: return L"GBuffer.OpaqueSimple";
    case RenderPass::GBuffer_OpaqueComplex: return L"GBuffer.OpaqueComplex";
    case RenderPass::Transparent_Driver: return L"Transparent.Driver";
    case RenderPass::Transparent_Simple: return L"Transparent.Simple";
    case RenderPass::Transparent_Complex: return L"Transparent.Complex";
    default: return {};
    }
}

