#include "core/profiling/ProfilerScopes.h"

namespace ProfilerScopes {

const Profiler::ScopeNameKey kWholeCycle = Profiler::RegisterTraceLiteral(L"Whole Cycle");
const Profiler::ScopeNameKey kWinMessages = Profiler::RegisterTraceLiteral(L"Win Messages");

const Profiler::ScopeNameKey kMaterialFSProbe = Profiler::RegisterTraceLiteral(L"Material::FSProbeAndFlagPending");

const Profiler::ScopeNameKey kRendererWaitForFrame = Profiler::RegisterTraceLiteral(L"Renderer::WaitForFrame");
const Profiler::ScopeNameKey kRendererBeginFrame = Profiler::RegisterTraceLiteral(L"Renderer::BeginFrame");
const Profiler::ScopeNameKey kRendererBeginThreadCommandList = Profiler::RegisterTraceLiteral(L"Renderer::BeginThreadCommandList");
const Profiler::ScopeNameKey kRendererEndThreadCommandList = Profiler::RegisterTraceLiteral(L"Renderer::EndThreadCommandList");
const Profiler::ScopeNameKey kRendererEndThreadCommandBundle = Profiler::RegisterTraceLiteral(L"Renderer::EndThreadCommandBundle");
const Profiler::ScopeNameKey kRendererExecuteTimelineAndPresent = Profiler::RegisterTraceLiteral(L"Renderer::ExecuteTimelineAndPresent");
const Profiler::ScopeNameKey kRendererTransition = Profiler::RegisterTraceLiteral(L"Renderer::Transition");

const Profiler::ScopeNameKey kSceneTick = Profiler::RegisterTraceLiteral(L"Scene::Tick");
const Profiler::ScopeNameKey kSceneRender = Profiler::RegisterTraceLiteral(L"Scene::Render");
const Profiler::ScopeNameKey kPassPrologueClear = Profiler::RegisterTraceLiteral(L"Pass_PrologueClear");
const Profiler::ScopeNameKey kPassCSM = Profiler::RegisterTraceLiteral(L"Pass_CSM");
const Profiler::ScopeNameKey kPassGBuffer = Profiler::RegisterTraceLiteral(L"Pass_GBuffer");
const Profiler::ScopeNameKey kPassLighting = Profiler::RegisterTraceLiteral(L"Pass_Lighting");
const Profiler::ScopeNameKey kPassSpotShadow = Profiler::RegisterTraceLiteral(L"Pass_SpotShadows");
const Profiler::ScopeNameKey kPassSpotLights = Profiler::RegisterTraceLiteral(L"Pass_SpotLights");
const Profiler::ScopeNameKey kPassPointLights = Profiler::RegisterTraceLiteral(L"Pass_PointLights");
const Profiler::ScopeNameKey kPassSkybox = Profiler::RegisterTraceLiteral(L"Pass_Skybox");
const Profiler::ScopeNameKey kPassSSR = Profiler::RegisterTraceLiteral(L"Pass_SSR");
const Profiler::ScopeNameKey kPassSSRBlur = Profiler::RegisterTraceLiteral(L"Pass_SSR.Blur");
const Profiler::ScopeNameKey kPassCompose = Profiler::RegisterTraceLiteral(L"Pass_Compose");
const Profiler::ScopeNameKey kPassTransparent = Profiler::RegisterTraceLiteral(L"Pass_Transparent");
const Profiler::ScopeNameKey kPassTonemap = Profiler::RegisterTraceLiteral(L"Pass_Tonemap");
const Profiler::ScopeNameKey kPassDebug = Profiler::RegisterTraceLiteral(L"Pass_Debug");
const Profiler::ScopeNameKey kFrameAsyncWait = Profiler::RegisterTraceLiteral(L"Frame Async Wait");
const Profiler::ScopeNameKey kPassOverlay = Profiler::RegisterTraceLiteral(L"Pass_Overlay");
const Profiler::ScopeNameKey kOverlayAsyncWait = Profiler::RegisterTraceLiteral(L"Overlay Async Wait");
const Profiler::ScopeNameKey kRenderObjectBatchAsync = Profiler::RegisterTraceLiteral(L"RenderObjectBatch.Async");
const Profiler::ScopeNameKey kRenderShadowBatchAsync = Profiler::RegisterTraceLiteral(L"RenderShadowBatch.Async");
const Profiler::ScopeNameKey kCSMPerCascade = Profiler::RegisterTraceLiteral(L"CSM.PerCascade");
const Profiler::ScopeNameKey kRenderObjectBatchGpu = Profiler::RegisterTraceLiteral(L"RenderObjectBatch");
const Profiler::ScopeNameKey kRenderShadowBatchGpu = Profiler::RegisterTraceLiteral(L"RenderShadowBatch");
const Profiler::ScopeNameKey kGBufferDriver = Profiler::RegisterTraceLiteral(L"GBuffer.Driver");
const Profiler::ScopeNameKey kTransparentDriver = Profiler::RegisterTraceLiteral(L"Transparent.Driver");
const Profiler::ScopeNameKey kOceanRender = Profiler::RegisterTraceLiteral(L"OceanRenderable::RecordCompute");

const Profiler::ScopeNameKey kTextManagerBuild = Profiler::RegisterTraceLiteral(L"TextManager::Build");
const Profiler::ScopeNameKey kTextManagerDraw = Profiler::RegisterTraceLiteral(L"TextManager::Draw");
const Profiler::ScopeNameKey kTextManagerEmitImmediate = Profiler::RegisterTraceLiteral(L"TextManager::EmitTextImmediate");

const Profiler::ScopeNameKey kRenderGraphExecute = Profiler::RegisterTraceLiteral(L"RenderGraph::Execute");
const Profiler::ScopeNameKey kRenderGraphExecuteParallel = Profiler::RegisterTraceLiteral(L"RenderGraph::ExecuteParallel");

const Profiler::ScopeNameKey kService1 = Profiler::RegisterTraceLiteral(L"Service1");
const Profiler::ScopeNameKey kService2 = Profiler::RegisterTraceLiteral(L"Service2");
const Profiler::ScopeNameKey kService3 = Profiler::RegisterTraceLiteral(L"Service3");
const Profiler::ScopeNameKey kService4 = Profiler::RegisterTraceLiteral(L"Service4");

} // namespace ProfilerScopes

