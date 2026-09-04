#include "core/profiling/ProfilerScopes.h"

namespace ProfilerScopes {

const Profiler::ScopeNameKey kWholeCycle = Profiler::RegisterTraceLiteral(L"Whole Cycle");
const Profiler::ScopeNameKey kWinMessages = Profiler::RegisterTraceLiteral(L"Win Messages");
const Profiler::ScopeNameKey kBeginImGuiFrame = Profiler::RegisterTraceLiteral(L"Renderer::BeginImGuiFrame");
const Profiler::ScopeNameKey kProfilerTick = Profiler::RegisterTraceLiteral(L"Profiler::Tick");
const Profiler::ScopeNameKey kRendererTick = Profiler::RegisterTraceLiteral(L"Renderer::Tick");
const Profiler::ScopeNameKey kAppControllerTick = Profiler::RegisterTraceLiteral(L"AppController::Tick");
const Profiler::ScopeNameKey kBuildDeveloperWindow = Profiler::RegisterTraceLiteral(L"DeveloperWindow::Draw");
const Profiler::ScopeNameKey kTextureDebugViewerDraw = Profiler::RegisterTraceLiteral(L"TextureDebugViewer::Draw");

const Profiler::ScopeNameKey kEditorDraw = Profiler::RegisterTraceLiteral(L"EditorController::Draw");
const Profiler::ScopeNameKey kEditorAssetRegistryPoll = Profiler::RegisterTraceLiteral(L"EditorController::AssetRegistryPoll");
const Profiler::ScopeNameKey kEditorSyncSceneSelection = Profiler::RegisterTraceLiteral(L"EditorController::SyncSceneSelection");
const Profiler::ScopeNameKey kEditorPanelStateSync = Profiler::RegisterTraceLiteral(L"EditorController::PanelStateSync");
const Profiler::ScopeNameKey kEditorPanelStateCapture = Profiler::RegisterTraceLiteral(L"EditorController::PanelStateCapture");
const Profiler::ScopeNameKey kEditorPanelStateBuildJson = Profiler::RegisterTraceLiteral(L"EditorController::PanelStateBuildJson");
const Profiler::ScopeNameKey kEditorPanelStateSave = Profiler::RegisterTraceLiteral(L"EditorController::PanelStateSave");
const Profiler::ScopeNameKey kAssetRegistryRefresh = Profiler::RegisterTraceLiteral(L"AssetRegistry::Refresh");
const Profiler::ScopeNameKey kAssetRegistryHasChangedOnDisk = Profiler::RegisterTraceLiteral(L"AssetRegistry::HasChangedOnDisk");
const Profiler::ScopeNameKey kContentBrowserDraw = Profiler::RegisterTraceLiteral(L"ContentBrowserPanel::Draw");
const Profiler::ScopeNameKey kContentBrowserDrawSources = Profiler::RegisterTraceLiteral(L"ContentBrowserPanel::DrawSources");
const Profiler::ScopeNameKey kContentBrowserBuildVisibleEntries = Profiler::RegisterTraceLiteral(L"ContentBrowserPanel::BuildVisibleEntries");
const Profiler::ScopeNameKey kContentBrowserDrawAssetView = Profiler::RegisterTraceLiteral(L"ContentBrowserPanel::DrawAssetView");
const Profiler::ScopeNameKey kAssetThumbnailRequest = Profiler::RegisterTraceLiteral(L"AssetThumbnailCache::Request");
const Profiler::ScopeNameKey kAssetThumbnailPreflight = Profiler::RegisterTraceLiteral(L"AssetThumbnailCache::Preflight");
const Profiler::ScopeNameKey kAssetThumbnailCommitPreflight = Profiler::RegisterTraceLiteral(L"AssetThumbnailCache::CommitPreflight");
const Profiler::ScopeNameKey kAssetThumbnailProcessPending = Profiler::RegisterTraceLiteral(L"AssetThumbnailCache::ProcessPending");
const Profiler::ScopeNameKey kSceneOutlinerDraw = Profiler::RegisterTraceLiteral(L"SceneOutlinerPanel::Draw");
const Profiler::ScopeNameKey kInspectorDraw = Profiler::RegisterTraceLiteral(L"InspectorPanel::Draw");
const Profiler::ScopeNameKey kCommandHistoryDraw = Profiler::RegisterTraceLiteral(L"CommandHistoryPanel::Draw");
const Profiler::ScopeNameKey kViewportGizmoUpdate = Profiler::RegisterTraceLiteral(L"ViewportGizmo::Update");
const Profiler::ScopeNameKey kEditorCommandExecute = Profiler::RegisterTraceLiteral(L"EditorCommandStack::Execute");
const Profiler::ScopeNameKey kEditorCommandUndo = Profiler::RegisterTraceLiteral(L"EditorCommandStack::Undo");
const Profiler::ScopeNameKey kEditorCommandRedo = Profiler::RegisterTraceLiteral(L"EditorCommandStack::Redo");
const Profiler::ScopeNameKey kEditorCommandMoveTo = Profiler::RegisterTraceLiteral(L"EditorCommandStack::MoveTo");

const Profiler::ScopeNameKey kMaterialFSProbe = Profiler::RegisterTraceLiteral(L"Material::FSProbeAndFlagPending");

const Profiler::ScopeNameKey kRendererWaitForFrame = Profiler::RegisterTraceLiteral(L"Renderer::WaitForFrame");
const Profiler::ScopeNameKey kRendererBeginFrame = Profiler::RegisterTraceLiteral(L"Renderer::BeginFrame");
const Profiler::ScopeNameKey kRendererBeginThreadCommandList = Profiler::RegisterTraceLiteral(L"Renderer::BeginThreadCommandList");
const Profiler::ScopeNameKey kRendererEndThreadCommandList = Profiler::RegisterTraceLiteral(L"Renderer::EndThreadCommandList");
const Profiler::ScopeNameKey kRendererEndThreadCommandBundle = Profiler::RegisterTraceLiteral(L"Renderer::EndThreadCommandBundle");
const Profiler::ScopeNameKey kRendererExecuteTimelineAndPresent = Profiler::RegisterTraceLiteral(L"Renderer::ExecuteTimelineAndPresent");
const Profiler::ScopeNameKey kRendererTransition = Profiler::RegisterTraceLiteral(L"Renderer::Transition");

const Profiler::ScopeNameKey kSceneTick = Profiler::RegisterTraceLiteral(L"Scene::Tick");
const Profiler::ScopeNameKey kSceneTickPointLights = Profiler::RegisterTraceLiteral(L"Scene::Tick.PointLights");
const Profiler::ScopeNameKey kSceneTickObjects = Profiler::RegisterTraceLiteral(L"Scene::Tick.Objects");
const Profiler::ScopeNameKey kSceneTickPostObjects = Profiler::RegisterTraceLiteral(L"Scene::Tick.PostObjects");
const Profiler::ScopeNameKey kSceneTickWind = Profiler::RegisterTraceLiteral(L"Scene::Tick.Wind");
const Profiler::ScopeNameKey kSceneRender = Profiler::RegisterTraceLiteral(L"Scene::Render");
const Profiler::ScopeNameKey kPassPrologueClear = Profiler::RegisterTraceLiteral(L"Pass_PrologueClear");
const Profiler::ScopeNameKey kPassObjectCompute = Profiler::RegisterTraceLiteral(L"Pass_ObjectCompute");
const Profiler::ScopeNameKey kPassGpuInstanceCompute = Profiler::RegisterTraceLiteral(L"Pass_GpuInstanceCompute");
// Step 9: the wetness pass had NO GPU scope, so its cost was invisible in every trace.
const Profiler::ScopeNameKey kPassShoreWetness = Profiler::RegisterTraceLiteral(L"Pass_ShoreWetness");
// Async-compute step 3: the scope around step 2's empty compute submission. Its only job is to put
// a real timestamped event on the SECOND trace row, so the row proves the compute queue's
// calibration, frequency and drain fence work — an empty row would prove nothing.
const Profiler::ScopeNameKey kAsyncEmptySubmit = Profiler::RegisterTraceLiteral(L"Async.EmptySubmit");
const Profiler::ScopeNameKey kPassShadowCull = Profiler::RegisterTraceLiteral(L"Pass_ShadowCull");
const Profiler::ScopeNameKey kPassVsmPageRequest = Profiler::RegisterTraceLiteral(L"Pass_VsmPageRequest");
const Profiler::ScopeNameKey kPassVsmPageRender = Profiler::RegisterTraceLiteral(L"Pass_VsmPageRender");
const Profiler::ScopeNameKey kVsmPageSetup = Profiler::RegisterTraceLiteral(L"VsmPageRender.Setup");
const Profiler::ScopeNameKey kShadowCastersRebuild = Profiler::RegisterTraceLiteral(L"ShadowGpuData::Rebuild");
const Profiler::ScopeNameKey kVsmPageScatter = Profiler::RegisterTraceLiteral(L"VsmPageRender.Scatter");
const Profiler::ScopeNameKey kPassCSM = Profiler::RegisterTraceLiteral(L"Pass_CSM");
const Profiler::ScopeNameKey kPassShoreDepth = Profiler::RegisterTraceLiteral(L"Pass_ShoreDepth");
const Profiler::ScopeNameKey kPassGBuffer = Profiler::RegisterTraceLiteral(L"Pass_GBuffer");
const Profiler::ScopeNameKey kPassGtao = Profiler::RegisterTraceLiteral(L"Pass_Gtao");
const Profiler::ScopeNameKey kPassHzb = Profiler::RegisterTraceLiteral(L"Pass_Hzb");
const Profiler::ScopeNameKey kPassOcclusionQueries = Profiler::RegisterTraceLiteral(L"Pass_OcclusionQueries");
const Profiler::ScopeNameKey kPassVisTest = Profiler::RegisterTraceLiteral(L"Pass_VisTest");
// P8: recorded inside Pass_Tonemap, so it needs its own scope to be readable at all --
// Pass_Tonemap is already dominated by the DLSS evaluate (see the note in that pass).
const Profiler::ScopeNameKey kPassBloom = Profiler::RegisterTraceLiteral(L"Pass_Bloom");
const Profiler::ScopeNameKey kPassBloomConv = Profiler::RegisterTraceLiteral(L"Pass_BloomConv");
const Profiler::ScopeNameKey kPassLighting = Profiler::RegisterTraceLiteral(L"Pass_Lighting");
const Profiler::ScopeNameKey kPassSpotShadow = Profiler::RegisterTraceLiteral(L"Pass_SpotShadows");
const Profiler::ScopeNameKey kPassPointShadow = Profiler::RegisterTraceLiteral(L"Pass_PointShadows");
const Profiler::ScopeNameKey kSpotShadowPerLight = Profiler::RegisterTraceLiteral(L"SpotShadow.PerLight");
const Profiler::ScopeNameKey kPassSpotLights = Profiler::RegisterTraceLiteral(L"Pass_SpotLights");
const Profiler::ScopeNameKey kPassPointLights = Profiler::RegisterTraceLiteral(L"Pass_PointLights");
const Profiler::ScopeNameKey kPassSkybox = Profiler::RegisterTraceLiteral(L"Pass_Skybox");
const Profiler::ScopeNameKey kPassBuildAS = Profiler::RegisterTraceLiteral(L"Pass_BuildAS");
const Profiler::ScopeNameKey kPassReflectionSource = Profiler::RegisterTraceLiteral(L"Pass_ReflectionSource");
const Profiler::ScopeNameKey kPassRTTrace = Profiler::RegisterTraceLiteral(L"Pass_RTTrace");
const Profiler::ScopeNameKey kPassRTResolve = Profiler::RegisterTraceLiteral(L"Pass_RTResolve");
const Profiler::ScopeNameKey kPassReflectionTemporal = Profiler::RegisterTraceLiteral(L"Pass_Reflection.Temporal");
const Profiler::ScopeNameKey kPassReflectionBlur = Profiler::RegisterTraceLiteral(L"Pass_Reflection.Blur");
const Profiler::ScopeNameKey kPassCompose = Profiler::RegisterTraceLiteral(L"Pass_Compose");
const Profiler::ScopeNameKey kPassRTDebug = Profiler::RegisterTraceLiteral(L"Pass_RTDebug");
const Profiler::ScopeNameKey kPassGlassReflGbuffer = Profiler::RegisterTraceLiteral(L"Pass_GlassReflGbuffer");
const Profiler::ScopeNameKey kPassGlassReflections = Profiler::RegisterTraceLiteral(L"Pass_GlassReflections");
const Profiler::ScopeNameKey kPassTransparent = Profiler::RegisterTraceLiteral(L"Pass_Transparent");
const Profiler::ScopeNameKey kPassOceanReflection = Profiler::RegisterTraceLiteral(L"Pass_OceanReflection");
const Profiler::ScopeNameKey kPassDebugDraw = Profiler::RegisterTraceLiteral(L"Pass_DebugDraw");
const Profiler::ScopeNameKey kPassExposureMetering = Profiler::RegisterTraceLiteral(L"Pass_ExposureMetering");
const Profiler::ScopeNameKey kPassTonemap = Profiler::RegisterTraceLiteral(L"Pass_Tonemap");
const Profiler::ScopeNameKey kPassDlss = Profiler::RegisterTraceLiteral(L"Pass_DLSS");
// P8C-2s: Pass_Tonemap's own work, which is everything after the DLSS evaluate it is named for.
const Profiler::ScopeNameKey kTonemapCurve = Profiler::RegisterTraceLiteral(L"Tonemap.Curve");
const Profiler::ScopeNameKey kTonemapFxaa = Profiler::RegisterTraceLiteral(L"Tonemap.Fxaa");
const Profiler::ScopeNameKey kTonemapResolve = Profiler::RegisterTraceLiteral(L"Tonemap.Resolve");
const Profiler::ScopeNameKey kTonemapBloomRecord = Profiler::RegisterTraceLiteral(L"Tonemap.RecordBloom");
const Profiler::ScopeNameKey kTonemapCurveRecord = Profiler::RegisterTraceLiteral(L"Tonemap.RecordCurve");
const Profiler::ScopeNameKey kTonemapTailRecord = Profiler::RegisterTraceLiteral(L"Tonemap.RecordTail");
const Profiler::ScopeNameKey kBloomRecKernel = Profiler::RegisterTraceLiteral(L"BloomRec.Kernel");
const Profiler::ScopeNameKey kBloomRecFft = Profiler::RegisterTraceLiteral(L"BloomRec.Fft");
const Profiler::ScopeNameKey kBloomRecResolve = Profiler::RegisterTraceLiteral(L"BloomRec.Resolve");
const Profiler::ScopeNameKey kBloomRecFlares = Profiler::RegisterTraceLiteral(L"BloomRec.Flares");
const Profiler::ScopeNameKey kPassDebug = Profiler::RegisterTraceLiteral(L"Pass_Debug");
const Profiler::ScopeNameKey kFrameAsyncWait = Profiler::RegisterTraceLiteral(L"Frame Async Wait");
const Profiler::ScopeNameKey kPassOverlay = Profiler::RegisterTraceLiteral(L"Pass_Overlay");
const Profiler::ScopeNameKey kOverlayAsyncWait = Profiler::RegisterTraceLiteral(L"Overlay Async Wait");
const Profiler::ScopeNameKey kRenderObjectBatchAsync = Profiler::RegisterTraceLiteral(L"RenderObjectBatch.Async");
const Profiler::ScopeNameKey kCSMPerCascade = Profiler::RegisterTraceLiteral(L"CSM.PerCascade");
const Profiler::ScopeNameKey kRenderObjectBatchGpu = Profiler::RegisterTraceLiteral(L"RenderObjectBatch");
const Profiler::ScopeNameKey kGBufferDriver = Profiler::RegisterTraceLiteral(L"GBuffer.Driver");
const Profiler::ScopeNameKey kExecuteBundles = Profiler::RegisterTraceLiteral(L"ExecuteBundles");
const Profiler::ScopeNameKey kEditorAssetErrorsScan = Profiler::RegisterTraceLiteral(L"Editor.AssetErrorsScan");
const Profiler::ScopeNameKey kOceanSurface = Profiler::RegisterTraceLiteral(L"Ocean.Surface");
const Profiler::ScopeNameKey kTransparentDriver = Profiler::RegisterTraceLiteral(L"Transparent.Driver");
const Profiler::ScopeNameKey kOceanRender = Profiler::RegisterTraceLiteral(L"OceanRenderable::RecordCompute");
const Profiler::ScopeNameKey kOceanSurfSim = Profiler::RegisterTraceLiteral(L"Ocean.SurfSim");
const Profiler::ScopeNameKey kPrepareTransparentBuckets = Profiler::RegisterTraceLiteral(L"Scene::PrepareTransparentBuckets");
const Profiler::ScopeNameKey kAddPass = Profiler::RegisterTraceLiteral(L"RenderGraph::AddPass");
const Profiler::ScopeNameKey kPrepareViews = Profiler::RegisterTraceLiteral(L"Scene::PrepareViews");
const Profiler::ScopeNameKey kPrepareViewsSetup = Profiler::RegisterTraceLiteral(L"Scene::PrepareViewsSetup");
const Profiler::ScopeNameKey kPrepareViewsBuildList = Profiler::RegisterTraceLiteral(L"Scene::PrepareViewsBuildList");
const Profiler::ScopeNameKey kPrepareQueue = Profiler::RegisterTraceLiteral(L"Scene::prepareQueue");
const Profiler::ScopeNameKey kPrepareMainView = Profiler::RegisterTraceLiteral(L"Scene::PrepareMainView");
const Profiler::ScopeNameKey kPrepareViewsDispatch = Profiler::RegisterTraceLiteral(L"Scene::PrepareViewsDispatch");
const Profiler::ScopeNameKey kPrepareViewsJoin = Profiler::RegisterTraceLiteral(L"Scene::PrepareViewsJoin");
const Profiler::ScopeNameKey kUpdateCascades = Profiler::RegisterTraceLiteral(L"Scene::UpdateCascades");
const Profiler::ScopeNameKey kSelectShadowedSpots = Profiler::RegisterTraceLiteral(L"LightManager::SelectShadowedSpots");
const Profiler::ScopeNameKey kSelectShadowedPoints = Profiler::RegisterTraceLiteral(L"LightManager::SelectShadowedPoints");
const Profiler::ScopeNameKey kSceneRenderQueueBucketize = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::Bucketize");
const Profiler::ScopeNameKey kSceneRenderQueueBucketizeCull = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::BucketizeCull");
const Profiler::ScopeNameKey kSceneRenderQueueCull = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::Cull");
const Profiler::ScopeNameKey kSceneRenderQueueSelectLods = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::SelectLods");
const Profiler::ScopeNameKey kSceneRenderQueueSortOpaque = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::SortOpaque");
const Profiler::ScopeNameKey kSceneRenderQueueSortTransparent = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::SortTransparent");
const Profiler::ScopeNameKey kDlssEvaluate = Profiler::RegisterTraceLiteral(L"DLSS::Evaluate");
const Profiler::ScopeNameKey kDlssSetTagsOptions = Profiler::RegisterTraceLiteral(L"DLSS::SetTagsOptions");
const Profiler::ScopeNameKey kDlssEvaluateFeature = Profiler::RegisterTraceLiteral(L"DLSS::slEvaluateFeature");
const Profiler::ScopeNameKey kSceneRenderQueueBuildInstancedBatchesForBucket = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::BuildInstancedBatchesForBucket");

const Profiler::ScopeNameKey kTextManagerBuild = Profiler::RegisterTraceLiteral(L"TextManager::Build");
const Profiler::ScopeNameKey kTextManagerDraw = Profiler::RegisterTraceLiteral(L"TextManager::Draw");
const Profiler::ScopeNameKey kTextManagerAddText = Profiler::RegisterTraceLiteral(L"TextManager::AddText");
const Profiler::ScopeNameKey kTextManagerBuildGlyphRun = Profiler::RegisterTraceLiteral(L"TextManager::BuildGlyphRun");
const Profiler::ScopeNameKey kTextManagerEmitImmediate = Profiler::RegisterTraceLiteral(L"TextManager::EmitTextImmediate");

const Profiler::ScopeNameKey kRenderGraphExecute = Profiler::RegisterTraceLiteral(L"RenderGraph::Execute");
const Profiler::ScopeNameKey kRenderGraphExecuteParallel = Profiler::RegisterTraceLiteral(L"RenderGraph::ExecuteParallel");
const Profiler::ScopeNameKey kRenderGraphPrepares = Profiler::RegisterTraceLiteral(L"RenderGraph::Prepares");
const Profiler::ScopeNameKey kRenderGraphCompileBarriers = Profiler::RegisterTraceLiteral(L"RenderGraph::CompileBarriers");

const Profiler::ScopeNameKey kService1 = Profiler::RegisterTraceLiteral(L"Service1");
const Profiler::ScopeNameKey kService2 = Profiler::RegisterTraceLiteral(L"Service2");
const Profiler::ScopeNameKey kService3 = Profiler::RegisterTraceLiteral(L"Service3");
const Profiler::ScopeNameKey kService4 = Profiler::RegisterTraceLiteral(L"Service4");

} // namespace ProfilerScopes

