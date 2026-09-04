// R5 (docs/scene_renderer_refactor_plan.md): the render graph's construction, one function per
// phase, in schedule order.
//
// These were ~1400 lines inside Render(), which made the frame's SHAPE -- what depends on what,
// where the CL groups start and end, which passes a setting removes entirely -- readable only by
// scrolling. Nothing here decides anything: DecideFrame settled the frame before the first pass was
// registered, and every AddPass2 builder runs later still, from ExecuteParallel. A phase is pure
// registration, so the only thing it must preserve is ORDER.
//
// GraphBuild carries the indices that cross a phase boundary; an index used inside one phase stays
// a local. Each phase opens the aliases the moved code already spelled (`rg`, `D`, `P`) so the
// bodies are unchanged to the character.
//
// THE CL GROUPS DO NOT STRADDLE A PHASE. The compute group opens and closes inside BuildPrologue,
// the reflection group inside BuildReflections, the tonemap group inside BuildPost. That is a
// correctness constraint, not tidiness: BeginCLGroup/EndCLGroup bracket a shared command list, and
// a phase boundary in the middle of one would put its members' recording in two different places.

#include "app/scene/SceneRenderer.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>

#include "app/camera/Camera.h"
#include "app/Systems.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/task/TaskSystem.h"
#include "ocean/OceanRenderable.h"
#include "ocean/OceanSimulation.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
#include "rendering/core/Renderer.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/shadows/ShadowGpuData.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/shadows/ShadowSettings.h"
#include "vfx/WindState.h"

#include "app/scene/SceneRenderInternal.h"
#include "rendering/visibility/OcclusionHistory.h" // occlusion plan S3a: the query plan the pass draws
#include "rendering/visibility/HzbOcclusionTester.h" // occlusion plan S3b: the tester Main_VisTest runs
using namespace scene_internal;

namespace
{
    // Every pass that binds a texture for reading asks for BOTH shader-visible states:
    // the same target is read from pixel shaders and from compute in the same frame, and
    // one combined state is one barrier instead of a flip between them.
    constexpr D3D12_RESOURCE_STATES kSrvAll =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

// AS build, prologue clear, object compute, surf sim, shore wetness, terrain depth.
void SceneRenderer::BuildPrologue(Renderer* renderer, GraphBuild& gb)
{
    auto& rg = gb.rg;

    // RT acceleration-structure build (S5): the first pass when RT is enabled.
    // No consumer yet, so it's an independent node (no prereqs/dependents); a
    // future RT reflections pass (S7) will depend on it. The pass declares no
    // resource states and never transitions the AS buffers, so they bypass the
    // the barrier compile entirely and stay in RAYTRACING_ACCELERATION_STRUCTURE.
    gb.pBuildAS = (size_t)-1;
    if (decisions_.rtBuildAS)
    {
        // pass-flow S5: AddPass2, though this pass declares NOTHING — measured: it performs no
        // transitions (the AS build bypasses the barrier compile entirely). The builder is the
        // whole registration, so there is no second lambda that could start declaring behind the
        // body's back.
        // ASYNC COMPUTE (step 10). The cleanest mover in the frame, and the one that tests a
        // different WORKLOAD character than RTTrace did:
        //   - it declares NOTHING (the AS buffers bypass the barrier compile entirely), so it has
        //     no D7 hand-over problem at all — the class of blocker that stopped the ocean sim;
        //   - it is the FIRST pass of the frame and its only consumer is Main_RTTrace, so the slack
        //     is ~1 ms;
        //   - its cross-queue edge comes from Main_RTTrace's explicit mtDep on it, NOT from any
        //     resource declaration. That mtDep is load-bearing (R14) and must not be tidied away.
        // An acceleration-structure build is more fixed-function than the bandwidth-bound compute
        // RTTrace runs, which is exactly why it is worth measuring separately.
        gb.pBuildAS = rg.AddPass2(RenderPass::Main_BuildAS, RenderQueue::AsyncCompute, {},
            [this, renderer](RenderGraphPassContext&) -> std::function<void(RenderGraphPassContext)> {
                return [this, renderer](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassBuildAS);
                    rtAs_.Build(renderer, c, *frame_, resources_.GetRtWindDeformMaterial().get());
                };
            });
    }

    // CL group (step 5): the prologue clear and the object-compute dispatches are
    // two tiny back-to-back lists with no mtDeps; share one command list.
    rg.BeginCLGroup();
    // pass-flow S5: the clear itself performs no transitions (measured), so the only declarations
    // here are for resources this pass does not touch — see below.
    //
    // ASYNC COMPUTE — D7's RELEASE half for Main_ObjectCompute, and the thing whose absence kept
    // that pass on the graphics queue through step 10.
    //
    // The ocean's maps go round a cycle that crosses the queue boundary twice per frame: the sim
    // writes them (compute), the ocean surface samples them in a PIXEL shader at the end of the
    // frame (graphics), and the next frame's sim takes them again. The acquire half has always had
    // a home — OceanRenderable::PrepareRender adds the PIXEL bit on the graphics queue. The release
    // half did not: stripping that bit has to happen on the graphics queue too, and the transparent
    // pass fans out over several command lists, so it cannot ride a barrier point there.
    //
    // It rides one HERE instead. This pass is the frame's first, it is on the graphics queue, and
    // Main_ObjectCompute depends on it directly — which is exactly the shape Main_Hzb has for
    // Main_RTTrace. One transition per map, in a pass that was already being recorded.
    auto pClear = rg.AddPass2(RenderPass::Main_PrologueClear, {},
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            // The point is taken UNCONDITIONALLY, so the pass has one shape whether or not the
            // level has an ocean; with no maps to hand over the point is empty and the emit is a
            // no-op. A barrier set that appears and disappears with the level is the kind of thing
            // that works everywhere except the one scene nobody captures.
            ctx.NextPoint();
            const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            if (OceanSimulation* oceanSim = Systems::GetOceanSimulation())
            {
                // Guarded per resource exactly as the sim's own Prepare guards them: registering a
                // transition the compile then cannot match is fatal under compiled barriers.
                constexpr D3D12_RESOURCE_STATES kComputeLegal =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                if (ID3D12Resource* d = oceanSim->GetDisplacementResource()) { ctx.Use(d, kComputeLegal); }
                if (ID3D12Resource* p = oceanSim->GetPreviousDisplacementResource()) { ctx.Use(p, kComputeLegal); }
                if (ID3D12Resource* f = oceanSim->GetFoamResource()) { ctx.Use(f, kComputeLegal); }
            }
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassPrologueClear);
                Pass_PrologueClear(renderer, c, point);
            };
        });

    // pass-flow S7b: the builder walks the scene ONCE and collects the objects whose compute will
    // actually record; the body runs that list instead of re-walking and re-filtering. The two
    // walks agreeing was previously a matter of the two filters staying identical by hand.
    //
    // Async-compute step 9: ONE builder, TWO passes, split on `ComputeFeedsShadowCull()`. The two
    // halves have nothing in common but the walk: the GI rotation is read by Main_ShadowCull two
    // passes later (no slack, never moves), while the ocean sim and the particles are not read
    // until Main_Transparent at the end of the frame. Sharing the lambda keeps the ONE walk and
    // the ONE overflow rule; only the predicate differs.
    auto computeBuilder = [this, renderer](bool feedsShadowCull, Profiler::ScopeNameKey scope) {
        return [this, renderer, feedsShadowCull, scope](RenderGraphPassContext& ctx)
            -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->objects) { return {}; }
            ObjectComputeList list;
            for (const auto& obj : *frame_->objects)
            {
                if (!obj) { continue; }
                if (obj->ComputeFeedsShadowCull() != feedsShadowCull) { continue; }
                if (!obj->PrepareCompute(ctx)) { continue; }
                if (list.size() >= list.capacity())
                {
                    // The cap is a budget, not a guess: it must not silently drop work.
                    RendererInvariantFailure("object compute: list overflow (raise kMaxComputeObjects)");
                    break;
                }
                list.push_back(obj.get());
            }
            if (list.empty()) { return {}; }
            return [this, renderer, list, scope](RenderGraphPassContext c) {
                CPU_SCOPE(scope);
                Pass_ObjectCompute(renderer, c, list, scope);
            };
        };
    };

    // The GI rotation. Stays on the graphics queue permanently — Main_ShadowCull's scatter reads
    // the very buffer this writes, so there is no window to hide it in.
    auto pGiCompute = rg.AddPass2(RenderPass::Main_GpuInstanceCompute, { pClear },
        computeBuilder(/*feedsShadowCull=*/true, ProfilerScopes::kPassGpuInstanceCompute));
    rg.EndCLGroup();

    // Ocean sim + particles. OUT of the CL group (a grouped pass cannot change queue), which is the
    // +1 command list the split costs.
    //
    // ASYNC COMPUTE. Step 10 left this on the graphics queue because D7's release half had no home
    // and the compile refused the move by name (`Ocean.PrevDisplacement` in 0xC0). Both halves now
    // exist, and neither is a special case bolted onto this pass:
    //   - the sim stopped parking its maps in NON_PIXEL|PIXEL, a state it set only for a consumer
    //     several passes away. It leaves them NON_PIXEL, which is legal on both queues.
    //   - the consumer that actually samples them in a pixel shader acquires the bit itself
    //     (OceanRenderable::PrepareRender, graphics queue) — including the foam map, which was
    //     being read UNDECLARED and only worked because the sim pre-set its state.
    //   - Main_PrologueClear strips the bit again at the top of the next frame, on the graphics
    //     queue, in a pass this one depends on directly. That is the release.
    // The prereq on `pClear` is therefore LOAD-BEARING and not redundant with pGiCompute: it is
    // both the hand-over and the fence edge (a cross-queue wait comes from prereqs and mtDeps
    // alike — see RenderGraph's batch walk).
    gb.pObjectCompute = rg.AddPass2(RenderPass::Main_ObjectCompute, RenderQueue::AsyncCompute,
        { pClear, pGiCompute },
        computeBuilder(/*feedsShadowCull=*/false, ProfilerScopes::kPassObjectCompute));

    // The surf sim and the wetness update follow the ocean sim and are grouped with each other:
    // both are tiny, both are ocean, and neither is an async candidate on its own. SurfSim is the
    // group's FIRST member, which is what lets it keep an outside prereq (BeginCLGroup's contract).
    rg.BeginCLGroup();
    // surf sim injection (pass-flow S3 pilot): the surf sim as its OWN pass, authored with
    // AddPass2 — the builder makes the frame's decisions, declares from them and returns the
    // record lambda; there is no separate Prepare to mirror. Third member of the compute CL
    // group, so it records into the same command list right after the FFT dispatches.
    // NOT the ocean sim: the surf sim touches none of the FFT sim's maps (it owns its own
    // wave/foam/spawner textures and reads the shore maps), so that prereq was ordering inherited
    // from when all of this was one graphics chain. Keeping it made the GRAPHICS queue wait for the
    // compute queue at the very top of the frame — measured: Pass_ObjectCompute overlapped exactly
    // 0% of anything until this arc and Main_TerrainDepth's were repointed.
    const size_t pSurfSim = rg.AddPass2(RenderPass::Main_SurfSim, { pGiCompute },
        [this](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->ocean) { return {}; }
            return frame_->ocean->BuildSurfSimPass(ctx);
        });
    gb.pWetness = rg.AddPass2(RenderPass::Main_ShoreWetness, { pSurfSim },
        [this](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->ocean) { return {}; }
            return frame_->ocean->BuildWetnessPass(ctx);
        });
    rg.EndCLGroup();

    // pass-flow S6: the two OceanSimulation flags were read by the Prepare and read AGAIN by the
    // body; now the builder decides both once, validates every target AND its DSV (the body's
    // renderCascade used to skip on a null DSV, after the pass had declared for it), and commits
    // the cross-frame `shoreSdfDirty_` clear that used to happen mid-record.
    //
    // ORDERING NOTE for that clear: `OceanRenderable::BuildSurfSimPass` reads the same two flags
    // to keep the surf sim off a frame that rebuilds the shore maps. Its pass (Main_SurfSim) sits
    // earlier in the schedule, so its builder runs BEFORE this one — the flag it sees is the same
    // one it saw when the clear lived in the record body.
    // Repointed off the ocean sim for the same reason as Main_SurfSim above: this pass renders the
    // shore depth map and builds its SDF, and reads nothing the sim writes.
    gb.pShoreDepth = rg.AddPass2(RenderPass::Main_TerrainDepth, { pGiCompute },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            OceanSimulation* oceanSim = Systems::GetOceanSimulation();
            if (!oceanSim) { return {}; }
            ShoreDepthPoints pts{};
            ID3D12Resource* const shoreDepth = oceanSim->GetShoreDepthResource();
            pts.drawDepth = oceanSim->ShouldRenderShoreDepth() && shoreDepth != nullptr &&
                            oceanSim->GetShoreDepthDsv().ptr != 0;
            ID3D12Resource* const sdfSource = oceanSim->GetShoreSdfSourceResource();
            ID3D12Resource* const scratch = oceanSim->GetShoreSdfScratchResource();
            ID3D12Resource* const sdf = oceanSim->GetShoreSdfResource();
            pts.buildSdf = oceanSim->ShouldBuildShoreSdf() && sdfSource != nullptr &&
                           scratch != nullptr && sdf != nullptr &&
                           oceanSim->GetShoreSdfSourceDsv().ptr != 0 &&
                           oceanSim->CanBuildShoreSdf();
            if (!pts.drawDepth && !pts.buildSdf) { return {}; }

            // The map is static now, so the depth window is re-rendered once per level rather
            // than every time the camera crosses a snap step.
            bool opened = false;
            if (pts.drawDepth)
            {
                pts.depthWrite = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(shoreDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                ctx.NextPoint();
                pts.depthRead = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(shoreDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                opened = true;
            }
            if (pts.buildSdf)
            {
                // Its own point rather than riding the depth window's read point: the two
                // cascades draw between them, and a point is emitted wholesale at its first
                // match.
                if (opened) { ctx.NextPoint(); }
                pts.sdfWrite = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(sdfSource, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                ctx.NextPoint();
                pts.sdfRead = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(sdfSource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                // The flood's ping-pong pair, taken at the same point the source becomes
                // readable — which is where BuildShoreSdf's own two UAV requests land.
                ctx.Use(scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ctx.Use(sdf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                // ...and the flood's closing transition, which it still performs by name.
                ctx.NextPoint();
                ctx.Use(sdf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                // Cross-frame state, committed in the builder: the record body used to clear it
                // AFTER BuildShoreSdf, including on the runs where the flood bailed out.
                oceanSim->MarkShoreSdfBuilt();
            }
            const SceneView* const shoreView = &oceanSim->GetShoreDepthView();
            return [this, renderer, shoreView, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassShoreDepth);
                Pass_ShoreDepth(renderer, c, shoreView, pts);
            };
        });
}

// GPU cull, CSM (legacy only), spot and point shadow atlases.
void SceneRenderer::BuildShadows(Renderer* renderer, GraphBuild& gb)
{
    auto& rg = gb.rg;
    const auto& D = gb.D;

    // Rung 0 / Step 4: GPU cull -> indirect shadow args, before the shadow passes (its output
    // is not consumed yet). Manages its own UAV states (declares none). Placed in the chain so
    // Step 6's ExecuteIndirect can consume it.
    // pass-flow S7a: PrepareCullPass RETURNS this frame's decisions (and the points it declared
    // them under); RecordCull takes them as a parameter. The two `Will*` predicates that existed
    // only to keep a Prepare and a Record from drifting are deleted.
    auto pShadowCull = rg.AddPass2(RenderPass::Main_ShadowCull, { gb.pShoreDepth },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->shadowGpu) { return {}; }
            const ShadowGpuData::CullDecisions dec = frame_->shadowGpu->PrepareCullPass(ctx);
            if (!dec.active) { return {}; }
            return [this, renderer, dec](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassShadowCull);
                Pass_ShadowCull(renderer, c, dec);
            };
        });

    // Step 24f-2 (decided in DecideFrame): in VSM mode the CSM pass is omitted entirely and
    // downstream passes chain off the cull instead.
    if (decisions_.vsmActive)
    {
        gb.pShadow = pShadowCull;
    }
    else
    {
        // pass-flow S7c: the builder decides `indirect` ONCE and both the registration walk and
        // the per-cascade draw workers get that same value. It selects which objects draw at all
        // (GPU-instanced casters go through the cull when GI folding is active), so the two sides
        // disagreeing means a caster transitioning its instance buffer with nothing declared.
        gb.pShadow = rg.AddPass2(RenderPass::Main_CSM, { pShadowCull }, /*mtDeps=*/{},
            { { D.shadow, D3D12_RESOURCE_STATE_DEPTH_WRITE } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                if (!frame_->cascadeViews) { return {}; }
                ctx.UseDeclared(); // the CSM atlas -> DEPTH_WRITE
                const std::uint32_t atlasPoint = ctx.usePoint ? *ctx.usePoint : 0u;
                const bool indirect = IndirectShadowDrawsActive();
                ctx.NextPoint();
                PrepareOpaqueDrawStates(ctx, frame_->cascadeViews->data(),
                                        frame_->cascadeViews->size(), indirect);
                return [this, renderer, atlasPoint, indirect](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassCSM);
                    Pass_CSM(renderer, c, *frame_->cascadeViews, atlasPoint, indirect);
                };
            });

        // Occlusion plan S5b: the cascades' light-space two-pass HZB cull, three passes chained
        // off Main_CSM (pass A). Every one of them is empty on a frame the main cull did not test
        // (PrepareCullPass's decision, read here), and gb.pShadow moves to the last one so the
        // local-light passes and the lighting (which samples the atlas) order after pass B.
        //   Main_CsmHzb        the four tile pyramids from the atlas after pass A (atlas readable,
        //                      pyramids UAV -> readable);
        //   Main_ShadowCullPost the deferred casters against them -> pass-B args + list;
        //   Main_CSMPost       ExecuteIndirect of pass B into the same tiles, no clear.
        auto pCsmHzb = rg.AddPass2(RenderPass::Main_CsmHzb, { gb.pShadow },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ShadowGpuData* sg = frame_->shadowGpu;
                if (!sg || !sg->CascadeHzbCullThisFrame()) { return {}; }
                render::CascadeHzb& hzb = sg->CascadeHzbRef();
                const auto& DF = ctx.renderer->GetDeferredForFrame();
                if (!hzb.Ready() || DF.shadow == nullptr || DF.shadowSRV.ptr == 0) { return {}; }
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(DF.shadow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c)
                {
                    ctx.Use(hzb.Pyramid(c), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                ctx.NextPoint();
                for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c)
                {
                    ctx.Use(hzb.Pyramid(c), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                // Cross-frame state, committed with the decision: after this frame the pyramids
                // hold this frame's tiles, which is what next frame's main cull tests against.
                hzb.MarkBuilt(ctx.renderer->GetTotalFrameNumber());
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassCsmHzb);
                    Pass_CsmHzb(renderer, c, point);
                };
            });
        auto pCullPost = rg.AddPass2(RenderPass::Main_ShadowCullPost, { pCsmHzb },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                if (!frame_->shadowGpu) { return {}; }
                const ShadowGpuData::CullPostDecisions dec = frame_->shadowGpu->PrepareCullPostPass(ctx);
                if (!dec.active) { return {}; }
                return [this, renderer, dec](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassShadowCullPost);
                    Pass_ShadowCullPost(renderer, c, dec);
                };
            });
        gb.pShadow = rg.AddPass2(RenderPass::Main_CSMPost, { pCullPost }, /*mtDeps=*/{},
            { { D.shadow, D3D12_RESOURCE_STATE_DEPTH_WRITE } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                if (!frame_->cascadeViews || !frame_->shadowGpu || !frame_->shadowGpu->CascadeHzbCullThisFrame()) { return {}; }
                ctx.UseDeclared(); // the CSM atlas -> DEPTH_WRITE, back from the pyramid build's read
                const std::uint32_t atlasPoint = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.NextPoint();
                return [this, renderer, atlasPoint](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassCSMPost);
                    Pass_CSMPost(renderer, c, *frame_->cascadeViews, atlasPoint);
                };
            });
    }

    // No declarations: the per-light command lists are recorded in parallel with
    // no deterministic submit order inside the batch, so each list must register
    // the atlas state itself (first-use in whichever list lands first).
    // pass-flow S6: the VSM skip and the "only the ACTIVE views" clamp were computed here AND
    // recomputed by the body; now the builder decides `n` once and it rides into the record as a
    // capture. The clamp itself still matters for the same reason it always did: the view arrays
    // are fixed-size and their tail entries keep queues from earlier frames, whose object
    // pointers a level switch has already freed — reading past it crashed the stress harness
    // inside the very first SwitchLevel.
    gb.pSpotShadow = rg.AddPass2(RenderPass::Main_SpotShadows, { gb.pShadow },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (render::VsmActive()) { return {}; } // the spot atlas is a 1x1 placeholder there
            if (!frame_->spotShadowViews) { return {}; }
            const size_t n = std::min(frame_->spotShadowViews->size(),
                                      frame_->lightManager->GetShadowedSpotCount());
            if (n == 0) { return {}; }
            // One registration of the atlas state covers every per-light list: they all ask for
            // the same state, and only the first light's list actually transitions it.
            const std::uint32_t atlasPoint = ctx.usePoint ? *ctx.usePoint : 0u;
            const bool indirect = IndirectShadowDrawsActive(); // S7c: one decision for both sides
            ctx.Use(ctx.renderer->GetDeferredForFrame().spotShadow.Get(),
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);
            ctx.NextPoint();
            PrepareOpaqueDrawStates(ctx, frame_->spotShadowViews->data(), n, indirect);
            return [this, renderer, n, atlasPoint, indirect](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSpotShadow);
                Pass_SpotShadows(renderer, c, *frame_->spotShadowViews, n, atlasPoint, indirect);
            };
        });

    // B2b: point cube shadows. Same per-CL atlas-state registration story as spot
    // shadows (parallel per-face lists, no declared states). Runs before Pass_PointLights
    // (which samples the cube atlas in B3).
    gb.pPointShadow = rg.AddPass2(RenderPass::Main_PointShadows, { gb.pSpotShadow },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (render::VsmActive()) { return {}; } // same skip as the spot pass
            if (!frame_->pointShadowViews) { return {}; }
            const size_t n = std::min(frame_->pointShadowViews->size(),
                                      frame_->lightManager->GetShadowedPointCount() * 6);
            if (n == 0) { return {}; }
            const std::uint32_t atlasPoint = ctx.usePoint ? *ctx.usePoint : 0u;
            const bool indirect = IndirectShadowDrawsActive(); // S7c: one decision for both sides
            ctx.Use(ctx.renderer->GetDeferredForFrame().pointShadow.Get(),
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);
            ctx.NextPoint();
            PrepareOpaqueDrawStates(ctx, frame_->pointShadowViews->data(), n, indirect);
            return [this, renderer, n, atlasPoint, indirect](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassPointShadow);
                Pass_PointShadows(renderer, c, *frame_->pointShadowViews, n, atlasPoint, indirect);
            };
        });
}

// G-buffer, VSM page request/render, depth pyramid, GTAO.
void SceneRenderer::BuildGBufferAndAo(Renderer* renderer, GraphBuild& gb)
{
    auto& rg = gb.rg;
    const auto& D = gb.D;
    const auto& P = gb.P;

    // pass-flow S7d: the G-buffer's target states used to be written TWICE — here, and again as
    // the inner graph's driver `declares` — with a comment asking the two lists to stay in step.
    // Now there is ONE list, and the inner driver emits the point this builder declared instead of
    // applying a declaration of its own.
    gb.pGbuf = rg.AddPass2(RenderPass::Main_GBuffer, { gb.pPointShadow },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
        RenderGraphPassContext& p = ctx;
        const std::uint32_t bindPoint = p.usePoint ? *p.usePoint : 0u;
        const auto& DG = p.renderer->GetDeferredForFrame();
        p.Use(DG.gb0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DG.gb1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DG.gb2.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DG.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#if WITH_EDITOR
        p.Use(DG.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#endif
        p.Use(DG.gbAux.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DG.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // Objects transition their own buffers inside Render, on the fan-out worker (GPU-
        // instanced clouds flip the instance buffer back to SRV).
        p.NextPoint();
        // The G-buffer never draws through the indirect shadow path, so its walk registers every
        // visible opaque object.
        if (frame_->mainView) { PrepareOpaqueDrawStates(p, frame_->mainView, 1, /*indirect=*/false); }
        return [this, renderer, bindPoint](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassGBuffer);
            Pass_GBuffer(renderer, c, *frame_->camera, *frame_->mainView, bindPoint);
        };
    });

    // Occlusion plan S3a: the camera prepare's box queries, drawn against the G-buffer depth
    // (read-only, no colour) right after the G-buffer -- UE's RenderOcclusion sits after the base
    // pass, before translucency, so glass and water never occlude. The builder declares nothing
    // on frames without a plan (method off, nothing to ask); Main_Hzb lists this pass as a
    // prerequisite so the depth's DEPTH_READ use is ordered before its NON_PIXEL consumers.
    gb.pOcclusion = rg.AddPass2(RenderPass::Main_OcclusionQueries, { gb.pGbuf },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const vis::OcclusionQueryPlan* plan = frame_->occlusionPlan;
            const auto& D = renderer->GetDeferredForFrame();
            if (!plan || plan->method != vis::OcclusionMethod::Queries || plan->batches.empty() ||
                !frame_->occlusionQueries || D.dsv.ptr == 0)
            {
                return {};
            }
            ctx.NextPoint();
            const uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ);
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassOcclusionQueries);
                Pass_OcclusionQueries(renderer, c, point);
            };
        });

    // Rung 2 / Step 19: VSM page-request pass — reads the camera depth (after GBuffer), marks the
    // virtual pages the frame needs. Independent consumer of depth (its output is unused for now),
    // so it doesn't gate lighting. Manages the request-buffer UAV state itself.
    // pass-flow S3c: authored with AddPass2 — one gate decides declarations and record, and the
    // barrier-point indices travel as a by-value capture. The depth read is registered by the
    // builder itself (NOT via the declare list): this pass runs right after the G-buffer, which
    // leaves depth in DEPTH_WRITE, and reading it without the graph transitioning it was GBV
    // id=1358 on all three Deferred[N].Depth.
    auto pVsmPageRequest = rg.AddPass2(RenderPass::Main_VsmPageRequest, { gb.pGbuf },
        [this, renderer](RenderGraphPassContext& ctx)
            -> std::function<void(RenderGraphPassContext)> {
            // `decisions_.vsmSkipUpdate` is decided before the graph is built and does not change during
            // the frame, so this gate is exact.
            if (!render::VsmActive() || decisions_.vsmSkipUpdate) { return {}; }
            if (!frame_->vsm || !frame_->vsm->IsAllocated()) { return {}; }
            // Async-compute step 8 (D7): NON_PIXEL, not kSrvAll. This pass reads depth from a
            // COMPUTE shader, so the PIXEL bit was never anything but the "one combined state
            // instead of a flip" optimisation — and that bit is illegal on a compute queue, so
            // leaving it set here would hand Main_RTTrace a state it cannot legally take.
            // The hand-over is the producer's job (D7), and this is the producer.
            ctx.Use(ctx.renderer->GetDeferredForFrame().depth.Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // VSM owns the buffers its Record* functions barrier, so it declares them itself.
            const VirtualShadowMap::PageRequestPoints pts =
                frame_->vsm->PrepareRequestPass(ctx);
            return [this, renderer, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassVsmPageRequest);
                Pass_VsmPageRequest(renderer, c, pts);
            };
        });
    (void)pVsmPageRequest;

    // Rung 2 / Step 22: render shadow casters into the resident physical pages (depth-only into the
    // VSM pool). Only wired when the VSM path is on, so the default render is untouched. Consumes
    // Step 20's page table + Rung 0's per-view cull; the light passes (Step 21) read the pool.
    // The skip-when-still decision (and the cross-frame stillness counters behind it) is taken in
    // DecideFrame; here it only selects whether the pass exists.
    gb.pVsmPageRender = static_cast<size_t>(-1);
    if (decisions_.vsmActive)
    {
        // No declared pool state: RecordPageRender transitions the pool DEPTH_WRITE itself (the
        // light passes declare it back to SRV). Ordering to the light passes is via their prereq.
        // pass-flow S3: authored with AddPass2 — ONE gate decides both the declarations and the
        // record, and the PageRenderDecisions travel as a by-value lambda capture instead of a
        // class-member bridge Prepare and Record could disagree over.
        gb.pVsmPageRender = rg.AddPass2(RenderPass::Main_VsmPageRender, { pVsmPageRequest },
            [this, renderer](RenderGraphPassContext& ctx)
                -> std::function<void(RenderGraphPassContext)> {
                if (decisions_.vsmSkipUpdate || !frame_->shadowGpu) { return {}; }
                const VirtualShadowMap::PageRenderDecisions dec =
                    frame_->vsm->PrepareRenderPass(ctx, frame_->shadowGpu, frame_->wind);
                return [this, renderer, dec](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassVsmPageRender);
                    Pass_VsmPageRender(renderer, c, dec);
                };
            });
    }

    // P6B: AO reads the G-buffer normal and depth and writes its own half-res target, so it only
    // has to order after the G-buffer. Lighting and compose consume it, so they order after this.
    // Skipped entirely when disabled -- an unregistered pass costs nothing, whereas a registered
    // one still pays its barriers.
    // AddPass2: the builder makes the decision ONCE and declares from it, so a disabled frame
    // declares nothing and the body is empty -- no separate Prepare to keep in sync, which is the
    // whole point of the form.
    // P6C: the depth pyramid. Ordered after the G-buffer (it reduces the depth buffer) and before
    // anything that would consume it -- GTAO's horizon search (step 5) and SSR's HiZ march (step 6).
    gb.pHzb = rg.AddPass2(RenderPass::Main_Hzb, { gb.pGbuf, gb.pOcclusion },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& D = renderer->GetDeferredForFrame();
            if (!resources_.GetHzbMaterial() || resources_.GetHzbCBSizeBytes() == 0u ||
                D.depthSRV.ptr == 0 || D.hzb.Get() == nullptr || D.hzbClosest.Get() == nullptr ||
                D.hzbMips == 0)
            {
                return {};
            }
            ctx.NextPoint();
            const uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            // Async-compute step 8 (D7): NON_PIXEL for the same reason as Main_VsmPageRequest —
            // the pyramid build is a compute shader, and the PIXEL bit would make the state
            // illegal for Main_RTTrace to take on the compute queue.
            ctx.Use(D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // D7 HAND-OVER, and the only declaration in this pass for a resource it does not use.
            //
            // `Main_RTTrace` runs on the compute queue and reads gb1, but the G-buffer leaves gb1 in
            // RENDER_TARGET — a DIRECT-queue-only state. A consumer normally acquires for itself,
            // and that is exactly what breaks here: the acquire's BEFORE state would be
            // RENDER_TARGET on a compute list. So the graphics side has to hand it over, in a pass
            // RTTrace explicitly depends on — this one. It costs one transition and rides the point
            // this pass already emits, so no body changed.
            //
            // Before the move RTTrace declared kSrvAll and acquired gb1 itself, which worked only
            // because it happened to be scheduled after the G-buffer's other consumers. That was
            // incidental ordering, not a dependency; the async move turned it into a hard error and
            // this declaration plus RTTrace's new prereq on this pass is the fix.
            ctx.Use(D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // The whole chain in ONE state for the whole build; the mips are separated by UAV
            // barriers, not transitions. See the shader header for why.
            ctx.Use(D.hzb.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // The closest chain is declared UNCONDITIONALLY, even on frames the shader does not
            // write it. Its descriptors sit in the same VOLATILE table either way, and a barrier
            // set that changes with a UI setting is exactly the kind of thing that compiles fine
            // and then breaks the one configuration nobody screenshots.
            ctx.Use(D.hzbClosest.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // ...and back to their resting state, which is what GTAO, SSR and the inspector read.
            ctx.NextPoint();
            ctx.Use(D.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ctx.Use(D.hzbClosest.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassHzb);
                Pass_Hzb(renderer, c, point);
            };
        });

    // Occlusion plan S3b: the camera prepare's boxes tested against the pyramid just built --
    // UE's FHZBOcclusionTester::Submit runs right after its HZB build, for the same reason. Two
    // points: the dispatch (pyramid readable, results UAV) and the readback copy (results
    // COPY_SOURCE, which is where the frame leaves them). Nothing downstream depends on this
    // pass; its output is read by the CPU `vis.queryLatency` frames later. The gate repeats the
    // pyramid's own: a plan with the Hzb method on a frame with no pyramid must not run a test
    // against whatever the texture holds.
    gb.pVisTest = rg.AddPass2(RenderPass::Main_VisTest, { gb.pHzb },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const vis::OcclusionQueryPlan* plan = frame_->occlusionPlan;
            vis::HzbOcclusionTester* tester = frame_->hzbTester;
            const auto& D = renderer->GetDeferredForFrame();
            const bool hzbBuilt = resources_.GetHzbMaterial() && resources_.GetHzbCBSizeBytes() != 0u &&
                                  D.depthSRV.ptr != 0 && D.hzb.Get() != nullptr && D.hzbClosest.Get() != nullptr &&
                                  D.hzbMips != 0;
            if (!plan || plan->method != vis::OcclusionMethod::Hzb || plan->boxes.empty() ||
                !tester || !tester->Ready() || !hzbBuilt || D.hzbSRV.ptr == 0)
            {
                return {};
            }
            ctx.NextPoint();
            const uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ctx.Use(tester->ResultsBuffer(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            ctx.Use(tester->ResultsBuffer(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassVisTest);
                Pass_VisTest(renderer, c, point);
            };
        });

    // P6C: the horizon search reads the pyramid, so GTAO orders after the build.
    gb.pGtao = rg.AddPass2(RenderPass::Main_Gtao, { gb.pGbuf, gb.pHzb },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const GtaoSettings& s = frame_->settings.gtao;
            const auto& D = renderer->GetDeferredForFrame();
            const auto& P = renderer->GetDeferredForPrevFrame();
            // Every material and every handle the chain needs, checked HERE rather than in the
            // body: a body that early-outs after the declarations have been made loses the pass's
            // barriers silently. Deciding once is the whole reason this is an AddPass2.
            const bool ready = s.enabled && resources_.GetGtaoMaterial() &&
                resources_.GetGtaoFilterMaterial() && resources_.GetGtaoTemporalMaterial() &&
                resources_.GetGtaoUpsampleMaterial() && resources_.GetGtaoCBSizeBytes() != 0u &&
                resources_.GetGtaoFilterCBSizeBytes() != 0u &&
                resources_.GetGtaoTemporalCBSizeBytes() != 0u &&
                resources_.GetGtaoUpsampleCBSizeBytes() != 0u &&
                D.depthSRV.ptr != 0 && D.gbSRV[1].ptr != 0 && D.gbSRV[3].ptr != 0 &&
                D.gtaoUAV.ptr != 0 && D.gtaoFilteredUAV.ptr != 0 &&
                D.gtaoHistoryUAV.ptr != 0 && D.gtaoUpsampledUAV.ptr != 0 &&
                P.gtaoHistorySRV.ptr != 0;
            if (!ready)
            {
                // Nothing declared, nothing recorded — and the history counter resets, so the
                // temporal stage re-seeds instead of reading a frame that was never written.
                gtaoHistoryFrames_ = 0u;
                return {};
            }

            constexpr D3D12_RESOURCE_STATES kAoRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            GtaoChain chain{};
            chain.denoise = s.denoise;
            chain.temporal = s.temporal;
            chain.frameIndex = gtaoFrameCounter_++;

            // Cross-frame state is committed HERE, in the serial Prepare, not in the body.
            const uint32_t aoW = std::max(1u, (renderer->GetRenderWidth() + 1u) / 2u);
            const uint32_t aoH = std::max(1u, (renderer->GetRenderHeight() + 1u) / 2u);
            if (aoW != gtaoHistoryWidth_ || aoH != gtaoHistoryHeight_)
            {
                gtaoHistoryWidth_ = aoW;
                gtaoHistoryHeight_ = aoH;
                gtaoHistoryFrames_ = 0u; // the whole Deferred set was recreated: no valid history
            }
            chain.historyValid = chain.temporal && gtaoHistoryFrames_ > 0u;
            gtaoHistoryFrames_ = chain.temporal ? std::min(gtaoHistoryFrames_ + 1u, 4u) : 0u;

            // --- declarations, from the same decision ---
            // The chain's own targets are declared NON_PIXEL only: every reader of them is a
            // compute shader, here and in the consumers P6B item 7 will add. Declaring kSrvAll
            // instead would leave them one state away from their canonical at frame end for no
            // reader's benefit — which --canonical-check duly reported. The G-buffer inputs keep
            // kSrvAll because the later forward passes really do sample them from a pixel shader.
            ctx.NextPoint();
            chain.pointRaw = ctx.usePoint ? *ctx.usePoint : 0u;
            // Async-compute step 8 (D7): NON_PIXEL, not kSrvAll — a deliberate reversal of the
            // note above, and it costs one flip.
            //
            // GTAO reads both from a compute shader; kSrvAll was here only so the later FORWARD
            // passes would not have to raise them again. But `Main_RTTrace` now runs on the compute
            // queue and reads the same two, and GTAO shares its dependencies (both hang off
            // pGbuf/pHzb) — so whichever of the two the topological order happens to put first,
            // this pass must not leave them in a state the other cannot legally take. Declaring
            // NON_PIXEL here makes the result INDEPENDENT of that order, which is worth more than
            // the flip it costs: the alternative is a correctness property that holds by accident.
            //
            // Ordering GTAO after RTTrace instead would be worse — it would make the graphics queue
            // wait on the async pass, which is precisely the overlap this move exists to create.
            ctx.Use(D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ctx.Use(D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // The pyramid rests in this state, so this normally compiles to no barrier -- declaring
            // it is what makes that a fact the compile knows rather than an assumption.
            ctx.Use(D.hzb.Get(), kAoRead);
            ctx.Use(D.gtao.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // Each stage reads what the previous one wrote, so every stage boundary is a
            // UAV -> SRV flip on one target and a fresh UAV on the next. The source chain below
            // has to match the record body's exactly; both read it off `chain`.
            if (chain.denoise)
            {
                ctx.NextPoint();
                chain.pointDenoise = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(D.gtao.Get(), kAoRead);
                ctx.Use(D.gtaoFiltered.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (chain.temporal)
            {
                ctx.NextPoint();
                chain.pointTemporal = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(chain.denoise ? D.gtaoFiltered.Get() : D.gtao.Get(), kAoRead);
                // The previous frame's accumulation. It rests shader-readable (the upsample read
                // it last frame), so this usually compiles to no barrier at all — but declaring it
                // is what makes that a fact the compile knows rather than an assumption.
                ctx.Use(P.gtaoHistory.Get(), kAoRead);
                ctx.Use(D.gbVelocity.Get(), kSrvAll);
                ctx.Use(D.gtaoHistory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            ctx.NextPoint();
            chain.pointUpsample = ctx.usePoint ? *ctx.usePoint : 0u;
            ID3D12Resource* const upsampleSrc = chain.temporal ? D.gtaoHistory.Get()
                : (chain.denoise ? D.gtaoFiltered.Get() : D.gtao.Get());
            ctx.Use(upsampleSrc, kAoRead);
            ctx.Use(D.gtaoUpsampled.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // ...and back to its resting state. Every other target in this chain ends the frame
            // shader-readable because the next stage reads it; the last one has no next stage, so
            // it says so explicitly instead of resting as a UAV.
            ctx.NextPoint();
            chain.pointRestore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.gtaoUpsampled.Get(), kAoRead);

            return [this, renderer, chain](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassGtao);
                Pass_Gtao(renderer, c, *frame_->camera, chain);
            };
        });

    // Gather-then-shade split (async-compute prep): the RT reflection's GATHER phase, added HERE
    // -- before the shadow/lighting passes -- because its inputs (TLAS, depth, gb1, the sky
    // cubes, the CPU-filled spot/point light buffers) are complete once the G-buffer and
    // Pass_BuildAS are done. On the direct queue this only moves the cost earlier in the frame;
    // when the async plan lands, THIS pass moves to the compute queue and overlaps
    // VsmPageRender + lighting. Two landmines for that move, both deliberate today:
    //   - the TLAS/bindless reads BYPASS the barrier compile (AS buffers are undeclared), so the
    //     cross-queue edge to Pass_BuildAS must become an EXPLICIT fence -- undeclared reads
    //     generate no edge;
    //   - depth/gb1 are declared kSrvAll here (their resting state at this point, no barrier);
    //     kSrvAll contains PIXEL_SHADER_RESOURCE, which is ILLEGAL on a compute queue, so the
    //     move must re-declare them NON_PIXEL_SHADER_RESOURCE.
    if (decisions_.rtReflect && gb.pBuildAS != static_cast<size_t>(-1))
    {
        const auto& DT = gb.D;
        // ASYNC COMPUTE (step 8) — the plan's first real user of the second queue.
        //
        // Both landmines above are now handled, not merely noted:
        //   - the cross-queue edge to Pass_BuildAS comes from the explicit mtDep `{ gb.pBuildAS }`.
        //     The AS buffers are undeclared, so no resource-derived edge exists; the graph turns a
        //     graph DEPENDENCY into the fence instead (step 6), which is exactly why that mtDep is
        //     load-bearing and must not be "cleaned up" as redundant with the prereq.
        //   - depth and gb1 are declared NON_PIXEL here, and the producers (Main_VsmPageRequest,
        //     Main_Hzb) hand them over in that state (D7). kSrvAll would be illegal on this queue
        //     and the compile refuses it by name.
        // The prereq on `pHzb` is NEW and load-bearing: it is the pass that hands depth and gb1
        // over in a compute-legal state (D7). Before the move this pass acquired them itself and
        // relied on being scheduled after the G-buffer's other readers — incidental ordering that
        // the compile now rejects by name rather than tolerating.
        gb.pRtTrace = rg.AddPass2(RenderPass::Main_RTTrace, RenderQueue::AsyncCompute,
            { gb.pGbuf, gb.pBuildAS, gb.pHzb }, { gb.pBuildAS },
            { { DT.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { DT.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { DT.rtPayload.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { DT.rtPayloadUv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                const auto& DP = renderer->GetDeferredForFrame();
                if (!resources_.GetRtTraceMaterial() || DP.rtPayloadUAV.ptr == 0 ||
                    DP.rtPayloadUvUAV.ptr == 0)
                {
                    return {};
                }
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassRTTrace);
                    Pass_RTTrace(renderer, c, *frame_->camera, point);
                };
            });
    }
}

// directional, spot and point lighting, skybox.
void SceneRenderer::BuildLighting(Renderer* renderer, GraphBuild& gb)
{
    auto& rg = gb.rg;
    const auto& D = gb.D;

    // pass-flow S5: ONE builder shared by both variants below (VSM vs Legacy declare different
    // first-use sets, but the readiness decision and the record are identical). It owns the
    // gate Pass_Lighting used to repeat AFTER declaring — a missing material, a zero CB size or a
    // null staged SRV made the body return without emitting a single one of the eleven barriers
    // it had already declared, which under compiled barriers leaves every later reader of the
    // G-buffer with a wrong before-state.
    auto lightBuilder = [this, renderer](RenderGraphPassContext& ctx)
        -> std::function<void(RenderGraphPassContext)> {
        if (!resources_.GetLightingMaterial() || resources_.GetLightingCBSizeBytes() == 0)
        {
            return {};
        }
        const auto& DL = renderer->GetDeferredForFrame();
        if (DL.gbSRV[0].ptr == 0 || DL.gbSRV[1].ptr == 0 || DL.gbSRV[2].ptr == 0 ||
            DL.gbSRV[3].ptr == 0 || DL.gbAuxSRV.ptr == 0 || DL.depthSRV.ptr == 0 ||
            DL.shadowSRV.ptr == 0)
        {
            return {};
        }
        ctx.UseDeclared();
        const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
        return [this, renderer, point](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassLighting);
            Pass_Lighting(renderer, c, *frame_->camera, point);
        };
    };
    size_t pLight;
    // Step 24f: in VSM mode the directional shader samples the clipmap (VSM page pool + table), so it
    // must order AFTER the page render and declare those SRV-readable. Legacy = the CSM-only decls.
    // P6B item 7: lighting now SAMPLES the AO target, so it must order after the chain that writes
    // it. Until this step the AO pass was a leaf nobody depended on, and the two were free to run
    // concurrently -- correct only while nothing read the result.
    if (decisions_.vsmActive && gb.pVsmPageRender != static_cast<size_t>(-1))
    {
        ID3D12Resource* vpool = frame_->vsm->PagePool();
        ID3D12Resource* vpt = frame_->vsm->PageTable();
        pLight = rg.AddPass2(RenderPass::Main_Lighting, { gb.pGbuf, gb.pVsmPageRender, gb.pGtao }, { gb.pShadow },
            { { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.shadow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { vpool, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { vpt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gtaoUpsampled.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            lightBuilder);
    }
    else
    {
        pLight = rg.AddPass2(RenderPass::Main_Lighting, { gb.pGbuf, gb.pGtao }, { gb.pShadow },
            { { D.gb0.Get(), kSrvAll },
              { D.gb1.Get(), kSrvAll },
              { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll },
              { D.gbAux.Get(), kSrvAll },
              { D.depth.Get(), kSrvAll },
              { D.shadow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gtaoUpsampled.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            lightBuilder);
    }

    // Step 21: the spot lighting shader always binds the VSM page-table (t7) + pool (t8) SRVs, so
    // when the VSM is allocated they must be in a readable state on entry (declared here). When VSM
    // sampling is active, also order after the page render (fresh page content this frame).
    // pass-flow S6: ONE builder for every variant. The old Prepare gated on the light COUNT only
    // ("body early-outs"), but the body had four more early-outs after it — the light buffer, its
    // CPU pointer and SRV, the staged G-buffer handles, and VSM readiness in VSM mode — each of
    // which returned with ten states already declared. All of them are frame state, knowable
    // here, so they decide once and the pass declares nothing on a frame it will not record.
    auto spotBuilder = [this, renderer](RenderGraphPassContext& ctx)
        -> std::function<void(RenderGraphPassContext)> {
        LightManager& lm = *frame_->lightManager;
        const size_t spotCount = lm.GetSpotLightCount();
        if (spotCount == 0 || !lm.HasSpotLightBuffer(spotCount)) { return {}; }
        const UINT frameIdx = renderer->GetCurrentFrameIndex();
        if (!lm.GetSpotLightBufferCPU(frameIdx) || lm.GetSpotLightSrv(frameIdx).ptr == 0)
        {
            return {};
        }
        const auto& DS = renderer->GetDeferredForFrame();
        if (DS.gbSRV[0].ptr == 0 || DS.gbSRV[1].ptr == 0 || DS.gbSRV[2].ptr == 0 ||
            DS.gbSRV[3].ptr == 0 || DS.gbAuxSRV.ptr == 0 || DS.depthSRV.ptr == 0 ||
            DS.spotShadowSRV.ptr == 0)
        {
            return {};
        }
        // Only when VSM SAMPLING is requested but the pool is not ready (startup / OOM) — never
        // in Legacy mode, which must still light through the atlas.
        const bool vsmReady = frame_->vsm && frame_->vsm->IsAllocated() &&
                              frame_->vsm->PageTableSrv().ptr != 0 &&
                              frame_->vsm->PagePoolSrv().ptr != 0;
        if (render::VsmActive() && !vsmReady) { return {}; }
        ctx.UseDeclared();
        const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
        return [this, renderer, point](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassSpotLights);
            Pass_SpotLights(renderer, c, *frame_->camera, point);
        };
    };
    const bool vsmAlloc = frame_->vsm && frame_->vsm->IsAllocated();
    size_t pSpotLights;
    if (vsmAlloc)
    {
        ID3D12Resource* vpool = frame_->vsm->PagePool();
        ID3D12Resource* vpt = frame_->vsm->PageTable();
        const std::initializer_list<ResourceStateDecl> spotDecls = {
            { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
            { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
            { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
            { D.spotShadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { vpool, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { vpt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } };
        if (decisions_.vsmActive && gb.pVsmPageRender != static_cast<size_t>(-1))
        {
            pSpotLights = rg.AddPass2(RenderPass::Main_SpotLights, { pLight, gb.pVsmPageRender }, { gb.pSpotShadow }, spotDecls, spotBuilder);
        }
        else
        {
            pSpotLights = rg.AddPass2(RenderPass::Main_SpotLights, { pLight }, { gb.pSpotShadow }, spotDecls, spotBuilder);
        }
    }
    else
    {
        pSpotLights = rg.AddPass2(RenderPass::Main_SpotLights, { pLight }, { gb.pSpotShadow },
            { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.spotShadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            spotBuilder);
    }

    // Depends on gb.pPointShadow too: the cube must be rendered + transitioned to a
    // shader-readable state before this pass samples it (B3). kSrvAll keeps it readable
    // by both this compute pass and the later transparent (glass) pixel pass. Step 21: the point
    // shader also binds the VSM page-table (t7) + pool (t8) SRVs; ordering after the page render is
    // transitive (this pass depends on pSpotLights, which depends on gb.pVsmPageRender when active).
    auto pointBuilder = [this, renderer](RenderGraphPassContext& ctx)
        -> std::function<void(RenderGraphPassContext)> {
        LightManager& lm = *frame_->lightManager;
        const size_t pointCount = lm.PointLights().size();
        if (pointCount == 0 || !lm.HasPointLightBuffer(pointCount)) { return {}; }
        const UINT frameIdx = renderer->GetCurrentFrameIndex();
        if (!lm.GetPointLightBufferCPU(frameIdx) || lm.GetPointLightSrv(frameIdx).ptr == 0)
        {
            return {};
        }
        const auto& DP = renderer->GetDeferredForFrame();
        if (DP.gbSRV[0].ptr == 0 || DP.gbSRV[1].ptr == 0 || DP.gbSRV[2].ptr == 0 ||
            DP.gbSRV[3].ptr == 0 || DP.gbAuxSRV.ptr == 0 || DP.depthSRV.ptr == 0)
        {
            return {};
        }
        const bool vsmReady = frame_->vsm && frame_->vsm->IsAllocated() &&
                              frame_->vsm->PageTableSrv().ptr != 0 &&
                              frame_->vsm->PagePoolSrv().ptr != 0;
        if (render::VsmActive() && !vsmReady) { return {}; }
        ctx.UseDeclared();
        const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
        return [this, renderer, point](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassPointLights);
            Pass_PointLights(renderer, c, *frame_->camera, point);
        };
    };
    size_t pPointLights;
    if (vsmAlloc)
    {
        pPointLights = rg.AddPass2(RenderPass::Main_PointLights, { pSpotLights, gb.pPointShadow }, /*mtDeps=*/{},
            { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.pointShadow.Get(), kSrvAll },
              { frame_->vsm->PagePool(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { frame_->vsm->PageTable(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            pointBuilder);
    }
    else
    {
        pPointLights = rg.AddPass2(RenderPass::Main_PointLights, { pSpotLights, gb.pPointShadow }, /*mtDeps=*/{},
            { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.pointShadow.Get(), kSrvAll } },
            pointBuilder);
    }

    // pass-flow S5/S6: all three lighting passes have several variants (VSM vs Legacy, with or
    // without spot/point shadows), but each variant declares its own first-use set, so ONE builder
    // per pass covers all of them — and every variant always creates a real pass, so no builder
    // can land on a pass index that aliases somebody else's.

    // pass-flow S5: the builder owns the `frame_->skybox` gate the body used to repeat after the
    // declarations were already made.
    gb.pSky = rg.AddPass2(RenderPass::Main_Skybox, { pPointLights }, /*mtDeps=*/{},
        { { D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ } },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->skybox) { return {}; }
            ctx.UseDeclared();
            const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkybox);
                Pass_Skybox(renderer, c, *frame_->camera, point);
            };
        });
}

// reflection source (RT/SSR/clear), temporal, blur, compose, RT debug, glass.
void SceneRenderer::BuildReflections(Renderer* renderer, GraphBuild& gb)
{
    auto& rg = gb.rg;
    const auto& D = gb.D;
    const auto& P = gb.P;

    // CL group (step 5): reflection source -> reflection blur -> compose is a sequential single-dispatch
    // chain with no mtDeps. Grouping collapses its 3 command lists into 1 — the
    // per-CL prologue/acquire overhead dominates these passes' tiny record cost,
    // and the inter-pass acquire barriers become correctly-placed intra-CL barriers.
    rg.BeginCLGroup();
    // Reflection source (S8): whichever variant runs writes the same premultiplied
    // reflection buffer, so the blur + compose chain is identical. RT (S7) runs
    // instead of the screen-space source (mt-dep on Main_BuildAS; its TLAS SRV
    // bypasses the state tracker); None/SkyOnly clear the reflection buffer and
    // compose decides whether the skybox fallback is enabled.
    // Compose samples the shore-wetness texture that Main_ShoreWetness writes, but Compose is a
    // NON-FIRST member of this CL group and BeginCLGroup's contract allows an outside prereq only
    // on the FIRST member — a grouped list records as one unit, so nothing can wait in its middle.
    // The dependency therefore rides the group's first member, which orders the whole list after
    // the wetness update and keeps Compose's guarantee. Reflection waiting too costs nothing:
    // wetness is the tail of the early compute group and has long since finished.
    // The opaque RT reflection is the gather-then-shade split (Main_RTTrace upstream +
    // Main_RTResolve here); the old monolithic dispatch was deleted after a pixel-parity A/B
    // (only the HUD digits differed). rt_reflections_cs.hlsl survives for the GLASS dispatch.
    const bool useRtReflections = decisions_.rtReflect && gb.pBuildAS != (size_t)-1 &&
                                  gb.pRtTrace != (size_t)-1;
    const std::initializer_list<ResourceStateDecl> reflectDecls = {
        { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { P.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        // P6C step 6: the HiZ tracer's pyramid. Already its resting state, so this compiles to no
        // barrier -- declaring it is what makes that a fact the compile knows.
        { D.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } };
    size_t pReflectionSource; // node the blur depends on (reflection chain end)
    if (useRtReflections)
    {
        // The SHADE phase of the split: payload + lit HDR -> reflection. The only RT dispatch
        // that waits for the lighting output; the expensive trace ran long before, upstream.
        pReflectionSource = rg.AddPass2(RenderPass::Main_RTResolve,
            { gb.pSky, gb.pWetness, gb.pRtTrace }, { gb.pSky, gb.pBuildAS },
            { { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.rtPayload.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.rtPayloadUv.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassRTResolve);
                    Pass_RTResolve(renderer, c, point);
                };
            });
    }
    else if (decisions_.clearReflections)
    {
        pReflectionSource = rg.AddPass2(RenderPass::Main_ReflectionSource, { gb.pSky, gb.pWetness }, /*mtDeps=*/{},
            { { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassReflectionSource);
                    Pass_ClearReflections(renderer, c, point);
                };
            });
    }
    else
    {
        // P6C step 6: the HiZ technique marches the depth pyramid, so this orders after the build.
        // The GLASS SSR pass needs the same guarantee and gets it transitively -- it hangs off
        // Compose, which hangs off the blur, which hangs off this node.
        pReflectionSource = rg.AddPass2(RenderPass::Main_ReflectionSource, { gb.pSky, gb.pWetness, gb.pHzb }, /*mtDeps=*/{},
            reflectDecls,
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassReflectionSource);
                    Pass_ScreenSpaceReflections(renderer, c, *frame_->camera, point);
                };
            });
    }
    // pass-flow S5: each of the three variants (RT / clear / SSR) is authored with its own
    // AddPass2 — same builder shape, different pass name and first-use set. Which one exists is a
    // GRAPH-SHAPE decision and stays out here; the builder only owns what happens inside the pass.

    // SSR temporal resolve, between the trace and the glossy blur. Skipped entirely when it is not
    // active -- an unregistered pass costs nothing, a registered one still pays its barriers.
    size_t pReflectionFiltered = pReflectionSource;
    if (decisions_.reflectionTemporal)
    {
        pReflectionFiltered = rg.AddPass2(RenderPass::Main_ReflectionTemporal, { pReflectionSource },
            /*mtDeps=*/{},
            { { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.reflectionHistory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassReflectionTemporal);
                    Pass_SsrTemporal(renderer, c, point);
                };
            });
    }

    // First-use states only; the blur ping-pongs reflection<->scratch states between
    // its two dispatches inside the pass body.
    // Barrier plan step 3 chose this pass as the comparator's first test subject because it
    // exercises the hard cases in one place — `reflection` and `reflectionScratch` each take TWO
    // states inside the body, and the second pair is behind a predicate. That predicate was
    // evaluated in the Prepare AND in the body, and the note left here said "that duplication is
    // exactly what D1.1 forbids; step 5 hoists it into pass state". Step 5 never did.
    // pass-flow S6 does: the builder decides `blur` once, declares the ping-pong point from it,
    // and the body emits markers for both points and re-decides nothing.
    auto pBlur = rg.AddPass2(RenderPass::Main_ReflectionBlur, { pReflectionFiltered }, /*mtDeps=*/{},
        { { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          // The resolve's output is the blur's first input; declared unconditionally so the
          // compiled barrier set does not change with a UI toggle.
          { D.reflectionHistory.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.reflectionScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } }, // S16: roughness drives glossy blur
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& DB = renderer->GetDeferredForFrame();
            BlurPoints pts{};
            ctx.UseDeclared();
            pts.apply = ctx.usePoint ? *ctx.usePoint : 0u;
            pts.blur = resources_.GetBlurMaterial() && resources_.GetBlurCBSizeBytes() != 0;
            if (pts.blur)
            {
                ctx.NextPoint(); // the vertical dispatch ping-pongs the two targets
                pts.pingPong = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(DB.reflectionScratch.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ctx.Use(DB.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            return [this, renderer, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassReflectionBlur);
                Pass_ReflectionBlur(renderer, c, pts);
            };
        });

    // First-use states only; Compose transitions scene back to RENDER_TARGET
    // for the transparent pass at the end of its body.
    // gb.pWetness is deliberately NOT listed here: see the CL-group note above the reflection source.
    // It is carried by the group's first member, which orders this whole list after it.
    gb.pCompose = rg.AddPass2(RenderPass::Main_Compose, { pBlur }, /*mtDeps=*/{},
        { { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb2.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gbAux.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          // P6B item 7: compose samples the AO for specular occlusion. Ordering to the AO pass is
          // transitive through lighting, which now depends on it directly.
          { D.gtaoUpsampled.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        // pass-flow S6: the wetness read is gated, the hand-back point is NOT — Compose gives
        // `scene` back to the transparent pass as a render target on EVERY path, both early-outs
        // and the success tail, so the POINT is unconditional and only its content is decided.
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& DC = renderer->GetDeferredForFrame();
            ctx.UseDeclared();
            const std::uint32_t apply = ctx.usePoint ? *ctx.usePoint : 0u;
            if (frame_->ocean && frame_->ocean->IsWetnessReady())
            {
                ctx.Use(frame_->ocean->GetWetnessResource(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            ctx.NextPoint();
            const std::uint32_t handBack = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(DC.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            return [this, renderer, apply, handBack](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassCompose);
                Pass_Compose(renderer, c, *frame_->camera, apply, handBack);
            };
        });
    rg.EndCLGroup();

    // RT debug visualization (S6): runs AFTER the reflection group so it can overwrite
    // the already-consumed reflection target with ray-hit data for inspection via
    // TextureDebugViewer -> Reflection, without disturbing the composited scene. Needs
    // the TLAS (mtDep on Main_BuildAS) and reflection free (prereq/mtDep on Compose).
    // The TLAS SRV bypasses the state tracker (staged as a plain descriptor).
    if (decisions_.rtDebugView && gb.pBuildAS != (size_t)-1)
    {
        // pass-flow S5: the TLAS SRV is staged as a plain descriptor and never transitioned, so
        // the declared trio is the whole ENTRY list — but the body also hands `reflection` back to
        // its resting read state on the frames it traces, and that transition was never registered
        // at all (silently dropped: a request may only match the current point, and there was no
        // second point). The builder decides `trace` once and declares the restore point with it.
        rg.AddPass2(RenderPass::Main_RTDebug, { gb.pCompose }, { gb.pCompose, gb.pBuildAS },
            { { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                RtDebugPoints pts{};
                ctx.UseDeclared();
                pts.apply = ctx.usePoint ? *ctx.usePoint : 0u;
                const UINT frameIndex = renderer->GetCurrentFrameIndex();
                pts.trace = resources_.GetRtDebugMaterial() && rtAs_.Bindless().FrameReady(frameIndex) &&
                            rtAs_.Manager().TlasSrvCpu(frameIndex).ptr != 0 &&
                            rtAs_.Manager().TlasInstanceCount(frameIndex) != 0;
                if (pts.trace)
                {
                    ctx.NextPoint();
                    pts.restore = ctx.usePoint ? *ctx.usePoint : 0u;
                    ctx.Use(renderer->GetDeferredForFrame().reflection.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                return [this, renderer, pts](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassRTDebug);
                    Pass_RTDebug(renderer, c, *frame_->camera, pts);
                };
            });
    }

    // Off-screen glass reflections (S15b): render a glass front-face G-buffer (normal+depth)
    // then compute reflections over it into glassReflection (sampled by the forward glass pass).
    // Active in RT mode (rt_reflections_cs, incl. off-screen recompute) AND SSR mode (ssr_cs).
    // Runs after Compose so the lit opaque `light` buffer is the on-screen color source.
    // None/SkyOnly skip these passes; glass.hlsl independently suppresses or samples
    // its skybox fallback through the second b1 flag.
    gb.pGlassReflect = (size_t)-1;
    if (decisions_.glassRefl)
    {
        // pass-flow S5: the prepass-material readiness is the builder's — the body used to break
        // out of the pass AFTER declaring both targets, emitting neither barrier.
        size_t pGlassGbuf = rg.AddPass2(RenderPass::Main_GlassReflGbuffer, { gb.pCompose }, /*mtDeps=*/{},
            { { D.glassReflNormal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
              { D.glassReflDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                if (!resources_.GetGlassReflPrepassMaterial()) { return {}; }
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassGlassReflGbuffer);
                    Pass_GlassReflGbuffer(renderer, c, *frame_->camera, *frame_->mainView, point);
                };
            });
        const std::initializer_list<ResourceStateDecl> glassReflDecls = {
            { D.glassReflNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.glassReflDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            // P6C step 6: only the SSR variant reads it, but the RT variant declaring a resource
            // already in that state costs nothing and keeps ONE decl list for both branches.
            { D.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.glassReflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } };
        if (useRtReflections && gb.pBuildAS != (size_t)-1)
        {
            // RT mode: dispatch rt_reflections_cs (needs the TLAS, so mt-dep on gb.pBuildAS).
            gb.pGlassReflect = rg.AddPass2(RenderPass::Main_GlassReflections, { pGlassGbuf }, { pGlassGbuf, gb.pBuildAS },
                glassReflDecls,
                [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                    ctx.UseDeclared();
                    const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                    return [this, renderer, point](RenderGraphPassContext c) {
                        CPU_SCOPE(ProfilerScopes::kPassGlassReflections);
                        Pass_GlassReflections(renderer, c, *frame_->camera, point);
                    };
                });
        }
        else
        {
            // SSR mode: dispatch ssr_cs over the glass G-buffer (no TLAS, works on all HW).
            gb.pGlassReflect = rg.AddPass2(RenderPass::Main_GlassReflections, { pGlassGbuf }, /*mtDeps=*/{},
                { { D.glassReflNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.glassReflDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { P.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.glassReflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
                [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                    ctx.UseDeclared();
                    const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                    return [this, renderer, point](RenderGraphPassContext c) {
                        CPU_SCOPE(ProfilerScopes::kPassGlassReflections);
                        Pass_GlassReflectionsSSR(renderer, c, *frame_->camera, point);
                    };
                });
        }
    }
}

// transparent, object-id readback, debug draw, selection outline.
void SceneRenderer::BuildForwardAndEditor(Renderer* renderer, GraphBuild& gb)
{
    auto& rg = gb.rg;
    const auto& D = gb.D;

    // No declarations: the driver sequences depth/scene copies (COPY_SOURCE/DEST flips mid-list)
    // before rebinding the targets — inherently ordered work. When glass reflections are active,
    // order the transparent pass after the glass-reflection compute (it samples glassReflection;
    // gb.pCompose + the AS build are covered transitively through it).
    //
    // ASYNC COMPUTE: `gb.pObjectCompute` runs on the compute queue and this is its ONLY graphics
    // consumer — the ocean surface samples the sim's maps here, and particle emitters are
    // transparent objects drawn in this same pass. The graph derives a cross-queue fence from
    // prereqs and mtDeps and NOT from resource declarations, so this entry is what makes the wait
    // exist at all. Dropping it as "already covered by the barriers" would be a read of maps the
    // compute queue is still writing.
    const std::initializer_list<size_t> transpDeps = decisions_.glassRefl
        ? std::initializer_list<size_t>{ gb.pCompose, gb.pGlassReflect, gb.pObjectCompute }
        : std::initializer_list<size_t>{ gb.pCompose, gb.pObjectCompute };
    auto pTransp = rg.AddPass2(RenderPass::Main_Transparent, transpDeps,
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
        RenderGraphPassContext& p = ctx;
        const auto& DT = p.renderer->GetDeferredForFrame();
        TransparentPoints pts{};
        pts.copyDepth = DT.depthCopy.Get() != nullptr;
        pts.copyScene = DT.sceneOpaque.Get() != nullptr;
        // The ocean reflection compute's FULL readiness, decided once. Both of the early-outs it
        // used to make itself ran after this pass had declared the read point below.
        pts.oceanReflect = pts.copyDepth && pts.copyScene && DT.oceanReflection.Get() != nullptr &&
                           resources_.GetOceanReflectionMaterial() &&
                           resources_.GetOceanReflectionCBSizeBytes() != 0 &&
                           DT.sceneOpaqueSRV.ptr != 0 && DT.depthCopySRV.ptr != 0 &&
                           DT.oceanReflectionUAV.ptr != 0;

        // 1. Snapshot depth + opaque colour for the refraction/reflection reads.
        pts.copy = p.usePoint ? *p.usePoint : 0u;
        if (pts.copyDepth)
        {
            p.Use(DT.depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            p.Use(DT.depthCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (pts.copyScene)
        {
            p.Use(DT.scene.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            p.Use(DT.sceneOpaque.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        // 2. RecordOceanReflection's compute — declared only on the frames it runs.
        p.NextPoint();
        pts.oceanRead = p.usePoint ? *p.usePoint : 0u;
        if (pts.oceanReflect)
        {
            p.Use(DT.sceneOpaque.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            p.Use(DT.depthCopy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // P13: the UE search reads the furthest pyramid here too. Already its resting state,
            // so this declares a fact rather than requesting a barrier -- and it is NOT
            // re-declared at the next point, because nothing downstream reads the pyramid from a
            // pixel shader.
            if (DT.hzb.Get())
            {
                p.Use(DT.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            p.Use(DT.oceanReflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        // 3. All three become PS-readable for the forward draws, on every path.
        p.NextPoint();
        pts.pixel = p.usePoint ? *p.usePoint : 0u;
        p.Use(DT.sceneOpaque.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        p.Use(DT.depthCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        p.Use(DT.oceanReflection.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // 4. Rebind the forward targets. The fan-out chunks re-apply the velocity/objectID
        // pair per chunk; same states, so one registration covers them.
        p.NextPoint();
        pts.rebind = p.usePoint ? *p.usePoint : 0u;
        p.Use(DT.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DT.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        p.Use(DT.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#if WITH_EDITOR
        p.Use(DT.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#endif
        p.Use(DT.glassReflection.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // 5. Per-object reads (ocean displacement, particle sim buffers), on fan-out workers.
        if (frame_->objects)
        {
            p.NextPoint();
            for (const auto& obj : *frame_->objects)
            {
                if (!obj || !obj->IsTransparent()) { continue; }
                obj->PrepareRender(p);
            }
        }
        return [this, renderer, pts](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassTransparent);
            Pass_Transparent(renderer, c, *frame_->camera, *frame_->mainView, pts);
        };
    });

#if WITH_EDITOR
    size_t pObjectIdReadback = pTransp;
    if (renderer->HasPendingObjectIdPick())
    {
        // pass-flow S5: with AddPass2 the builder is attached at Add time, so the old trap this
        // `if` guarded against (a Prepare set on an index that ALIASES pTransp when there is no
        // pending pick) cannot happen — there is no second call to misplace.
        pObjectIdReadback = rg.AddPass2(RenderPass::Main_ObjectIdReadback, { pTransp }, /*mtDeps=*/{},
            { { D.objectID.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE } },
            [renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [renderer, point](RenderGraphPassContext c) {
                    auto t = c.BeginCL();
                    SetCommandListName(t.cl, c.pass);
                    renderer->EmitPoint(t.cl, point);
                    renderer->RecordObjectIdPickReadback(t.cl);
                    c.EndCL(t);
                };
            });
    }
#else
    const size_t pObjectIdReadback = pTransp;
#endif

    auto pDebugDraw = rg.AddPass2(RenderPass::Main_DebugDraw, { pObjectIdReadback }, /*mtDeps=*/{},
        { { D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            DebugDrawSystem* const dd = renderer->GetDebugDrawSystem();
            if (!dd || !dd->HasCommands()) { return {}; } // nothing submitted this frame
            ctx.UseDeclared();
            const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassDebugDraw);
                Pass_DebugDraw(renderer, c, *frame_->camera, point);
            };
        });

    gb.pSelectionOutline = pDebugDraw;
#if WITH_EDITOR
    if (frame_->selectedEditorObjectCount != 0)
    {
        // pass-flow S5: the material / CB-size / handle checks the body used to make AFTER
        // declaring depth -> NPS and scene -> UAV are the builder's now.
        gb.pSelectionOutline = rg.AddPass2(RenderPass::Main_SelectionOutline, { pDebugDraw }, /*mtDeps=*/{},
            { { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                if (!resources_.GetSelectionOutlineMaterial() ||
                    resources_.GetSelectionOutlineCBSizeBytes() == 0)
                {
                    return {};
                }
                const auto& DS = renderer->GetDeferredForFrame();
                if (DS.stencilSRV.ptr == 0 || DS.sceneUAV.ptr == 0) { return {}; }
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    Pass_SelectionOutline(renderer, c, point);
                };
            });
    }
#endif
}

// exposure metering, DLSS, tonemap, debug preview, debug texture.
void SceneRenderer::BuildPost(Renderer* renderer, GraphBuild& gb)
{
    auto& rg = gb.rg;
    const auto& D = gb.D;

    // Ensure tonemapping runs after the debug draw pass so the resolved backbuffer
    // always includes any debug geometry submitted during rendering.
    // Only the unconditional outputs are declared; the tonemap source (scene or
    // DLSS output) and the backbuffer copy are handled inside the pass body.
    // CL group (step 5): the optional debug-texture draw follows tonemap on the
    // same target with no mtDeps; share one command list (Debug usually early-outs).
    // P2: metering runs on the finished scene-referred HDR image, before the tone curve consumes
    // it. Deliberately OUTSIDE the tonemap CL group: it is the group's external prereq, and the
    // group contract only allows that on the first member (see the reflection group above).
    // Scheduled unconditionally but early-outs in the body when the camera is dormant, so a
    // disabled camera costs one empty command list rather than a graph-shape change per frame.
    const size_t pExposure = rg.AddPass2(RenderPass::Main_ExposureMetering, { gb.pSelectionOutline },
        /*mtDeps=*/{},
        { { D.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            ExposurePoints pts{};
            // UNCONDITIONAL, and it has to be: the body hands `scene` to the metering read BEFORE
            // it checks whether the camera is dormant, so this happens on every frame including
            // the ones with no dispatches. Returning above it left the body performing a
            // transition the compile had never registered -- the comparator's FATAL direction,
            // "MISSING (performed, never registered) res=Deferred[N].Scene". It only reproduced on
            // a level with NO cameraExposure block at all (d_emissive_test).
            ctx.UseDeclared();
            pts.apply = ctx.usePoint ? *ctx.usePoint : 0u;

            ExposureMetering& metering = renderer->Exposure();
            // The body needs all three materials too — checking only `enabled` + IsReady() here
            // left the two remaining points declared on a frame whose body broke out before them.
            pts.meter = frame_->cameraExposure.enabled && metering.IsReady() &&
                        resources_.GetExposureClearMaterial() &&
                        resources_.GetExposureBuildMaterial() &&
                        resources_.GetExposureSolveMaterial();
            if (!pts.meter)
            {
                return [this, renderer, pts](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassExposureMetering);
                    Pass_ExposureMetering(renderer, c, pts);
                };
            }
            // The histogram and exposure buffers rest at UNORDERED_ACCESS and are used at
            // UNORDERED_ACCESS, so declaring them emits no barrier -- but declaring them is what
            // makes them legal to touch at all, since an undeclared resource is an invariant
            // failure.
            ctx.Use(metering.HistogramResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(metering.ExposureResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            pts.baseLum = resources_.GetExposureBaseLumMaterial() != nullptr;
            if (pts.baseLum)
            {
                // P3B: the base layer leaves its resting read state only for this pass...
                ctx.Use(metering.BaseLumResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ctx.NextPoint();
                pts.baseLumRead = ctx.usePoint ? *ctx.usePoint : 0u;
                // ...and is back in it before the solve runs, which is why this is its OWN point.
                ctx.Use(metering.BaseLumResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            // The dev-UI readback copies AFTER the solve, then straight back to canonical so the
            // tonemap's UAV binding needs no barrier of its own.
            ctx.NextPoint();
            pts.copySrc = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(metering.ExposureResource(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            ctx.Use(metering.HistogramResource(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            ctx.NextPoint();
            pts.restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(metering.ExposureResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(metering.HistogramResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return [this, renderer, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassExposureMetering);
                Pass_ExposureMetering(renderer, c, pts);
            };
        });

    // DLSS-split: the upscale is its own pass, recorded CONCURRENTLY with the tonemap's bloom and
    // tone curve rather than in front of them in one command list. Three things make that legal:
    //   * GPU order comes from SUBMISSION order, and `pDlss` is in the tonemap's PREREQS (batch
    //     order) while its mtDeps stay empty (no record-time wait) — so the two tasks run on
    //     different workers and the lists still reach the queue in the right order.
    //   * The barrier compile walks the schedule, so this pass's points compile before the
    //     tonemap's, exactly as when they shared a body.
    //   * `ranDlss` is PREDICTED (Renderer::WillEvaluateDlss) instead of being discovered inside
    //     the record — the one thing the split really costs. See DlssHandler::WillEvaluate.
    // Its prereq is the selection outline, NOT the exposure metering: the upscale does not read
    // the metered exposure, so it can start recording while metering is still being recorded.
    // The prediction itself is DecideFrame's; this only selects whether the pass exists.
    const bool willDlss = decisions_.willDlss;
    size_t pDlss = static_cast<size_t>(-1);
    if (willDlss)
    {
        pDlss = rg.AddPass2(RenderPass::Main_DLSS, { gb.pSelectionOutline },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                const auto& DD = renderer->GetDeferredForFrame();
                DlssPoints pts{};
                pts.apply = ctx.usePoint ? *ctx.usePoint : 0u;
                // Inside EvaluateDLSS (DlssHandler): the three inputs plus the upscaled output.
                ctx.Use(DD.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ctx.Use(DD.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ctx.Use(DD.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ctx.Use(DD.dlssOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ctx.NextPoint();
                pts.output = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(DD.dlssOutput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                return [this, renderer, pts](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassDlss);
                    Pass_Dlss(renderer, c, pts);
                };
            });
    }

    rg.BeginCLGroup();
    // The tonemap's prereqs carry BOTH the metering (its exposure input) and the upscale (its
    // colour input); neither is an mtDep, so neither makes this task wait to be recorded.
    const std::initializer_list<size_t> tonemapPrereqs = willDlss
        ? std::initializer_list<size_t>{ pExposure, pDlss }
        : std::initializer_list<size_t>{ pExposure };
    // pass-flow S8: the last pass on the old API, and the one the plan called a counterexample.
    // It is not one any more — the DLSS split took the only mid-record discovery out of it, so
    // every remaining gate (bloom method, flares, FXAA readiness, the tonemap material, the
    // backbuffer) is frame state the builder can decide once and the body just walks.
    auto pTone = rg.AddPass2(RenderPass::Main_Tonemap, tonemapPrereqs, /*mtDeps=*/{},
        { { D.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.fxaa.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        [this, renderer, willDlss](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
        RenderGraphPassContext& p = ctx;
        constexpr D3D12_RESOURCE_STATES kNps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        TonemapPoints pts{};
        pts.ranDlss = willDlss;
        // The body records the bloom, the FXAA and the resolve only AFTER it has a tone-curve
        // material, so every one of those points has to be gated on it too — declaring them
        // without it left five points behind a `break`.
        const bool haveTonemap = resources_.GetTonemapMaterial() != nullptr;
        pts.bloom = haveTonemap && bloom_.Active();
        // The METHOD comes from the same flag the readiness check produced. The body used to read
        // `settings.bloom.method` directly, so a frame with method=convolution but an unready
        // convolution (no baked kernel, missing FFT material) recorded the convolution against
        // declarations made for the PYRAMID — its FFT grids then went to UAV with no barrier at all.
        pts.convolution = bloom_.Convolution();
        pts.flares = bloom_.Flares();
        pts.fxaa = haveTonemap && frame_->settings.doFxaa && resources_.GetFxaaMaterial() &&
                   resources_.GetFxaaCBSizeBytes() > 0 &&
                   renderer->GetWidth() > 0 && renderer->GetHeight() > 0;
        pts.resolve = haveTonemap && renderer->GetCurrentBackbuffer() != nullptr;

        p.UseDeclared(); // tonemap + fxaa -> UAV
        pts.apply = p.usePoint ? *p.usePoint : 0u;
        // P2: the exposure record is bound to the tonemap dispatch on every path. It rests at
        // UNORDERED_ACCESS and is used at UNORDERED_ACCESS, so this declares intent without
        // emitting a barrier — but an undeclared resource would be an invariant failure.
        if (ExposureMetering& metering = p.renderer->Exposure(); metering.IsReady())
        {
            p.Use(metering.ExposureResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        const auto& DTM = p.renderer->GetDeferredForFrame();
        p.NextPoint();
        pts.source = p.usePoint ? *p.usePoint : 0u;
        if (!willDlss)
        {
            // Hand the forward targets back. The transparent pass ends with depth as DEPTH_WRITE
            // and velocity as RENDER_TARGET because that is what it drew into, and with DLSS on
            // the upscale pass returns both to a read state as a side effect of consuming them.
            // With DLSS off nothing did, so the frame ended off-canonical on all three frame sets
            // -- an invariant that must not depend on which upscaler path is selected.
            p.Use(DTM.gbVelocity.Get(), kNps);
            p.Use(DTM.depth.Get(), kNps);
        }
        // The tonemap source. With the upscale split out, this is `scene` only when DLSS is not
        // running; the DLSS pass leaves `dlssOutput` shader-readable for the other case, and
        // declaring it here as well costs nothing and keeps the read legal for the compile.
        p.Use(willDlss ? DTM.dlssOutput.Get() : DTM.scene.Get(), kNps);
        // P8: the bloom pyramid, built between the upscale above and the tone curve below. Both
        // chains go to UNORDERED_ACCESS for the build and come back shader-readable -- the same
        // shape as the HZB pyramid, and for the same reason: a level reads the level above it
        // through its own UAV because this barrier layer transitions whole resources.
        if (pts.bloom)
        {
            // R3: the four bloom points are declared by the subsystem that records them, from
            // the decision it took in Decide(). It advances the cursor four times whatever those
            // decisions are — a point is a POSITION in this pass's program, so only its CONTENT
            // is gated.
            bloom_.Declare(p, pts.bloom_);
        }
        p.NextPoint();
        pts.fxaaRead = p.usePoint ? *p.usePoint : 0u;
        // `pts.fxaa` needed ALL of the material, the CB size, a non-zero output size AND the
        // setting — gating on the setting alone registered the FXAA resolve source on frames the
        // FXAA pass could not run.
        if (pts.fxaa) { p.Use(DTM.tonemap.Get(), kNps); } // FXAA input
        if (pts.resolve)
        {
            p.NextPoint();
            pts.resolveCopy = p.usePoint ? *p.usePoint : 0u;
            // The resolve reads whichever of the two actually produced this frame.
            // The backbuffer is NOT registered: it is driven from outside the graph (present
            // epilogue + RecordBindAndClear both write it with hand-rolled barriers), so the body
            // resolves it with Renderer::TransitionExplicit and the compile models only the source.
            p.Use(pts.fxaa ? DTM.fxaa.Get() : DTM.tonemap.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            p.NextPoint();
            pts.resolveBack = p.usePoint ? *p.usePoint : 0u;
            p.Use(pts.fxaa ? DTM.fxaa.Get() : DTM.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // ...and `tonemap` with it on the FXAA path. The body has always asked for this (a
            // trailing `Transition(tonemap, UAV)`), but no point ever named it, so the request was
            // dropped and the pass left `tonemap` shader-readable instead of at its canonical UAV.
            if (pts.fxaa) { p.Use(DTM.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
        }
        return [this, renderer, pts](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassTonemap);
            Pass_Tonemap(renderer, c, pts);
        };
    });

    // The inspector's preview. Must complete before the overlay draws ImGui, which is what will
    // sample it. The request was left during UI building, which happens before Scene::Render.
    const size_t pDebugPreview = rg.AddPass2(RenderPass::Main_DebugPreview, { pTone },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& DD = renderer->GetDeferredForFrame();
            const Renderer::DebugPreviewRequest& req = renderer->DebugPreviewRequestRef();
            if (req.resource == nullptr || !resources_.GetDebugPreviewMaterial() ||
                resources_.GetDebugPreviewCBSizeBytes() == 0u || DD.debugPreviewUAV.ptr == 0)
            {
                return {};
            }
            ctx.NextPoint();
            const uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            // The SOURCE is whatever the user picked and is deliberately NOT declared: it can be
            // any target, in any of several resting states, and the graph would have to model all
            // of them. It is transitioned explicitly from its canonical and back, the same way the
            // overlay borrows a texture for display.
            ctx.Use(DD.debugPreview.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            ctx.Use(DD.debugPreview.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassDebug);
                Pass_DebugPreview(renderer, c, point);
            };
        });
    (void)pDebugPreview;

    const DebugTexPick debugPick = PickDebugTexTarget(D, frame_->settings.debugTexTarget);
    const bool debugTexOn = frame_->settings.debugTexMode && debugPick.resource != nullptr &&
                            debugPick.srv.ptr != 0;
    // The state this target rests in, which is where the blit has to put it back. Read from the
    // registry rather than assumed, because the list spans targets with different resting states.
    const D3D12_RESOURCE_STATES debugCanon =
        debugPick.resource ? renderer->GetCanonicalState(debugPick.resource)
                           : D3D12_RESOURCE_STATE_COMMON;
    // The blit binds the backbuffer RTV/DSV and draws a triangle, so the only state it needs is the
    // one texture it samples — in a PIXEL shader, which is why it must be declared rather than
    // assumed (it used to be assumed, and got away with it because the shadow atlas happened to be
    // readable). It then puts the target BACK: the overlay's texture inspector transitions out of a
    // resource's CANONICAL state without transitioning back, so every graph pass has to leave its
    // resources where the registry says they rest.
    // pass-flow S6: the pick was already decided once above and captured into BOTH lambdas; now
    // the builder is its single home, and the two points ride with it.
    rg.AddPass2(RenderPass::Main_Debug, { pTone },
        [this, renderer, debugTexOn, debugPick, debugCanon](RenderGraphPassContext& ctx)
            -> std::function<void(RenderGraphPassContext)> {
            if (!debugTexOn) { return {}; }
            DebugBlitPoints pts{};
            pts.read = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(debugPick.resource, kSrvAll);
            ctx.NextPoint();
            pts.restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(debugPick.resource, debugCanon);
            return [this, renderer, debugPick, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassDebug);
                Pass_Debug(renderer, c, debugPick, pts);
            };
        });
    rg.EndCLGroup();
}

