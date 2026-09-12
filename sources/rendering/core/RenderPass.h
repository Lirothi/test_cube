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
    Main_CloudNoise, // C1: the noise set, seed-dirty
    Main_CloudShadow, // C3: the cloud shadow map from the sun, per frame
    Main_SkyDistant, // B5: isotropic ambient at 6 km
    Main_SkyEnvironment, // B4: dirty sky capture and IBL convolution
    Main_SkyAerial, // B3: per-view finite-distance atmosphere
    Main_SkyView, // B2: per-view atmosphere radiance
    Main_SkyAtmosphereDebug, // B1: LUT inspection after forward composition
    Main_SkyAtmosphereLuts, // B1: parameter-dirty, view-independent LUTs
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
    // Occlusion plan S5b: the cascades' light-space two-pass HZB cull -- the tile pyramids from
    // pass A, the deferred casters retested, pass B into the same tiles.
    Main_CsmHzb,
    Main_ShadowCullPost,
    Main_CSMPost,
    Main_SpotShadows,
    Main_PointShadows,
    Main_GBuffer,
    // Occlusion plan S5: the camera's two-pass HZB occlusion -- pass A's pyramid, the deferred
    // candidates retested, pass B into the same G-buffer.
    Main_HzbA,
    Main_CamCullPost,
    Main_GBufferB,
    Main_ObjectIdReadback,
    Main_VsmPageRequest,
    Main_VsmPageRender,
    Main_OcclusionQueries, // occlusion plan S3a: box queries against the G-buffer depth
    Main_Gtao,   // P6B screen-space ambient occlusion, between the G-buffer and lighting
    Main_VolumetricFog, // plan part A: the froxel scatter + integration, before lighting/compose
    Main_Hzb,    // P6C hierarchical depth pyramid, built from the G-buffer depth
    Main_VisTest, // occlusion plan S3b: the plan's boxes against the pyramid, one dispatch + readback
    Main_DebugPreview, // texture-inspector preview, drawn through our own shader
    Main_Lighting,
    Main_SpotLights,
    Main_PointLights,
    Main_Skybox,
    Main_CloudTrace, // C2: the half-res view march + temporal resolve, before compose applies it
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
    // B6.1, UE's order (DeferredShadingRenderer.cpp:3230 water, then 3262 sky+fog, then translucency):
    // Main_Transparent draws only the transparents that WRITE DEPTH -- the ocean, our SingleLayerWater
    // -- so that by the time the screen-space fog runs, the water is in the depth buffer and is fogged
    // by the same pass as everything else. Main_Translucent then draws what does NOT write depth
    // (glass, particles), after the fog, each fogging itself in its own shader exactly as UE's
    // translucency does. Splitting them is what makes ONE fog application possible at all.
    Main_Transparent,
    Main_TransparentFog,
    Main_Translucent,
    Main_LightShafts,     // plan A7: UE LightShaftBloom added into scene colour after the transparents
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
    GBuffer_Indirect, // occlusion plan S4: the registry's camera args, ExecuteIndirect per group
    GBuffer_Selected,
    GBuffer_Count,

    Transparent_Driver = GBuffer_Count,
    Transparent_Water,
    Transparent_Count,

    Translucent_Driver = Transparent_Count,
    Translucent_Simple,
    Translucent_Complex,
    Translucent_Selected,
    Translucent_Count,

    Count = Translucent_Count
};

inline std::wstring_view RenderPassToWString(RenderPass pass)
{
    switch (pass)
    {
    case RenderPass::Main_CloudNoise: return L"CloudNoise";
    case RenderPass::Main_CloudShadow: return L"CloudShadow";
    case RenderPass::Main_CloudTrace: return L"CloudTrace";
    case RenderPass::Main_SkyDistant: return L"SkyDistant";
    case RenderPass::Main_SkyEnvironment: return L"SkyEnvironment";
    case RenderPass::Main_SkyAerial: return L"SkyAerial";
    case RenderPass::Main_SkyView: return L"SkyView";
    case RenderPass::Main_SkyAtmosphereDebug: return L"SkyAtmosphereDebug";
    case RenderPass::Main_SkyAtmosphereLuts: return L"SkyAtmosphereLuts";
    case RenderPass::Main_BuildAS: return L"BuildAS";
    case RenderPass::Main_PrologueClear: return L"PrologueClear";
    case RenderPass::Main_GpuInstanceCompute: return L"GpuInstanceCompute";
    case RenderPass::Main_ObjectCompute: return L"ObjectCompute";
    case RenderPass::Main_SurfSim: return L"SurfSim";
    case RenderPass::Main_ShoreWetness: return L"ShoreWetness";
    case RenderPass::Main_TerrainDepth: return L"TerrainDepth";
    case RenderPass::Main_ShadowCull: return L"ShadowCull";
    case RenderPass::Main_CSM: return L"CSM";
    case RenderPass::Main_CsmHzb: return L"CsmHzb";
    case RenderPass::Main_ShadowCullPost: return L"ShadowCullPost";
    case RenderPass::Main_CSMPost: return L"CSMPost";
    case RenderPass::Main_SpotShadows: return L"SpotShadows";
    case RenderPass::Main_PointShadows: return L"PointShadows";
    case RenderPass::Main_GBuffer: return L"GBuffer";
    case RenderPass::Main_HzbA: return L"HzbA";
    case RenderPass::Main_CamCullPost: return L"CamCullPost";
    case RenderPass::Main_GBufferB: return L"GBufferB";
    case RenderPass::Main_ObjectIdReadback: return L"ObjectIdReadback";
    case RenderPass::Main_VsmPageRequest: return L"VsmPageRequest";
    case RenderPass::Main_VsmPageRender: return L"VsmPageRender";
    case RenderPass::Main_OcclusionQueries: return L"OcclusionQueries";
    case RenderPass::Main_Gtao: return L"Gtao";
    case RenderPass::Main_VolumetricFog: return L"VolumetricFog";
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
    case RenderPass::Main_TransparentFog: return L"TransparentFog";
    case RenderPass::Main_Translucent: return L"Translucent";
    case RenderPass::Main_LightShafts: return L"LightShafts";
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
    case RenderPass::GBuffer_Indirect: return L"GBuffer.Indirect";
    case RenderPass::GBuffer_Selected: return L"GBuffer.Selected";
    case RenderPass::Transparent_Driver: return L"Transparent.Driver";
    case RenderPass::Transparent_Water: return L"Transparent.Water";
    case RenderPass::Translucent_Driver: return L"Translucent.Driver";
    case RenderPass::Translucent_Simple: return L"Translucent.Simple";
    case RenderPass::Translucent_Complex: return L"Translucent.Complex";
    case RenderPass::Translucent_Selected: return L"Translucent.Selected";
    default: return {};
    }
}

