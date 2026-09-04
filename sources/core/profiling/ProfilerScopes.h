#pragma once

#include "core/profiling/Profiler.h"

namespace ProfilerScopes {

// App
extern const Profiler::ScopeNameKey kWholeCycle;
extern const Profiler::ScopeNameKey kWinMessages;
// The stretch between the message pump and AppController::Tick used to be unattributed on the
// timeline — three calls with no scopes of their own.
extern const Profiler::ScopeNameKey kBeginImGuiFrame;
extern const Profiler::ScopeNameKey kProfilerTick;
extern const Profiler::ScopeNameKey kRendererTick;
extern const Profiler::ScopeNameKey kAppControllerTick;
extern const Profiler::ScopeNameKey kBuildDeveloperWindow;
extern const Profiler::ScopeNameKey kTextureDebugViewerDraw;

// Editor
extern const Profiler::ScopeNameKey kEditorDraw;
extern const Profiler::ScopeNameKey kEditorAssetRegistryPoll;
extern const Profiler::ScopeNameKey kEditorSyncSceneSelection;
extern const Profiler::ScopeNameKey kEditorPanelStateSync;
extern const Profiler::ScopeNameKey kEditorPanelStateCapture;
extern const Profiler::ScopeNameKey kEditorPanelStateBuildJson;
extern const Profiler::ScopeNameKey kEditorPanelStateSave;
extern const Profiler::ScopeNameKey kAssetRegistryRefresh;
extern const Profiler::ScopeNameKey kAssetRegistryHasChangedOnDisk;
extern const Profiler::ScopeNameKey kContentBrowserDraw;
extern const Profiler::ScopeNameKey kContentBrowserDrawSources;
extern const Profiler::ScopeNameKey kContentBrowserBuildVisibleEntries;
extern const Profiler::ScopeNameKey kContentBrowserDrawAssetView;
extern const Profiler::ScopeNameKey kAssetThumbnailRequest;
extern const Profiler::ScopeNameKey kAssetThumbnailPreflight;
extern const Profiler::ScopeNameKey kAssetThumbnailCommitPreflight;
extern const Profiler::ScopeNameKey kAssetThumbnailProcessPending;
extern const Profiler::ScopeNameKey kSceneOutlinerDraw;
extern const Profiler::ScopeNameKey kInspectorDraw;
extern const Profiler::ScopeNameKey kCommandHistoryDraw;
extern const Profiler::ScopeNameKey kViewportGizmoUpdate;
extern const Profiler::ScopeNameKey kEditorCommandExecute;
extern const Profiler::ScopeNameKey kEditorCommandUndo;
extern const Profiler::ScopeNameKey kEditorCommandRedo;
extern const Profiler::ScopeNameKey kEditorCommandMoveTo;

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
extern const Profiler::ScopeNameKey kSceneTickPointLights;
extern const Profiler::ScopeNameKey kSceneTickObjects;
extern const Profiler::ScopeNameKey kSceneTickPostObjects;
extern const Profiler::ScopeNameKey kSceneTickWind;
extern const Profiler::ScopeNameKey kSceneRender;
extern const Profiler::ScopeNameKey kPassPrologueClear;
extern const Profiler::ScopeNameKey kPassObjectCompute;
extern const Profiler::ScopeNameKey kPassGpuInstanceCompute;
extern const Profiler::ScopeNameKey kPassShoreWetness;
extern const Profiler::ScopeNameKey kAsyncEmptySubmit; // step 3: populates the second GPU track
extern const Profiler::ScopeNameKey kPassShadowCull;
extern const Profiler::ScopeNameKey kPassVsmPageRequest;
extern const Profiler::ScopeNameKey kPassVsmPageRender;
// Sub-scope of Pass_VsmPageRender: the per-page instance cull / draw-arg setup compute (splits the
// pass's cull cost from its rasterization cost — they scale with different things).
extern const Profiler::ScopeNameKey kVsmPageSetup;
// A caster-set REBUILD. It resizes casterLod_, which reads as a LOD change, which sets VSM's
// forceAll -- so one rebuild costs a full page-pool re-render. If it shows up per frame in a
// trace, that is the bug, and without this scope it is invisible: the cost lands in
// Pass_VsmPageRender's draws, nowhere near the call that caused it.
extern const Profiler::ScopeNameKey kShadowCastersRebuild;
// Sub-scope: the spatial scatter cull (clear + scatter) that feeds the clipmap pages.
extern const Profiler::ScopeNameKey kVsmPageScatter;
extern const Profiler::ScopeNameKey kVsmHzbBuild;   // occlusion plan S5b.2: the pool pyramid after pass A
extern const Profiler::ScopeNameKey kVsmHzbPost;    // occlusion plan S5b.2: the deferred-pair retest + setup B
extern const Profiler::ScopeNameKey kVsmPageDrawB;  // occlusion plan S5b.2: pass B into the pages
extern const Profiler::ScopeNameKey kPassCSM;
extern const Profiler::ScopeNameKey kPassCsmHzb;         // occlusion plan S5b
extern const Profiler::ScopeNameKey kPassShadowCullPost; // occlusion plan S5b
extern const Profiler::ScopeNameKey kPassCSMPost;        // occlusion plan S5b
extern const Profiler::ScopeNameKey kPassHzbA;           // occlusion plan S5: the pyramid of pass A's depth
extern const Profiler::ScopeNameKey kPassCamCullPost;    // occlusion plan S5: the deferred retest
extern const Profiler::ScopeNameKey kPassGBufferB;       // occlusion plan S5: pass B into the G-buffer
extern const Profiler::ScopeNameKey kPassShoreDepth;
extern const Profiler::ScopeNameKey kPassGBuffer;
extern const Profiler::ScopeNameKey kPassGtao;
extern const Profiler::ScopeNameKey kPassHzb;
extern const Profiler::ScopeNameKey kPassOcclusionQueries; // occlusion plan S3a
extern const Profiler::ScopeNameKey kPassVisTest;          // occlusion plan S3b
extern const Profiler::ScopeNameKey kGBufferIndirect;      // occlusion plan S4 (CPU record + GPU)
extern const Profiler::ScopeNameKey kPassBloom;
extern const Profiler::ScopeNameKey kPassBloomConv;
extern const Profiler::ScopeNameKey kPassLighting;
extern const Profiler::ScopeNameKey kPassSpotShadow;
extern const Profiler::ScopeNameKey kPassPointShadow;
extern const Profiler::ScopeNameKey kSpotShadowPerLight;
extern const Profiler::ScopeNameKey kPassSpotLights;
extern const Profiler::ScopeNameKey kPassPointLights;
extern const Profiler::ScopeNameKey kPassSkybox;
extern const Profiler::ScopeNameKey kPassBuildAS;
extern const Profiler::ScopeNameKey kPassReflectionSource;
extern const Profiler::ScopeNameKey kPassRTTrace;
extern const Profiler::ScopeNameKey kPassRTResolve;
extern const Profiler::ScopeNameKey kPassReflectionTemporal;
extern const Profiler::ScopeNameKey kPassReflectionBlur;
extern const Profiler::ScopeNameKey kPassCompose;
extern const Profiler::ScopeNameKey kPassRTDebug;
extern const Profiler::ScopeNameKey kPassGlassReflGbuffer;
extern const Profiler::ScopeNameKey kPassGlassReflections;
extern const Profiler::ScopeNameKey kPassTransparent;
extern const Profiler::ScopeNameKey kPassOceanReflection;
extern const Profiler::ScopeNameKey kPassDebugDraw;
extern const Profiler::ScopeNameKey kPassExposureMetering;
extern const Profiler::ScopeNameKey kPassTonemap;
extern const Profiler::ScopeNameKey kPassDlss;
// P8C-2s: the three things Pass_Tonemap does AFTER the DLSS evaluate, none of which had a scope --
// so they read as an unnamed hole at the end of the pass, which is exactly how they were found.
// The bloom is already covered by kPassBloom / kPassBloomConv; these are the rest of it.
extern const Profiler::ScopeNameKey kTonemapCurve;
extern const Profiler::ScopeNameKey kTonemapFxaa;
extern const Profiler::ScopeNameKey kTonemapResolve;
// P8C-2s: and the CPU side of the same region, which is the one that was actually unnamed. The
// bloom is RECORDED inside Pass_Tonemap -- kernel resample, six FFT dispatches, the resolve and
// the flares, each staging descriptors and allocating a constant buffer -- so on the CPU timeline
// it is a wide unlabelled block sitting after DLSS::Evaluate returns.
extern const Profiler::ScopeNameKey kTonemapBloomRecord;
extern const Profiler::ScopeNameKey kTonemapCurveRecord;
extern const Profiler::ScopeNameKey kTonemapTailRecord;
// P8C-2y: inside the bloom recording, so an optimisation has something to aim at.
extern const Profiler::ScopeNameKey kBloomRecKernel;
extern const Profiler::ScopeNameKey kBloomRecFft;
extern const Profiler::ScopeNameKey kBloomRecResolve;
extern const Profiler::ScopeNameKey kBloomRecFlares;
extern const Profiler::ScopeNameKey kPassDebug;
extern const Profiler::ScopeNameKey kFrameAsyncWait;
extern const Profiler::ScopeNameKey kPassOverlay;
extern const Profiler::ScopeNameKey kOverlayAsyncWait;
extern const Profiler::ScopeNameKey kRenderObjectBatchAsync;
extern const Profiler::ScopeNameKey kCSMPerCascade;
extern const Profiler::ScopeNameKey kRenderObjectBatchGpu;
extern const Profiler::ScopeNameKey kGBufferDriver;
// Bundle execution. A bundle CANNOT carry a timestamp query (D3D12 forbids queries in bundles),
// and the bundles are appended to the driver list at GATHER time — after the pass body already
// closed its own scope. So all bundled geometry executed inside the driver list but outside every
// scope, and read as an 82 us HOLE in the GPU trace on 122 of 123 frames. This brackets the
// ExecuteBundle calls on the driver list, which is the one place a query is legal.
extern const Profiler::ScopeNameKey kExecuteBundles;
// The editor's missing-asset scan. It was unscoped, and a trace of an inspector stall showed
// 930 ms of a 946 ms frame inside EditorController::Draw with nothing named — this is why.
extern const Profiler::ScopeNameKey kEditorAssetErrorsScan;
// The ocean SURFACE draw. One fixed scope on the one object that dominates the transparent
// batch (measured: 88% of it, ~14% of the GPU frame), so the split is visible in every trace
// at no cost — instead of a general per-object mechanism that cost 17% of the frame to run.
extern const Profiler::ScopeNameKey kOceanSurface;

extern const Profiler::ScopeNameKey kTransparentDriver;
extern const Profiler::ScopeNameKey kOceanRender;
extern const Profiler::ScopeNameKey kOceanSurfSim;
extern const Profiler::ScopeNameKey kPrepareTransparentBuckets;
extern const Profiler::ScopeNameKey kAddPass;
extern const Profiler::ScopeNameKey kPrepareViews;
extern const Profiler::ScopeNameKey kPrepareViewsSetup;
extern const Profiler::ScopeNameKey kPrepareViewsBuildList;
extern const Profiler::ScopeNameKey kPrepareQueue;
extern const Profiler::ScopeNameKey kPrepareMainView;
extern const Profiler::ScopeNameKey kPrepareViewsDispatch;
extern const Profiler::ScopeNameKey kPrepareViewsJoin;
extern const Profiler::ScopeNameKey kUpdateCascades;
extern const Profiler::ScopeNameKey kSelectShadowedSpots;
extern const Profiler::ScopeNameKey kSelectShadowedPoints;
extern const Profiler::ScopeNameKey kSceneRenderQueueBucketize;
extern const Profiler::ScopeNameKey kSceneRenderQueueBucketizeCull;
extern const Profiler::ScopeNameKey kSceneRenderQueueCull;
extern const Profiler::ScopeNameKey kSceneRenderQueueSelectLods;
extern const Profiler::ScopeNameKey kSceneRenderQueueSortOpaque;
extern const Profiler::ScopeNameKey kSceneRenderQueueSortTransparent;

// DLSS/Streamline evaluate breakdown (diagnostic).
extern const Profiler::ScopeNameKey kDlssEvaluate;
extern const Profiler::ScopeNameKey kDlssSetTagsOptions;
extern const Profiler::ScopeNameKey kDlssEvaluateFeature;
extern const Profiler::ScopeNameKey kSceneRenderQueueBuildInstancedBatchesForBucket;

// TextManager
extern const Profiler::ScopeNameKey kTextManagerBuild;
extern const Profiler::ScopeNameKey kTextManagerDraw;
extern const Profiler::ScopeNameKey kTextManagerAddText;
extern const Profiler::ScopeNameKey kTextManagerBuildGlyphRun;
extern const Profiler::ScopeNameKey kTextManagerEmitImmediate;

// RenderGraph
extern const Profiler::ScopeNameKey kRenderGraphExecute;
extern const Profiler::ScopeNameKey kRenderGraphExecuteParallel;
// Barrier plan step 7: the two-phase prologue. Both are SERIAL by construction (see
// RenderGraph::RunPrepares / CompileBarriers) and run before any body records, so they are the
// one piece of the design that could become a bottleneck without showing up anywhere else.
//
// DO NOT ADD THESE TWO TOGETHER. `CompileBarriers` is called from the tail of `RunPrepares`, so
// the Prepares row is INCLUSIVE of the CompileBarriers row — `Prepares` alone is the whole
// prologue. (Summing them once turned a 0.012 ms cost into a reported 0.022 ms.)
extern const Profiler::ScopeNameKey kRenderGraphPrepares;
extern const Profiler::ScopeNameKey kRenderGraphCompileBarriers;

//Service
extern const Profiler::ScopeNameKey kService1;
extern const Profiler::ScopeNameKey kService2;
extern const Profiler::ScopeNameKey kService3;
extern const Profiler::ScopeNameKey kService4;

} // namespace ProfilerScopes

