#pragma once

#include "core/profiling/Profiler.h"

namespace ProfilerScopes {

// App
extern const Profiler::ScopeNameKey kWholeCycle;
extern const Profiler::ScopeNameKey kWinMessages;
extern const Profiler::ScopeNameKey kAppControllerTick;
extern const Profiler::ScopeNameKey kBuildDeveloperWindow;
extern const Profiler::ScopeNameKey kTextureDebugViewerDraw;

// Material
extern const Profiler::ScopeNameKey kMaterialFSProbe;

// Renderer
extern const Profiler::ScopeNameKey kRendererWaitForFrame;
extern const Profiler::ScopeNameKey kRendererBeginFrame;
extern const Profiler::ScopeNameKey kRendererBeginThreadCommandList;
extern const Profiler::ScopeNameKey kRendererEndThreadCommandList;
extern const Profiler::ScopeNameKey kRendererEndThreadCommandBundle;
extern const Profiler::ScopeNameKey kRendererExecuteTimelineAndPresent;
extern const Profiler::ScopeNameKey kRendererTransition;

// Scene
extern const Profiler::ScopeNameKey kSceneTick;
extern const Profiler::ScopeNameKey kSceneRender;
extern const Profiler::ScopeNameKey kPassPrologueClear;
extern const Profiler::ScopeNameKey kPassObjectCompute;
extern const Profiler::ScopeNameKey kPassCSM;
extern const Profiler::ScopeNameKey kPassShoreDepth;
extern const Profiler::ScopeNameKey kPassGBuffer;
extern const Profiler::ScopeNameKey kPassLighting;
extern const Profiler::ScopeNameKey kPassSpotShadow;
extern const Profiler::ScopeNameKey kSpotShadowPerLight;
extern const Profiler::ScopeNameKey kPassSpotLights;
extern const Profiler::ScopeNameKey kPassPointLights;
extern const Profiler::ScopeNameKey kPassSkybox;
extern const Profiler::ScopeNameKey kPassBuildAS;
extern const Profiler::ScopeNameKey kPassReflectionSource;
extern const Profiler::ScopeNameKey kPassRTReflections;
extern const Profiler::ScopeNameKey kPassRTDenoise;
extern const Profiler::ScopeNameKey kPassReflectionBlur;
extern const Profiler::ScopeNameKey kPassCompose;
extern const Profiler::ScopeNameKey kPassRTDebug;
extern const Profiler::ScopeNameKey kPassGlassReflGbuffer;
extern const Profiler::ScopeNameKey kPassGlassReflections;
extern const Profiler::ScopeNameKey kPassTransparent;
extern const Profiler::ScopeNameKey kPassOceanReflection;
extern const Profiler::ScopeNameKey kPassDebugDraw;
extern const Profiler::ScopeNameKey kPassTonemap;
extern const Profiler::ScopeNameKey kPassDebug;
extern const Profiler::ScopeNameKey kFrameAsyncWait;
extern const Profiler::ScopeNameKey kPassOverlay;
extern const Profiler::ScopeNameKey kOverlayAsyncWait;
extern const Profiler::ScopeNameKey kRenderObjectBatchAsync;
extern const Profiler::ScopeNameKey kCSMPerCascade;
extern const Profiler::ScopeNameKey kRenderObjectBatchGpu;
extern const Profiler::ScopeNameKey kGBufferDriver;
extern const Profiler::ScopeNameKey kTransparentDriver;
extern const Profiler::ScopeNameKey kOceanRender;
extern const Profiler::ScopeNameKey kPrepareTransparentBuckets;
extern const Profiler::ScopeNameKey kAddPass;
extern const Profiler::ScopeNameKey kPrepareViews;
extern const Profiler::ScopeNameKey kPrepareQueue;
extern const Profiler::ScopeNameKey kUpdateCascades;

// TextManager
extern const Profiler::ScopeNameKey kTextManagerBuild;
extern const Profiler::ScopeNameKey kTextManagerDraw;
extern const Profiler::ScopeNameKey kTextManagerAddText;
extern const Profiler::ScopeNameKey kTextManagerBuildGlyphRun;
extern const Profiler::ScopeNameKey kTextManagerEmitImmediate;

// RenderGraph
extern const Profiler::ScopeNameKey kRenderGraphExecute;
extern const Profiler::ScopeNameKey kRenderGraphExecuteParallel;

//Service
extern const Profiler::ScopeNameKey kService1;
extern const Profiler::ScopeNameKey kService2;
extern const Profiler::ScopeNameKey kService3;
extern const Profiler::ScopeNameKey kService4;

} // namespace ProfilerScopes

