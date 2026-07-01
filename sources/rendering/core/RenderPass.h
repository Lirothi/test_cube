#pragma once

#include <cstdint>
#include <string_view>

enum class RenderPass : uint16_t {
    Main_BuildAS,
    Main_PrologueClear,
    Main_ObjectCompute,
    Main_TerrainDepth,
    Main_CSM,
    Main_SpotShadows,
    Main_GBuffer,
    Main_ObjectIdReadback,
    Main_Lighting,
    Main_SpotLights,
    Main_PointLights,
    Main_Skybox,
    Main_ReflectionSource,
    Main_ReflectionBlur,
    Main_Compose,
    Main_RTReflections,
    Main_RTDenoise,
    Main_RTDebug,
    Main_GlassReflGbuffer,
    Main_GlassReflections,
    Main_Transparent,
    Main_DebugDraw,
    Main_SelectionOutline,
    Main_Tonemap,
    Main_Debug,
    Main_Count,

    Epilogue_Overlay = Main_Count,
    Epilogue_Count,

    GBuffer_Driver = Epilogue_Count,
    GBuffer_OpaqueSimple,
    GBuffer_OpaqueComplex,
    GBuffer_Selected,
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
    case RenderPass::Main_BuildAS: return L"BuildAS";
    case RenderPass::Main_PrologueClear: return L"PrologueClear";
    case RenderPass::Main_ObjectCompute: return L"ObjectCompute";
    case RenderPass::Main_TerrainDepth: return L"TerrainDepth";
    case RenderPass::Main_CSM: return L"CSM";
    case RenderPass::Main_SpotShadows: return L"SpotShadows";
    case RenderPass::Main_GBuffer: return L"GBuffer";
    case RenderPass::Main_ObjectIdReadback: return L"ObjectIdReadback";
    case RenderPass::Main_Lighting: return L"Lighting";
    case RenderPass::Main_SpotLights: return L"SpotLights";
    case RenderPass::Main_PointLights: return L"PointLights";
    case RenderPass::Main_Skybox: return L"Skybox";
    case RenderPass::Main_ReflectionSource: return L"ReflectionSource";
    case RenderPass::Main_ReflectionBlur: return L"Reflection.Blur";
    case RenderPass::Main_Compose: return L"Compose";
    case RenderPass::Main_RTReflections: return L"RTReflections";
    case RenderPass::Main_RTDenoise: return L"RTDenoise";
    case RenderPass::Main_GlassReflGbuffer: return L"GlassReflGbuffer";
    case RenderPass::Main_GlassReflections: return L"GlassReflections";
    case RenderPass::Main_RTDebug: return L"RTDebug";
    case RenderPass::Main_Transparent: return L"Transparent";
    case RenderPass::Main_DebugDraw: return L"DebugDraw";
    case RenderPass::Main_SelectionOutline: return L"SelectionOutline";
    case RenderPass::Main_Tonemap: return L"Tonemap";
    case RenderPass::Main_Debug: return L"Debug";
    case RenderPass::Epilogue_Overlay: return L"Overlay";
    case RenderPass::GBuffer_Driver: return L"GBuffer.Driver";
    case RenderPass::GBuffer_OpaqueSimple: return L"GBuffer.OpaqueSimple";
    case RenderPass::GBuffer_OpaqueComplex: return L"GBuffer.OpaqueComplex";
    case RenderPass::GBuffer_Selected: return L"GBuffer.Selected";
    case RenderPass::Transparent_Driver: return L"Transparent.Driver";
    case RenderPass::Transparent_Simple: return L"Transparent.Simple";
    case RenderPass::Transparent_Complex: return L"Transparent.Complex";
    default: return {};
    }
}

