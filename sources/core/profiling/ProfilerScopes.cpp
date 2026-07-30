#include "core/profiling/ProfilerScopes.h"

namespace ProfilerScopes {

const Profiler::ScopeNameKey kWholeCycle = Profiler::RegisterTraceLiteral(L"Whole Cycle");
const Profiler::ScopeNameKey kWinMessages = Profiler::RegisterTraceLiteral(L"Win Messages");
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
const Profiler::ScopeNameKey kSceneRender = Profiler::RegisterTraceLiteral(L"Scene::Render");
const Profiler::ScopeNameKey kPassPrologueClear = Profiler::RegisterTraceLiteral(L"Pass_PrologueClear");
const Profiler::ScopeNameKey kPassObjectCompute = Profiler::RegisterTraceLiteral(L"Pass_ObjectCompute");
const Profiler::ScopeNameKey kPassShadowCull = Profiler::RegisterTraceLiteral(L"Pass_ShadowCull");
const Profiler::ScopeNameKey kPassVsmPageRequest = Profiler::RegisterTraceLiteral(L"Pass_VsmPageRequest");
const Profiler::ScopeNameKey kPassVsmPageRender = Profiler::RegisterTraceLiteral(L"Pass_VsmPageRender");
const Profiler::ScopeNameKey kVsmPageSetup = Profiler::RegisterTraceLiteral(L"VsmPageRender.Setup");
const Profiler::ScopeNameKey kVsmPageScatter = Profiler::RegisterTraceLiteral(L"VsmPageRender.Scatter");
const Profiler::ScopeNameKey kPassCSM = Profiler::RegisterTraceLiteral(L"Pass_CSM");
const Profiler::ScopeNameKey kPassShoreDepth = Profiler::RegisterTraceLiteral(L"Pass_ShoreDepth");
const Profiler::ScopeNameKey kPassGBuffer = Profiler::RegisterTraceLiteral(L"Pass_GBuffer");
const Profiler::ScopeNameKey kPassLighting = Profiler::RegisterTraceLiteral(L"Pass_Lighting");
const Profiler::ScopeNameKey kPassSpotShadow = Profiler::RegisterTraceLiteral(L"Pass_SpotShadows");
const Profiler::ScopeNameKey kPassPointShadow = Profiler::RegisterTraceLiteral(L"Pass_PointShadows");
const Profiler::ScopeNameKey kSpotShadowPerLight = Profiler::RegisterTraceLiteral(L"SpotShadow.PerLight");
const Profiler::ScopeNameKey kPassSpotLights = Profiler::RegisterTraceLiteral(L"Pass_SpotLights");
const Profiler::ScopeNameKey kPassPointLights = Profiler::RegisterTraceLiteral(L"Pass_PointLights");
const Profiler::ScopeNameKey kPassSkybox = Profiler::RegisterTraceLiteral(L"Pass_Skybox");
const Profiler::ScopeNameKey kPassBuildAS = Profiler::RegisterTraceLiteral(L"Pass_BuildAS");
const Profiler::ScopeNameKey kPassReflectionSource = Profiler::RegisterTraceLiteral(L"Pass_ReflectionSource");
const Profiler::ScopeNameKey kPassRTReflections = Profiler::RegisterTraceLiteral(L"Pass_RTReflections");
const Profiler::ScopeNameKey kPassRTDenoise = Profiler::RegisterTraceLiteral(L"Pass_RTDenoise");
const Profiler::ScopeNameKey kPassReflectionBlur = Profiler::RegisterTraceLiteral(L"Pass_Reflection.Blur");
const Profiler::ScopeNameKey kPassCompose = Profiler::RegisterTraceLiteral(L"Pass_Compose");
const Profiler::ScopeNameKey kPassRTDebug = Profiler::RegisterTraceLiteral(L"Pass_RTDebug");
const Profiler::ScopeNameKey kPassGlassReflGbuffer = Profiler::RegisterTraceLiteral(L"Pass_GlassReflGbuffer");
const Profiler::ScopeNameKey kPassGlassReflections = Profiler::RegisterTraceLiteral(L"Pass_GlassReflections");
const Profiler::ScopeNameKey kPassTransparent = Profiler::RegisterTraceLiteral(L"Pass_Transparent");
const Profiler::ScopeNameKey kPassOceanReflection = Profiler::RegisterTraceLiteral(L"Pass_OceanReflection");
const Profiler::ScopeNameKey kPassDebugDraw = Profiler::RegisterTraceLiteral(L"Pass_DebugDraw");
const Profiler::ScopeNameKey kPassTonemap = Profiler::RegisterTraceLiteral(L"Pass_Tonemap");
const Profiler::ScopeNameKey kPassDebug = Profiler::RegisterTraceLiteral(L"Pass_Debug");
const Profiler::ScopeNameKey kFrameAsyncWait = Profiler::RegisterTraceLiteral(L"Frame Async Wait");
const Profiler::ScopeNameKey kPassOverlay = Profiler::RegisterTraceLiteral(L"Pass_Overlay");
const Profiler::ScopeNameKey kOverlayAsyncWait = Profiler::RegisterTraceLiteral(L"Overlay Async Wait");
const Profiler::ScopeNameKey kRenderObjectBatchAsync = Profiler::RegisterTraceLiteral(L"RenderObjectBatch.Async");
const Profiler::ScopeNameKey kCSMPerCascade = Profiler::RegisterTraceLiteral(L"CSM.PerCascade");
const Profiler::ScopeNameKey kRenderObjectBatchGpu = Profiler::RegisterTraceLiteral(L"RenderObjectBatch");
const Profiler::ScopeNameKey kGBufferDriver = Profiler::RegisterTraceLiteral(L"GBuffer.Driver");
const Profiler::ScopeNameKey kTransparentDriver = Profiler::RegisterTraceLiteral(L"Transparent.Driver");
const Profiler::ScopeNameKey kOceanRender = Profiler::RegisterTraceLiteral(L"OceanRenderable::RecordCompute");
const Profiler::ScopeNameKey kPrepareTransparentBuckets = Profiler::RegisterTraceLiteral(L"Scene::PrepareTransparentBuckets");
const Profiler::ScopeNameKey kAddPass = Profiler::RegisterTraceLiteral(L"RenderGraph::AddPass");
const Profiler::ScopeNameKey kPrepareViews = Profiler::RegisterTraceLiteral(L"Scene::PrepareViews");
const Profiler::ScopeNameKey kPrepareQueue = Profiler::RegisterTraceLiteral(L"Scene::prepareQueue");
const Profiler::ScopeNameKey kUpdateCascades = Profiler::RegisterTraceLiteral(L"Scene::UpdateCascades");
const Profiler::ScopeNameKey kSelectShadowedSpots = Profiler::RegisterTraceLiteral(L"LightManager::SelectShadowedSpots");
const Profiler::ScopeNameKey kSelectShadowedPoints = Profiler::RegisterTraceLiteral(L"LightManager::SelectShadowedPoints");
const Profiler::ScopeNameKey kSceneRenderQueueBucketize = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::Bucketize");
const Profiler::ScopeNameKey kSceneRenderQueueBucketizeCull = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::BucketizeCull");
const Profiler::ScopeNameKey kSceneRenderQueueCull = Profiler::RegisterTraceLiteral(L"SceneRenderQueue::Cull");
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

const Profiler::ScopeNameKey kService1 = Profiler::RegisterTraceLiteral(L"Service1");
const Profiler::ScopeNameKey kService2 = Profiler::RegisterTraceLiteral(L"Service2");
const Profiler::ScopeNameKey kService3 = Profiler::RegisterTraceLiteral(L"Service3");
const Profiler::ScopeNameKey kService4 = Profiler::RegisterTraceLiteral(L"Service4");

} // namespace ProfilerScopes

