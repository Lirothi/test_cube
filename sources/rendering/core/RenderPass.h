#pragma once

#include <cstdint>
#include <string_view>

// Async-compute plan step 4 (design D1): the QUEUE a pass runs on.
//
// It is a property of the PASS, fixed when the graph is built — not of the command list, not of the
// material, and never decided at record time. Anything later than graph-build time makes the
// barrier compile undecidable, because that compile runs before any pass body records a thing.
// With AddPass2 the graph-build moment and the declaration moment are the same moment, which is
// what makes this expressible at all.
//
// INERT at step 4: `AsyncCompute` exists and is threaded through, and NOTHING selects it. Step 5
// adds the eligibility rules, step 8 moves the first pass.
enum class RenderQueue : uint8_t {
    Graphics,      // the direct queue — every pass, today
    AsyncCompute,  // the second queue (GraphicsDevice::ComputeQueue)
};

enum class RenderPass : uint16_t {
    Main_BuildAS,
    Main_PrologueClear,
    // Async-compute step 9: the GI rotation compute, split out of Main_ObjectCompute because
    // Main_ShadowCull consumes its output two passes later — it has no slack and never moves.
    Main_GpuInstanceCompute,
    Main_ObjectCompute,   // ocean sim + particles: consumed by Main_Transparent, whole-frame slack
    Main_SurfSim,
    Main_ShoreWetness,
    Main_TerrainDepth,
    Main_ShadowCull,
    Main_CSM,
    Main_SpotShadows,
    Main_PointShadows,
    Main_GBuffer,
    Main_ObjectIdReadback,
    Main_VsmPageRequest,
    Main_VsmPageRender,
    Main_OcclusionQueries, // occlusion plan S3a: box queries against the G-buffer depth
    Main_Gtao,   // P6B screen-space ambient occlusion, between the G-buffer and lighting
    Main_Hzb,    // P6C hierarchical depth pyramid, built from the G-buffer depth
    Main_VisTest, // occlusion plan S3b: the plan's boxes against the pyramid, one dispatch + readback
    Main_DebugPreview, // texture-inspector preview, drawn through our own shader
    Main_Lighting,
    Main_SpotLights,
    Main_PointLights,
    Main_Skybox,
    Main_ReflectionSource,
    Main_ReflectionTemporal, // SSR temporal resolve, between the trace and the glossy blur
    Main_ReflectionBlur,
    Main_Compose,
    // Gather-then-shade split of the opaque RT reflection (async-compute prep): RTTrace needs
    // only TLAS/depth/gb1 and is the pass that later moves to the compute queue; RTResolve is
    // the only RT consumer of the lighting output.
    Main_RTTrace,
    Main_RTResolve,
    Main_RTDebug,
    Main_GlassReflGbuffer,
    Main_GlassReflections,
    Main_Transparent,
    Main_DebugDraw,
    Main_SelectionOutline,
    Main_ExposureMetering, // P2: histogram + percentile solve, feeds the tonemap's exposure
    // DLSS-split: the upscale is its OWN pass so its ~116us of Streamline recording overlaps the
    // tonemap's bloom + tone curve instead of sitting in front of them in one command list.
    Main_DLSS,
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
    Transparent_Selected,
    Transparent_Count,

    Count = Transparent_Count
};

inline std::wstring_view RenderPassToWString(RenderPass pass)
{
    switch (pass)
    {
    case RenderPass::Main_BuildAS: return L"BuildAS";
    case RenderPass::Main_PrologueClear: return L"PrologueClear";
    case RenderPass::Main_GpuInstanceCompute: return L"GpuInstanceCompute";
    case RenderPass::Main_ObjectCompute: return L"ObjectCompute";
    case RenderPass::Main_SurfSim: return L"SurfSim";
    case RenderPass::Main_ShoreWetness: return L"ShoreWetness";
    case RenderPass::Main_TerrainDepth: return L"TerrainDepth";
    case RenderPass::Main_ShadowCull: return L"ShadowCull";
    case RenderPass::Main_CSM: return L"CSM";
    case RenderPass::Main_SpotShadows: return L"SpotShadows";
    case RenderPass::Main_PointShadows: return L"PointShadows";
    case RenderPass::Main_GBuffer: return L"GBuffer";
    case RenderPass::Main_ObjectIdReadback: return L"ObjectIdReadback";
    case RenderPass::Main_VsmPageRequest: return L"VsmPageRequest";
    case RenderPass::Main_VsmPageRender: return L"VsmPageRender";
    case RenderPass::Main_OcclusionQueries: return L"OcclusionQueries";
    case RenderPass::Main_Gtao: return L"Gtao";
    case RenderPass::Main_Hzb: return L"Hzb";
    case RenderPass::Main_VisTest: return L"VisTest";
    case RenderPass::Main_DebugPreview: return L"DebugPreview";
    case RenderPass::Main_Lighting: return L"Lighting";
    case RenderPass::Main_SpotLights: return L"SpotLights";
    case RenderPass::Main_PointLights: return L"PointLights";
    case RenderPass::Main_Skybox: return L"Skybox";
    case RenderPass::Main_ReflectionSource: return L"ReflectionSource";
    case RenderPass::Main_ReflectionTemporal: return L"Reflection.Temporal";
    case RenderPass::Main_ReflectionBlur: return L"Reflection.Blur";
    case RenderPass::Main_Compose: return L"Compose";
    case RenderPass::Main_RTTrace: return L"RTTrace";
    case RenderPass::Main_RTResolve: return L"RTResolve";
    case RenderPass::Main_GlassReflGbuffer: return L"GlassReflGbuffer";
    case RenderPass::Main_GlassReflections: return L"GlassReflections";
    case RenderPass::Main_RTDebug: return L"RTDebug";
    case RenderPass::Main_Transparent: return L"Transparent";
    case RenderPass::Main_DebugDraw: return L"DebugDraw";
    case RenderPass::Main_SelectionOutline: return L"SelectionOutline";
    case RenderPass::Main_ExposureMetering: return L"ExposureMetering";
    case RenderPass::Main_DLSS: return L"DLSS";
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
    case RenderPass::Transparent_Selected: return L"Transparent.Selected";
    default: return {};
    }
}

