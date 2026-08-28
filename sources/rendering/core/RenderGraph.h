#pragma once
#include <atomic>
#include <array>
#include <functional>
#include <string_view>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "core/diagnostics/DiagPaths.h" // step 7: barrier dump path
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include "rendering/core/Renderer.h"
#include "rendering/core/RendererInvariantFailure.h"
#include "rendering/core/BarrierTranslation.h"
#include "core/task/TaskSystem.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/containers/inl_vector.h"
#include "rendering/core/RenderPass.h"
#include "third_party/robin_hood.h"

// A resource state a pass declares it needs before it runs. The pass body applies them with
// ctx.ApplyDeclaredStates(cl) and its Prepare registers them with ctx.UseDeclared(), so they are
// compiled into real barriers ahead of execution like every other registered use.
struct ResourceStateDecl {
    ID3D12Resource* resource = nullptr;   // null entries are skipped
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
// Capacity 10 was exactly the VSM-mode lighting pass's declaration count, so P6B item 7 adding
// ONE more (the AO target it now samples) overflowed it. inl_vector only ASSERTS on overflow,
// which means Debug aborts and RELEASE SILENTLY CORRUPTS -- the Release build looked fine and
// was writing past the inline storage. Sized with headroom rather than to the current maximum.
using ResourceStateDeclList = tc::inl_vector<ResourceStateDecl, 16>;

// --- Barrier plan, Part A step A.1s (see docs/enhanced_barriers_migration_plan.md) ---
//
// A pass may supply a Prepare callback that runs per frame, SERIALLY, before any
// recording begins, and registers every resource state the pass will need via
// ctx.Use(). The registrations are ORDERED and repeatable: the same resource appears
// once per point at which the pass needs it in a different state, which is the normal
// case (VirtualShadowMap::RecordPageRender moves nine resources through 2-3 states).
// ctx.NextPoint() closes one barrier point and opens the next; the pass body marks the
// same boundaries with ctx.Barrier(cl, n).
//
// At Step 7 these registrations became the ONLY source of barriers: CompileBarriers turns them
// into D3D12_RESOURCE_BARRIER arrays before any body records, and Renderer::Transition emits the
// array for the point the body has reached. Every pass has a Prepare — a pass without one would
// now hit the invariant failure in Renderer::Transition rather than silently lose its barriers.
struct ResourceUse {
    ID3D12Resource*       resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    std::uint32_t         point = 0; // barrier point within the pass
};

// Registrations live in ONE arena per graph, not a list per pass: a per-pass inline
// list costs sizeof(list) x MaxPasses on the stack (a RenderGraph is a stack local in
// SceneRenderer::Render), which at 48 entries/pass added ~36 KB and tripped C6262.
// One append-only arena also matches how the compile reads them — in order.
// Capacity scales with the pass count; Use() hard-fails on overflow because
// inl_vector only asserts, i.e. is UNCHECKED in Release.
// Per-pass budget for both registered uses and observed transitions. 8 was too small —
// Compose declares 8 states alone and its body adds more. Lives entirely in the heap-side
// PrepareState, so raising it costs no stack (see the C6262 note in the plan's Executor Guide).
// 24 in turn was too small for Main_Transparent, whose driver alone performs 15 and whose
// fan-out chunks each add two more.
inline constexpr std::size_t kResourceUsesPerPassBudget = 48;

namespace render {
// Barrier plan step 3: diff every converted pass's registered state usage against what
// its body actually transitioned, and log the differences. DEFAULT OFF — it installs a
// per-thread log around each converted pass body and walks two lists per pass at end of
// frame. Turn it on while converting passes (step 5); it is the check that makes that
// conversion safe, since a Prepare that disagrees with its Record produces wrong barriers
// with no other symptom.
// Pass-flow S1: default ON in Debug so a Prepare/Record mismatch surfaces on ANY editor run,
// not only under --barrier-cmp. It is quiet when the engine is clean (logs only mismatches),
// and Release keeps it off — the observation log + end-of-frame diff are Debug-priced.
#ifdef _DEBUG
inline bool g_barrierComparator = true;
#else
inline bool g_barrierComparator = false;
#endif
// Step 7: one line per frame with the compiled barrier count. DEFAULT OFF.
inline bool g_barrierCompileLog = false;
// Async-compute step 7: `--dump-barriers` writes every compiled barrier ONCE (pass, point,
// resource NAME, before -> after) to logs/barrier_dump.log, for a before/after diff of the compile.
inline bool g_dumpBarriers = false;
} // namespace render

// Debug name for a pass's command list: PIX, GBV and the breadcrumb dump all read it, so the
// list a body records into is named after the pass that opened it. Call right after BeginCL.
// R4: lives here rather than in the SceneRenderer's internal header because the RT AS build
// moved out of sources/app/scene/ and records a pass of its own.
inline void SetCommandListName(ID3D12GraphicsCommandList* cl, RenderPass pass)
{
    const auto nameW = RenderPassToWString(pass);
    if (!nameW.empty() && cl)
    {
        cl->SetName(nameW.data());
    }
}

namespace render {
// Resource DEBUG NAME, not just the pointer: a bare address makes mapping a diagnostic back to a
// `Use` call guesswork. Most engine resources get SetName() at creation; unnamed ones fall back to
// the address.
//
// Takes ID3D12Object, not ID3D12Resource: command lists carry debug names too, and step 6's
// submit-order dump needs exactly this formatting for them. One definition, so the barrier
// comparator, step 5's queue-legality check and that dump can never disagree about a name.
inline void DebugObjectLabel(ID3D12Object* res, char* out, size_t outSize)
{
    out[0] = '\0';
    if (!res) { std::snprintf(out, outSize, "<null>"); return; }
    wchar_t wide[128] = {};
    UINT bytes = sizeof(wide) - sizeof(wchar_t);
    if (SUCCEEDED(res->GetPrivateData(WKPDID_D3DDebugObjectNameW, &bytes, wide)) && wide[0]) {
        std::snprintf(out, outSize, "%ls", wide);
        return;
    }
    std::snprintf(out, outSize, "%p", static_cast<const void*>(res));
}
} // namespace render

struct RenderGraphPassContext {
    // Shared command-list state for a CL group (step 5). Lives on the group
    // task's stack; every member's context points at the same instance so they
    // record into one CL. Opened lazily on the first BeginCL (an all-early-out
    // group emits no CL); the group closes it once after its last member.
    struct GroupCL {
        Renderer::ThreadCL shared{};
        bool opened = false;
    };

    Renderer* renderer = nullptr;
    size_t      batchIndex = (size_t)-1;
    RenderPass  pass{};
    const ResourceStateDeclList* declares = nullptr;
    GroupCL* groupCL = nullptr; // non-null => this pass is a member of a CL group

    // A.1s: the Prepare sink — the owning graph's arena, plus its capacity so Use()
    // can bounds-check without knowing the graph's template parameter. Non-null ONLY
    // while a Prepare callback runs; null during Record, which is what makes a stray
    // Use() there inert instead of appending into another pass's range.
    ResourceUse*  useArena = nullptr;
    std::uint32_t* useCount = nullptr;   // live size of the arena
    std::uint32_t  useCapacity = 0;
    std::uint32_t* usePoint = nullptr;

    // Async-compute step 4 (D1): the queue this pass was registered on. Read-only to the body —
    // a pass cannot change its own queue, because the barrier compile has already run by then.
    RenderQueue queue = RenderQueue::Graphics;

    // The command-list TYPE this pass's queue needs. Its own function so the mapping lives in one
    // place: step 6 submits per queue and step 8 moves the first pass, and both must agree with
    // whatever a body already opened.
    D3D12_COMMAND_LIST_TYPE CommandListType() const
    {
        return (queue == RenderQueue::AsyncCompute) ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                                    : D3D12_COMMAND_LIST_TYPE_DIRECT;
    }

    // Provision the pass's command list. Ungrouped: a fresh list of the pass's own queue type,
    // closed into the batch by EndCL. Grouped: the group's shared list (opened on first call), and
    // EndCL is a no-op — the group closes it after its last member.
    // Pass bodies are agnostic: same shape either way, only the provider differs.
    Renderer::ThreadCL BeginCL(ID3D12PipelineState* initialPSO = nullptr) const
    {
        if (groupCL) {
            // A CL GROUP shares one list, so its members share one queue — the group's, which is
            // DIRECT. Step 5 makes "an AsyncCompute pass inside a CL group" fail fast rather than
            // silently record onto the wrong queue here; until then no pass is async, so this
            // branch is unreachable with anything but Graphics.
            if (!groupCL->opened) {
                groupCL->shared = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT, initialPSO);
                groupCL->opened = true;
            }
            return groupCL->shared;
        }
        return renderer->BeginThreadCommandList(CommandListType(), initialPSO);
    }

    void EndCL(Renderer::ThreadCL& t) const
    {
        if (groupCL) {
            return; // group owns the shared list's lifecycle (closed after last member)
        }
        // Lone list in its own batch -> local order 0.
        renderer->EndThreadCommandList(t, batchIndex, 0);
    }

    // Registers the pass's declared resource states on the given (just-opened)
    // command list. Call once, right after BeginCL, on the command list that
    // must observe the declared states first. In a group each member calls this
    // at its own recording position, so the tracker places intra-CL barriers
    // exactly between members (see step 5d).
    void ApplyDeclaredStates(ID3D12GraphicsCommandList* cl) const
    {
        if (!renderer || !declares || !cl) { return; }
        for (const ResourceStateDecl& decl : *declares) {
            renderer->Transition(cl, decl.resource, decl.state);
        }
    }

    // --- A.1s: Prepare-side registration (valid only inside a Prepare callback) ---

    // Register that this pass needs `resource` in `state` at the current barrier
    // point. Call in the order the pass actually needs them; repeats are expected.
    void Use(ID3D12Resource* resource, D3D12_RESOURCE_STATES state) const
    {
        if (!useArena || !useCount || !resource) { return; }
        if (*useCount >= useCapacity) {
            // Raising the budget is the fix; silently dropping a Use means a missing
            // barrier, which corrupts invisibly. Never downgrade this to an assert.
            RendererInvariantFailure("RenderGraph::Use: resource-use arena exhausted (raise kResourceUsesPerPassBudget)");
        }
        // Async-compute step 5 (D6): queue legality is checked HERE, at registration, where the
        // pass, the resource and the state are all in hand and the message can name all three.
        // Waiting for the debug layer means the failure surfaces as a barrier error on some other
        // resource three passes later — the compile advances its model past a barrier that could
        // never be emitted, and everything downstream of it gets a wrong before-state.
        if (queue == RenderQueue::AsyncCompute) { CheckAsyncQueueLegality(resource, state); }
        useArena[*useCount] = ResourceUse{ resource, state, usePoint ? *usePoint : 0u };
        ++(*useCount);
    }

    // Step 5: the two things an AsyncCompute pass may not register. Out of line so `Use` stays a
    // straight-line append on the path every pass takes.
    void CheckAsyncQueueLegality(ID3D12Resource* resource, D3D12_RESOURCE_STATES state) const;

    // Register everything the pass already lists in its AddPass `declares`. Most bodies
    // open with ApplyDeclaredStates(cl), so this reproduces exactly that — the right
    // starting point when converting a pass. Anything the body transitions BEYOND its
    // declarations still needs its own Use() call; the comparator names those.
    void UseDeclared() const
    {
        if (!declares) { return; }
        for (const ResourceStateDecl& decl : *declares) { Use(decl.resource, decl.state); }
    }

    // Close the current barrier point and open the next. The pass body emits the
    // matching boundary with Barrier(cl, n) using the same index.
    void NextPoint() const
    {
        if (usePoint) { ++(*usePoint); }
    }

    // --- A.1s: Record-side emission (DORMANT) ---

    // Will replay the precomputed barrier array compiled for (this pass, point).
    // A.1s emits nothing: passes still transition through Renderer::Transition, so
    // adding a call here today changes no behavior. A.2s builds the arrays and
    // compares them against the tracker; A.6s makes them authoritative.
    void Barrier(ID3D12GraphicsCommandList* cl, std::uint32_t point) const
    {
        (void)cl;
        (void)point;
    }
};

// Async-compute plan step 5 (D5/D6) — what an AsyncCompute pass may not register.
//
// Both rules fail FAST rather than logging, because there is no safe way to continue: a compiled
// barrier that the compute queue cannot legally emit leaves the compile's model one transition
// ahead of the GPU, and every later user of that resource gets a wrong before-state. That is
// silent corruption, so it must stop at the registration that caused it, naming it.
inline void RenderGraphPassContext::CheckAsyncQueueLegality(ID3D12Resource* resource,
                                                            D3D12_RESOURCE_STATES state) const
{
    char label[160];
    render::DebugObjectLabel(resource, label, sizeof(label));

    // (a) Queue-illegal state (R10). The engine's DEFAULT read state `kSrvAll` carries the PIXEL
    // bit and is used at 32 sites, so this is the rule that will actually fire when a pass moves.
    if (barriers::IsDirectQueueExclusiveState(state)) {
        char msg[320];
        std::snprintf(msg, sizeof(msg),
                      "RenderGraph::Use: pass '%ls' is on the ASYNC COMPUTE queue but registers "
                      "res=%s in state 0x%X, which only the DIRECT queue can carry "
                      "(RENDER_TARGET / DEPTH_WRITE / DEPTH_READ / PIXEL_SHADER_RESOURCE). "
                      "Declare NON_PIXEL only here and let the graphics consumer acquire the "
                      "PIXEL half (design D7).",
                      RenderPassToWString(pass).data(), label, static_cast<unsigned>(state));
        RendererInvariantFailure(msg);
    }

    // (b) The swapchain. Present is a direct-queue affair and the backbuffer is not compile-managed
    // at all, so an async pass touching it would produce no barrier and no error — the worst
    // combination. Checked explicitly rather than left to (a): PRESENT is not a direct-exclusive
    // BIT, it is state 0.
    if (renderer && resource != nullptr && resource == renderer->GetCurrentBackbuffer()) {
        char msg[288];
        std::snprintf(msg, sizeof(msg),
                      "RenderGraph::Use: pass '%ls' is on the ASYNC COMPUTE queue and registers the "
                      "SWAPCHAIN backbuffer (res=%s). Presentation belongs to the direct queue "
                      "(design D5).",
                      RenderPassToWString(pass).data(), label);
        RendererInvariantFailure(msg);
    }
}

template <std::size_t MaxPasses>
class RenderGraph {
public:
    using PassContext = RenderGraphPassContext;

    using ExecFn = std::function<void(PassContext)>;
    // A.1s: runs serially before any recording; registers state usage via ctx.Use().
    using PrepareFn = std::function<void(PassContext&)>;

    explicit RenderGraph(size_t submitBatchIndex = (size_t)-1)
        : submitBatchIndex_(submitBatchIndex) {
        static_assert(MaxPasses > 0, "RenderGraph must allow at least one pass");
    }

    static constexpr size_t kMinAdjacencyBucketSize = 4;
    static constexpr size_t kAdjacencyCapacity =
        (MaxPasses <= kMinAdjacencyBucketSize)
            ? MaxPasses
            : ((MaxPasses / kMinAdjacencyBucketSize) < kMinAdjacencyBucketSize
                    ? size_t{kMinAdjacencyBucketSize}
                    : MaxPasses / kMinAdjacencyBucketSize);

    using SuccessorList = tc::inl_vector<size_t, kAdjacencyCapacity>;

    static constexpr size_t kPassDependencyCapacity = 4;
    using DependencyList = tc::inl_vector<size_t, kPassDependencyCapacity>;

    static constexpr size_t kNoGroup = (size_t)-1;
    static constexpr size_t kMaxGroupMembers = 8;
    using GroupMemberList = tc::inl_vector<size_t, kMaxGroupMembers>;

    struct Pass {
        RenderPass name{};
        DependencyList prereqs; // batch opening order (DAG)
        ExecFn exec;     // pass body
        DependencyList mtDeps;  // runtime dependencies (which passes must complete)
        SuccessorList successors;
        ResourceStateDeclList declares; // resource states the pass needs on entry
        size_t groupId = kNoGroup;      // CL group this pass belongs to (kNoGroup = ungrouped)
        // Pass-flow S9: authored through AddPass2, i.e. its declarations and its body come from
        // ONE builder. Asserted for every pass of a barrier-compiling graph — see RunPrepares.
        bool builtByBuilder = false;
        // Async-compute step 4 (D1): which queue this pass records onto. Fixed at graph-build time
        // and never changed afterwards. Every pass is Graphics until step 8.
        RenderQueue queue = RenderQueue::Graphics;
    };

    // A.1s: everything the two-phase path needs, kept OFF the Pass array and off the
    // stack. `SceneRenderer::Render` builds its RenderGraph as a local and already sits
    // at ~16.2 KB against C6262's 16 KB threshold, so `Pass` has no room left: adding
    // one std::function per pass (64 B x MaxPasses) plus an inline arena tripped the
    // warning immediately. This block is heap-allocated lazily by SetPassPrepare, so a
    // graph with no Prepare (every graph, at A.1s) costs exactly one pointer.
    //
    // NOTE for A.4s: the first real conversion makes this allocate once per graph
    // construction, i.e. once per frame. Move the render graphs off the stack (own them
    // on SceneRenderer) BEFORE converting passes, or that allocation lands on the hot path.
    struct PrepareState {
        struct Slice {
            std::uint32_t begin = 0;  // into `arena`
            std::uint32_t count = 0;
            std::uint32_t points = 0; // barrier points this pass declared
        };
        std::array<PrepareFn, MaxPasses> fns{};
        std::array<Slice, MaxPasses>     slices{};
        ResourceUse   arena[MaxPasses * kResourceUsesPerPassBudget]{};
        std::uint32_t arenaSize = 0;

        // Step 3 comparator: what each converted pass's body ACTUALLY transitioned.
        // Per-pass storage rather than one arena, because bodies run concurrently —
        // each pass writes only its own slot, so no synchronisation is needed.
        struct Observed {
            Renderer::ObservedTransition entries[kResourceUsesPerPassBudget]{};
            std::atomic<std::uint32_t> count{ 0 }; // fan-out workers append concurrently
            bool ran = false; // the body executed (an early-out pass registers but does nothing)
            // The thread log the body installs. Lives HERE, not on RunPassBody's stack:
            // RenderObjectBatch fans out with DispatchTrack (fire-and-track, joined only at
            // the frame's WaitForTrackedAsyncTasks), so a worker can still be appending long
            // after the body returned. A stack-local log was a use-after-free that crashed
            // inside GpuInstancedModels::Render the moment Main_GBuffer got a Prepare.
            Renderer::TransitionLog log{};
        };
        std::array<Observed, MaxPasses> observed{};
        // Step 7: per-pass view of the compiled barriers. HEAP-side for the same reason the
        // TransitionLog is: RenderObjectBatch fans out with DispatchTrack, so a worker can
        // still be emitting after the body returned. A stack local here crashed immediately.
        std::array<Renderer::CompiledBarriers, MaxPasses> passBarriers{};

        // --- Step 7: the compile ---
        //
        // Turns the registered uses into the barriers a pass must emit at each of its barrier
        // points, seeded from the CANONICAL table rather than any live map — that is the whole
        // reason Step 6 existed. Produced once per frame after every Prepare has run and before
        // any body records, so it is single-threaded by construction.
        //
        // One flat arena plus a {begin,count} slice per (pass, point): a pass can declare at most
        // one point per registered use, so the slice grid is bounded by the same budget.
        struct BarrierSlice { std::uint32_t begin = 0, count = 0; };
        D3D12_RESOURCE_BARRIER barrierArena[MaxPasses * kResourceUsesPerPassBudget]{};
        // One claim flag per arena entry: a barrier is emitted at most once even when the
        // pass records from several fan-out threads. Reset with the compile, not per body.
        // One Point record per (pass, point): the emit unit of the flip.
        std::array<std::array<Renderer::CompiledBarriers::Point, kResourceUsesPerPassBudget>, MaxPasses> barrierPointViews{};
        std::uint32_t barrierCount = 0;
        std::array<std::array<BarrierSlice, kResourceUsesPerPassBudget>, MaxPasses> barrierPoints{};
        bool compiled = false;

        // --- Cross-frame cache of the compile's OUTPUT (see CompileBarriers) ---------------
        //
        // The compile is a pure function of (compile order, registered uses, the incoming
        // `predicted` state of each resource it first touches, and which resources are compile-
        // managed). The graph is rebuilt every frame but almost always with the same shape, so
        // the same inputs produce the same barriers frame after frame.
        //
        // The key is an EXACT COPY of the inputs, not a hash. A hash collision here would serve
        // barriers with wrong before-states — silent GPU corruption — and `arenaSize` is a few
        // hundred 16-byte entries, so a memcmp of the copy is cheaper than hashing it element by
        // element anyway.
        //
        // ONE SLOT PER FRAME IN FLIGHT, and that is not an optimisation — it is the difference
        // between a cache and a decoration. Almost every pass registers `GetDeferredForFrame()`
        // targets, and there are kFrameCount rotating Deferred sets, so consecutive frames register
        // DIFFERENT resource pointers and a single-slot cache measured a 0 % hit rate on the main
        // graph. Frame N matches frame N-kFrameCount, not frame N-1.
        // Step 7: the QUEUE joins the cache key. Two frames whose uses and order match byte for
        // byte but whose pass ran on different queues do NOT produce the same barriers, so a key
        // without it would serve one frame's barriers to the other — silent corruption.
        struct CompiledPass { size_t index; RenderPass name; Slice slice; RenderQueue queue; };
        struct CacheSlot {
            std::vector<CompiledPass> order;    // compile order + each pass's slice
            std::vector<ResourceUse>  uses;     // arena[0, arenaSize) verbatim
            // The compact output. Only the USED prefix of the arena is kept (~100 barriers), so a
            // hit is two small memcpys rather than a second 147 KB arena per slot.
            std::vector<D3D12_RESOURCE_BARRIER> barriers;
            std::array<std::array<BarrierSlice, kResourceUsesPerPassBudget>, MaxPasses> points{};
            std::uint64_t generation = 0;       // CanonicalStateRegistry::Generation()
            bool          valid = false;
        };
        std::array<CacheSlot, render::kFrameCount> cache{};
        std::uint32_t cacheHits = 0;            // diagnostics only (--barrier-compile-log)
        std::uint32_t cacheMisses = 0;
        std::uint32_t cacheNotFixedPoint = 0;   // compiled, but its own output forbids reuse
        // --barrier-cache-verify: what the cache WOULD have served this frame, kept so the fresh
        // compile can be compared against it byte for byte.
        std::vector<D3D12_RESOURCE_BARRIER> verifyBarriers;
        std::vector<BarrierSlice>           verifyPoints;
    };

    // Convenience AddPass: treat all prereqs as mt-deps (flag) or specify mtDeps explicitly
    size_t AddPass(RenderPass name,
        std::initializer_list<size_t> prereqs,
        ExecFn fn)
    {
        CPU_SCOPE(ProfilerScopes::kAddPass);
        return AddPassInternal(name, prereqs, std::initializer_list<size_t>{}, {}, std::move(fn));
    }

    size_t AddPass(RenderPass name,
        const DependencyList& prereqs,
        ExecFn fn)
    {
        CPU_SCOPE(ProfilerScopes::kAddPass);
        return AddPassInternal(name, prereqs, DependencyList{}, {}, std::move(fn));
    }

    // With resource declarations: the pass body calls ctx.ApplyDeclaredStates(cl)
    // on its main command list instead of issuing manual Transition calls.
    size_t AddPass(RenderPass name,
        std::initializer_list<size_t> prereqs,
        std::initializer_list<ResourceStateDecl> declares,
        ExecFn fn)
    {
        CPU_SCOPE(ProfilerScopes::kAddPass);
        return AddPassInternal(name, prereqs, std::initializer_list<size_t>{}, declares, std::move(fn));
    }

    // Overload with explicit mt-deps (more precise)
    size_t AddPassMT(RenderPass name,
        std::initializer_list<size_t> prereqs,
        std::initializer_list<size_t> mtDeps,
        ExecFn fn)
    {
        return AddPassInternal(name, prereqs, mtDeps, {}, std::move(fn));
    }

    size_t AddPassMT(RenderPass name,
        std::initializer_list<size_t> prereqs,
        std::initializer_list<size_t> mtDeps,
        std::initializer_list<ResourceStateDecl> declares,
        ExecFn fn)
    {
        return AddPassInternal(name, prereqs, mtDeps, declares, std::move(fn));
    }

    size_t AddPassMT(RenderPass name,
        const DependencyList& prereqs,
        const DependencyList& mtDeps,
        ExecFn fn)
    {
        return AddPassInternal(name, prereqs, mtDeps, {}, std::move(fn));
    }

    // Pass-flow S2 (docs/render_graph_pass_flow_plan.md): the RDG authoring shape — ONE builder
    // per pass instead of a Prepare/Record pair kept in sync by discipline. The builder runs at
    // Prepare time (serially, before any recording): it makes every frame decision as a LOCAL,
    // declares points/uses from those locals via ctx.Use()/ctx.NextPoint(), may commit
    // cross-frame state (ping-pong indices) immediately, and returns the record lambda capturing
    // those locals BY VALUE. Prepare and Record cannot disagree because they are the same
    // values; combined with EmitPoint markers (S1) the body names no resource and no state.
    // Returning an empty ExecFn means "this pass does nothing this frame" — the body becomes a
    // no-op, consistent with whatever the builder declared (normally nothing) before bailing.
    //
    //   rg.AddPass2(RenderPass::X, { prev }, [this](PassContext& ctx) -> ExecFn {
    //       const bool work = WillWork();                  // decision, once
    //       if (!work) { return {}; }
    //       ctx.NextPoint();
    //       const std::uint32_t point = *ctx.usePoint;     // for EmitPoint markers
    //       ctx.Use(res, state);
    //       return [this, work, point](PassContext c) {
    //           /* open a CL via c, then c.renderer->EmitPoint(cl, point) and record */
    //       };
    //   });
    //
    // Implementation note: the pass is created with a placeholder body because RunPrepareOne
    // skips a pass whose exec is empty; the builder's return REPLACES it. RunPrepares is serial
    // and runs before any record task exists, so writing passes_[idx].exec there is safe.
    using BuildFn = std::function<ExecFn(PassContext&)>;

    size_t AddPass2(RenderPass name, std::initializer_list<size_t> prereqs, BuildFn builder)
    {
        return AddPass2Internal(name, prereqs, std::initializer_list<size_t>{}, {},
            std::move(builder), RenderQueue::Graphics);
    }

    size_t AddPass2(RenderPass name,
        std::initializer_list<size_t> prereqs,
        std::initializer_list<size_t> mtDeps,
        std::initializer_list<ResourceStateDecl> declares,
        BuildFn builder)
    {
        return AddPass2Internal(name, prereqs, mtDeps, declares, std::move(builder),
            RenderQueue::Graphics);
    }

    // --- Async-compute step 4 (D1): the same two overloads, with an explicit QUEUE ---
    //
    // Separate overloads rather than a defaulted trailing parameter: `builder` is the last argument
    // and is always a lambda, so a trailing default would put the queue AFTER a multi-line lambda
    // at every call site that wanted it — unreadable, and easy to attach to the wrong pass. Putting
    // the queue up front next to the pass name keeps it where the reader looks for pass identity.
    //
    // NOTHING calls these at step 4. That is the step's deliverable: the enum is threaded through
    // and provably unused (grep for `RenderQueue::AsyncCompute` outside the graph itself).
    size_t AddPass2(RenderPass name, RenderQueue queue,
        std::initializer_list<size_t> prereqs, BuildFn builder)
    {
        return AddPass2Internal(name, prereqs, std::initializer_list<size_t>{}, {},
            std::move(builder), queue);
    }

    size_t AddPass2(RenderPass name, RenderQueue queue,
        std::initializer_list<size_t> prereqs,
        std::initializer_list<size_t> mtDeps,
        std::initializer_list<ResourceStateDecl> declares,
        BuildFn builder)
    {
        return AddPass2Internal(name, prereqs, mtDeps, declares, std::move(builder), queue);
    }

    // Command-list group brackets (step 5). Passes added between Begin/End share
    // ONE command list and run as ONE schedulable node (one batch, one task,
    // members executed in declaration order). Members must form a contiguous
    // prereq chain; only the first member may have prereqs from outside the
    // group (asserted in EndCLGroup). Use for short single-dispatch passes whose
    // per-CL overhead dominates their recording cost — measure one group at a
    // time. Fan-out passes (drivers, per-chunk/cascade/light workers) are never
    // groupable and must keep calling the renderer directly.
    void BeginCLGroup()
    {
        assert(currentGroup_ == kNoGroup && "nested CL groups are not supported");
        assert(groupCount_ < MaxPasses && "CL group capacity exceeded");
        currentGroup_ = groupCount_++;
        groups_[currentGroup_].clear_fast();
    }

    void EndCLGroup()
    {
        assert(currentGroup_ != kNoGroup && "EndCLGroup without BeginCLGroup");
        const GroupMemberList& members = groups_[currentGroup_];
        assert(members.size() >= 1 && "empty CL group");
        // Contiguity: every member after the first may only depend on earlier
        // members of the same group (no external prereq into the middle of a
        // group, which would let other work interleave between members).
        for (size_t i = 1; i < members.size(); ++i) {
            const Pass& m = passes_[members[i]];
            for (size_t pr : m.prereqs) {
                bool inGroup = false;
                for (size_t mm : members) { if (mm == pr) { inGroup = true; break; } }
                assert(inGroup && "grouped pass (non-first) has a prereq from outside the group");
                (void)inGroup;
            }
        }
        currentGroup_ = kNoGroup;
    }

    struct FlatNode { size_t pass; size_t batch; };

    // Legacy path: sequential execution in place
    void Execute(Renderer* renderer)
    {
        CPU_SCOPE(ProfilerScopes::kRenderGraphExecute);
        if (renderer == nullptr) { return; }
        // Without Prepares this path runs pass bodies INLINE during the traversal — one
        // walk, nothing to order. That is what the inner G-buffer/transparent graphs use.
        if (!prepare_) {
            Unroll(renderer, /*executeInplace=*/true, nullptr);
            return;
        }
        // With Prepares it must be two-phase for the same reason ExecuteParallel is: every
        // registration has to land before the first body records, or a pass's barriers would
        // be compiled from a half-built list. Same Unroll, just not in place.
        tc::inl_vector<FlatNode, MaxPasses> schedule;
        Unroll(renderer, /*executeInplace=*/false, &schedule);
        if (schedule.empty()) { return; }
        RunPrepares(renderer, schedule);
        for (const FlatNode& n : schedule) {
            RunNode(renderer, n.pass, n.batch);
        }
        ReportComparator();
    }

    // Parallel path: create batches in topological order, then
    // submit actual pass tasks that wait on their mt-deps.
    void ExecuteParallel(Renderer* renderer, TaskSystem& tasks)
    {
        CPU_SCOPE(ProfilerScopes::kRenderGraphExecuteParallel);
#if !TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
        (void)tasks;
        Execute(renderer);
        return;
#endif
        tc::inl_vector<FlatNode, MaxPasses> schedule;
        Unroll(renderer, /*executeInplace=*/false, &schedule);
        if (schedule.empty()) { return; }

        // A.1s: every Prepare runs here — after the schedule (and therefore the batch
        // indices) exist, and before a single task is created, i.e. before any
        // recording. Serial by construction. Dormant until A.2s consumes the result.
        RunPrepares(renderer, schedule);

        const size_t N = passesNum_;

        // schedule holds one entry per NODE (group owner or singleton). passDone
        // is keyed by pass index; only owners carry a handle, so a wait/release
        // walk over schedule touches each task exactly once.
        tc::inl_vector<TaskSystem::TaskHandle, MaxPasses> passDone;
        passDone.resize(N, nullptr);

        DependencyList nodeDeps;

        // create all tasks first (a group runs all its members in one task)
        for (const auto& n : schedule) {
            const size_t ownerIdx = n.pass;
            const size_t batch = n.batch;
            CollectNodeDeps(ownerIdx, nodeDeps);
            auto handle = tasks.CreateTask([this, renderer, ownerIdx, batch]() {
                RunNode(renderer, ownerIdx, batch);
            }, nodeDeps.size());
            passDone[ownerIdx] = handle;
        }

        tc::inl_vector<TaskSystem::TaskHandle, kPassDependencyCapacity> deps;
        // set up dependencies between created tasks (member deps resolve to owners)
        for (const auto& n : schedule) {
            const size_t ownerIdx = n.pass;
            CollectNodeDeps(ownerIdx, nodeDeps);
            deps.clear();
            for (size_t dep : nodeDeps) {
                if (passDone[dep]) {
                    deps.push_back(passDone[dep]);
                }
            }
            tasks.SetDependencies(passDone[ownerIdx], deps.data(), deps.size());
        }

        // finally submit nodes with no runtime dependencies
        for (const auto& n : schedule) {
            const size_t ownerIdx = n.pass;
            CollectNodeDeps(ownerIdx, nodeDeps);
            if (nodeDeps.size() == 0) {
                tasks.Submit(passDone[ownerIdx]);
            }
        }

        for (const auto& n : schedule) {
            if (passDone[n.pass]) {
                tasks.Wait(passDone[n.pass]);
            }
        }
        for (size_t i = schedule.size(); i-- > 0;) {
            tasks.Release(passDone[schedule[i].pass]);
        }

        // Every body has finished — but bodies that fan out with DispatchTrack may still have
        // workers appending observations, and those are joined only at the frame's
        // WaitForTrackedAsyncTasks. Pull that wait forward when the comparator is on so the
        // diff sees a complete buffer; the frame's own wait then returns immediately. Costs
        // nothing with the flag off, which is the default.
        if (render::g_barrierComparator) { tasks.WaitForTrackedAsyncTasks(); }
        ReportComparator();
    }

    void Clear()
    {
        //passes_.clear();
        for (auto& pending : pendingSuccessors_) {
            pending.clear_fast();
        }
    }

    // Reuse this graph for another frame instead of constructing a new one.
    //
    // WHY: a graph is MaxPasses x Pass, and Pass carries a std::function plus several
    // inline vectors — the main graph is ~16 KB. Built as a local it puts that on the
    // caller's stack every frame (C6262 fired on SceneRenderer::Render at ~16.2 KB, with
    // a 16 KB threshold), and rebuilding it also re-constructs every std::function. An
    // owner keeps one on the heap and calls Reset() per frame instead.
    //
    // Clears everything AddPass / BeginCLGroup build up, INCLUDING the pass bodies: they
    // capture that frame's data, and a reused graph would otherwise hold stale captures
    // between frames. Call at the START of building a frame's graph.
    void Reset(size_t submitBatchIndex = (size_t)-1)
    {
        for (size_t i = 0; i < passesNum_; ++i) {
            Pass& p = passes_[i];
            p.name = RenderPass{};
            p.prereqs.clear_fast();
            p.exec = nullptr;      // release the frame captures
            p.mtDeps.clear_fast();
            p.successors.clear_fast();
            p.declares.clear_fast();
            p.groupId = kNoGroup;
            p.queue = RenderQueue::Graphics; // step 4: a reused graph must not inherit a queue
        }
        // Pending edges and groups are indexed independently of passesNum_, so clear all.
        for (auto& pending : pendingSuccessors_) { pending.clear_fast(); }
        for (auto& group : groups_) { group.clear_fast(); }
        passesNum_ = 0;
        groupCount_ = 0;
        currentGroup_ = kNoGroup;
        submitBatchIndex_ = submitBatchIndex;
        if (prepare_) {
            // Keep the allocation (that is the point of reuse), drop last frame's content.
            for (auto& fn : prepare_->fns) { fn = nullptr; }
            for (auto& slice : prepare_->slices) { slice = {}; }
            prepare_->arenaSize = 0;
        }
    }

private:
    // The owner of a pass's node: itself if ungrouped, else its group's first
    // member. Tasks, batches, and dependency edges are keyed by owner.
    size_t OwnerOf(size_t passIdx) const
    {
        const size_t gid = passes_[passIdx].groupId;
        return (gid == kNoGroup) ? passIdx : groups_[gid][0];
    }

    // A.1s: run one pass's Prepare into its own registration list. Cleared first, so
    // a pass whose Prepare early-outs this frame cannot inherit last frame's uses.
    void RunPrepareOne(Renderer* renderer, size_t passIdx, size_t batch)
    {
        PrepareState& ps = *prepare_;
        auto& slice = ps.slices[passIdx]; // `typename PrepareState::Slice&` here trips MSVC's
        slice = { ps.arenaSize, 0u, 0u };  // dependent-name parse; auto sidesteps it
        const PrepareFn& fn = ps.fns[passIdx];
        if (!fn || !passes_[passIdx].exec) { return; }
        PassContext ctx;
        ctx.renderer = renderer;
        ctx.batchIndex = batch;
        ctx.pass = passes_[passIdx].name;
        ctx.declares = &passes_[passIdx].declares;
        ctx.groupCL = nullptr;   // Prepare records nothing; there is no command list yet
        ctx.queue = passes_[passIdx].queue; // step 4: the builder sees its own queue
        ctx.useArena = ps.arena;
        ctx.useCount = &ps.arenaSize;
        ctx.useCapacity = static_cast<std::uint32_t>(std::size(ps.arena));
        ctx.usePoint = &slice.points;
        fn(ctx);
        slice.count = ps.arenaSize - slice.begin;
        ++slice.points; // points = count; NextPoint() only closed the earlier ones
    }

    // A.1s: all Prepares, serially, in schedule order — group members in declaration
    // order, mirroring RunNode so registration order matches recording order. No graph
    // has a Prepare at A.1s, so this returns immediately and costs one null check.
    void RunPrepares(Renderer* renderer, const tc::inl_vector<FlatNode, MaxPasses>& schedule)
    {
        if (!prepare_) { return; }
        CPU_SCOPE(ProfilerScopes::kRenderGraphPrepares);
        // Pass-flow S9: in a graph that COMPILES barriers, every pass must be authored with
        // AddPass2 — declarations and body from one builder. A pass added the old way would
        // declare nothing while its body transitions, which is the FATAL direction (a barrier the
        // compile never registered simply is not emitted). Graphs without a Prepare block (the
        // inner G-buffer/transparent graphs, whose states belong to their outer pass) never reach
        // here, which is exactly the intended exemption.
#ifndef NDEBUG
        for (size_t i = 0; i < passesNum_ && i < MaxPasses; ++i) {
            assert((!passes_[i].exec || passes_[i].builtByBuilder) &&
                   "pass-flow S9: a barrier-compiling graph takes AddPass2 passes only");
        }
#endif
        prepare_->arenaSize = 0; // one arena per frame; slices handed out in schedule order
        for (const FlatNode& n : schedule) {
            const size_t gid = passes_[n.pass].groupId;
            if (gid == kNoGroup) {
                RunPrepareOne(renderer, n.pass, n.batch);
                continue;
            }
            for (size_t m : groups_[gid]) { RunPrepareOne(renderer, m, n.batch); }
        }
        CompileBarriers(renderer, schedule);
    }

    // Step 7: compile every registered use into the barriers its pass must emit.
    //
    // Walks the schedule in the SAME order RunPrepares just did — which is also the order the
    // bodies will record in — carrying a running state per resource. A use whose required state
    // already matches produces nothing; anything else produces one transition and advances the
    // running state. The seed is the canonical table (Step 6), so there is no cross-frame carry
    // and a graph that changes shape frame to frame (DLSS on/off, VSM vs Legacy, editor) is
    // handled without a live map.
    //
    // DORMANT: nothing reads `barrierArena` yet — `ctx.Barrier` is still a no-op and the tracker
    // still emits the real barriers. This step is judged on the compiled counts being sane and on
    // the flip afterwards, not on any behaviour change here.
    void CompileBarriers(Renderer* renderer, const tc::inl_vector<FlatNode, MaxPasses>& schedule)
    {
        if (!prepare_ || !renderer) { return; }
        CPU_SCOPE(ProfilerScopes::kRenderGraphCompileBarriers);

        // --- Cache lookup ---------------------------------------------------------------------
        // Sound iff nothing the cached compile READ has changed. Three inputs, three guards:
        //   uses + compile order -> compared byte for byte (CompileInputsUnchanged)
        //   which resources are declared / compile-managed -> the registry generation
        //   the incoming `predicted` of every resource first touched -> ALSO the generation,
        //       because SetPredicted only bumps it when a value actually moves.
        // The third is the one that is easy to get wrong: reuse spans kFrameCount frames, so it is
        // not enough that THIS slot's compile was a fixed point — a different slot's compile could
        // have moved a shared resource in between. The generation covers that; the fixed-point test
        // below is kept as a second, conservative gate.
        //
        // A hit also SKIPS the `predicted` write-back, which is sound for the same reason: the
        // values it would write are the ones already there.
        const UINT slotIndex = renderer->GetCurrentFrameIndex();
        if (slotIndex >= render::kFrameCount) { return; } // no slot to key on: refuse to cache
        auto& slot = prepare_->cache[slotIndex];
        const std::uint64_t generation = renderer->DeclarationsGeneration();
        const bool inputsMatch = slot.valid && slot.generation == generation &&
                                 CompileInputsUnchanged(schedule, slot);
        if (inputsMatch) {
            if (!render::g_barrierCacheVerify) {
                RestoreCachedOutput(slot);
                ++prepare_->cacheHits;
                return;
            }
            // --barrier-cache-verify: keep what the cache WOULD have served, compile for real, diff.
            prepare_->verifyBarriers = slot.barriers;
            prepare_->verifyPoints.clear();
            for (size_t p = 0; p < MaxPasses; ++p) {
                for (const auto& sl : slot.points[p]) { prepare_->verifyPoints.push_back(sl); }
            }
        }
        ++prepare_->cacheMisses;

        prepare_->barrierCount = 0;
        prepare_->compiled = false;
        // `row.fill(PrepareState::BarrierSlice{})` trips MSVC's dependent-name parse inside the
        // class template (same trap as `typename PrepareState::Slice&` back in Step 1); `{}` is fine.
        for (auto& row : prepare_->barrierPoints) { row.fill({}); }

        // Running state per resource for THIS frame only. Seeded lazily from canonical the first
        // time a resource is touched, which is what makes the table static and the map disposable.
        compileState_.clear();
        returnBarrierEstimate_ = 0;

        // Measurement for the "every pass is self-contained" design (the fix for the ordering
        // blocker): if each pass returned what it touched to canonical, how many EXTRA barriers
        // would that cost? Counted here, emitted nowhere — the number decides whether that design
        // is affordable before any of it is written.
        passTouched_.clear();

        auto compileOne = [&](size_t passIdx) {
            passTouched_.clear();
            const auto& slice = prepare_->slices[passIdx];
            const RenderQueue passQueue = passes_[passIdx].queue;
            for (std::uint32_t i = 0; i < slice.count; ++i) {
                const ResourceUse& use = prepare_->arena[slice.begin + i];
                if (!use.resource || use.point >= kResourceUsesPerPassBudget) { continue; }
                // Not ours to model: driven from outside the graph (the backbuffer), or never
                // declared at all. The SAME predicate Renderer::Transition uses to decide whether
                // to emit a compiled barrier or fall back to the tracker — the two disagreeing is
                // a barrier compiled but never emitted, i.e. a running state ahead of the GPU.
                if (!renderer->IsResourceCompileManaged(use.resource)) { continue; }
                auto it = compileState_.find(use.resource);
                const D3D12_RESOURCE_STATES before =
                    (it == compileState_.end()) ? renderer->GetPredictedState(use.resource) : it->second.state;
                const RenderQueue owner =
                    (it == compileState_.end()) ? renderer->GetPredictedOwner(use.resource) : it->second.owner;

                // --- Async-compute step 7 (D3/D7): OWNERSHIP TRANSFER ---
                //
                // The state is one thing; WHO LEFT IT THERE is another, and the pair is what decides
                // whether this use is expressible. Crossing queues is legal only if the state the
                // producer left behind is legal on the CONSUMER's queue too — that is D7, and it is
                // why D3's release half is normally a no-op.
                //
                // When it is not, the fix belongs in the PRODUCING pass, not here: it declares one
                // more `Use` handing the resource over in a both-legal state, which the compile then
                // emits on the producer's own (graphics) list like any other barrier. The compile
                // deliberately does NOT invent that barrier itself — it would have to be inserted
                // into a pass the walk has already gone past, i.e. a second compile pass, to fix
                // something a one-line declaration fixes at the source.
                if (owner != passQueue && renderer->IsResourceCompileManaged(use.resource)) {
                    const bool illegalForConsumer =
                        (passQueue == RenderQueue::AsyncCompute) &&
                        barriers::IsDirectQueueExclusiveState(before);
                    if (illegalForConsumer) {
                        char label[160];
                        render::DebugObjectLabel(use.resource, label, sizeof(label));
                        char msg[416];
                        std::snprintf(msg, sizeof(msg),
                                      "RenderGraph::CompileBarriers: pass '%ls' (ASYNC COMPUTE) takes "
                                      "res=%s from the graphics queue, which left it in 0x%X — a "
                                      "DIRECT-queue-only state. The PRODUCING pass must hand it over "
                                      "in a state legal on both queues (design D7): add a final "
                                      "ctx.Use(res, NON_PIXEL...) there.",
                                      RenderPassToWString(passes_[passIdx].name).data(), label,
                                      static_cast<unsigned>(before));
                        RendererInvariantFailure(msg);
                    }
                }

                if (before == use.state && owner == passQueue) { continue; } // already there — no barrier
                if (before == use.state) {
                    // Same state, different queue: no TRANSITION is needed (D7 guaranteed the state
                    // is legal on both), but ownership still moves — record it so the next consumer
                    // sees the right owner.
                    compileState_[use.resource] = TrackedState{ use.state, passQueue };
                    passTouched_[use.resource] = use.state;
                    continue;
                }
                if (prepare_->barrierCount >= std::size(prepare_->barrierArena)) {
                    RendererInvariantFailure("RenderGraph::CompileBarriers: barrier arena exhausted");
                }
                D3D12_RESOURCE_BARRIER& b = prepare_->barrierArena[prepare_->barrierCount];
                b = D3D12_RESOURCE_BARRIER{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = use.resource;
                b.Transition.StateBefore = before;
                b.Transition.StateAfter = use.state;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                auto& pt = prepare_->barrierPoints[passIdx][use.point];
                // Points are filled in ascending order within a pass, so a slice is contiguous.
                if (pt.count == 0) { pt.begin = prepare_->barrierCount; }
                ++pt.count;
                ++prepare_->barrierCount;
                compileState_[use.resource] = TrackedState{ use.state, passQueue };
                passTouched_[use.resource] = use.state;
            }
            for (const auto& kv : passTouched_) {
                if (kv.second != renderer->GetCanonicalState(kv.first)) { ++returnBarrierEstimate_; }
            }
        };

        for (const FlatNode& n : schedule) {
            const size_t gid = passes_[n.pass].groupId;
            if (gid == kNoGroup) { compileOne(n.pass); continue; }
            for (size_t m : groups_[gid]) { compileOne(m); }
        }
        // Hand this graph's ending states to the next compile — the main graph's to the epilogue's,
        // this frame's to the next frame's. See CanonicalStateRegistry::Entry::predicted for why
        // re-seeding from canonical every frame was wrong.
        //
        // The same loop decides whether this compile may be CACHED. Reusing its barriers next frame
        // means next frame's incoming states must equal THIS frame's incoming states — i.e. the
        // compile must be a FIXED POINT: every resource it touched has to end where it began.
        // `GetPredictedState` still returns the incoming value here, because the write-back happens
        // in this very loop, so the test costs one lookup per touched resource and no extra memory.
        //
        // If it is not a fixed point the compile still runs normally; only the caching is refused.
        // That is the conservative direction: a wrongly-refused cache costs 0.01 ms, a wrongly-
        // accepted one emits barriers whose before-state the GPU has already moved past.
        // Step 7: the fixed-point test is now over (state, OWNER). A resource that ends in the same
        // state but owned by the other queue is NOT a fixed point — next frame's compile would seed
        // from a different starting point, and reusing this frame's barriers over that is exactly
        // the silent corruption the test exists to refuse.
        bool fixedPoint = true;
        for (const auto& kv : compileState_) {
            if (kv.second.state != renderer->GetPredictedState(kv.first) ||
                kv.second.owner != renderer->GetPredictedOwner(kv.first)) {
                fixedPoint = false;
            }
            renderer->SetPredictedState(kv.first, kv.second.state, kv.second.owner);
        }
        prepare_->compiled = true;

        if (fixedPoint) {
            SnapshotCompile(schedule, slot);
            // AFTER the write-back: a non-fixed-point compile bumps the generation as it writes, and
            // the slot must record the value it will be compared against next time.
            slot.generation = renderer->DeclarationsGeneration();
            slot.valid = true;
        }
        else {
            ++prepare_->cacheNotFixedPoint;
            slot.valid = false;
        }

        if (inputsMatch) { VerifyAgainstCachedOutput(); }

        // Async-compute step 7 acceptance: dump every compiled barrier ONCE, by resource NAME and
        // by (pass, point). Names not pointers, for the same reason the submit-order dump uses
        // them — pointers differ between runs and prove nothing about the compile.
        if (render::g_dumpBarriers) {
            render::g_dumpBarriers = false;
            DumpCompiledBarriers(schedule);
        }

        // Throttled: one line per compile drowns any run long enough to leave the churn behind and
        // reach steady state, which is the only regime where the cache's hit rate means anything.
        // (Measured the hard way: a 15-frame --scene-stress read 1 hit / 14 misses, all of it
        // level-reload and resize traffic.)
        if (render::g_barrierCompileLog &&
            ((prepare_->cacheHits + prepare_->cacheMisses) % 512u) == 0u) {
            // Step 12: the emit split says whether the ENHANCED path actually ran, as opposed to
            // merely compiling. `legacy` counts points that fell back — a translation gap must be
            // visible, never silent.
            std::uint32_t emitEnhanced = 0, emitLegacy = 0;
            barriers::EmitStats(emitEnhanced, emitLegacy);
            // Step 14: the acceleration-structure split, reported apart from the totals. It is the
            // only counter that says whether RT ran at all — a stress run on a machine with
            // reflections off would otherwise look identical to one where the AS barrier works.
            std::uint32_t asEnhanced = 0, asLegacy = 0;
            barriers::AsEmitStats(asEnhanced, asLegacy);
            char msg[416];
            std::snprintf(msg, sizeof(msg),
                          "[barrier-compile] %u barriers over %zu resources; return-to-canonical would add %u; "
                          "cache hits=%u misses=%u not-fixed-point=%u; emit enhanced=%u legacy=%u; "
                          "as enhanced=%u legacy=%u\n",
                          prepare_->barrierCount, compileState_.size(), returnBarrierEstimate_,
                          prepare_->cacheHits, prepare_->cacheMisses, prepare_->cacheNotFixedPoint,
                          emitEnhanced, emitLegacy, asEnhanced, asLegacy);
            Renderer::DiagLog(msg);
        }
    }

    // Step 7 acceptance: the compiled barrier arrays, written out for a before/after diff.
    void DumpCompiledBarriers(const tc::inl_vector<FlatNode, MaxPasses>& schedule) const
    {
        FILE* f = nullptr;
        if (fopen_s(&f, diag::LogPath("barrier_dump.log").c_str(), "a") != 0 || !f) { return; }
        std::fprintf(f, "== compiled barriers: %u ==\n", prepare_->barrierCount);
        auto dumpPass = [&](size_t passIdx) {
            for (std::uint32_t pt = 0; pt < kResourceUsesPerPassBudget; ++pt) {
                const auto& sl = prepare_->barrierPoints[passIdx][pt];
                for (std::uint32_t i = 0; i < sl.count; ++i) {
                    const D3D12_RESOURCE_BARRIER& b = prepare_->barrierArena[sl.begin + i];
                    char label[160];
                    render::DebugObjectLabel(b.Transition.pResource, label, sizeof(label));
                    std::fprintf(f, "%ls point=%u res=%s 0x%X->0x%X\n",
                                 RenderPassToWString(passes_[passIdx].name).data(), pt, label,
                                 static_cast<unsigned>(b.Transition.StateBefore),
                                 static_cast<unsigned>(b.Transition.StateAfter));
                }
            }
        };
        for (const FlatNode& n : schedule) {
            const size_t gid = passes_[n.pass].groupId;
            if (gid == kNoGroup) { dumpPass(n.pass); continue; }
            for (size_t m : groups_[gid]) { dumpPass(m); }
        }
        std::fclose(f);
    }

    // Is every input the compile reads identical to the snapshot the cached output was built from?
    //
    // Compared as raw bytes rather than hashed on purpose: the cost of being wrong here is barriers
    // with a before-state the GPU has already left, which neither the comparator nor GBV is
    // guaranteed to catch on the frame it happens. `arenaSize` is a few hundred 16-byte entries, so
    // this is one memcmp-shaped loop.
    //
    // NOT covered here, and covered by `cacheGeneration` instead: whether each resource is still
    // declared and still compile-managed. That can change with the use list untouched.
    bool CompileInputsUnchanged(const tc::inl_vector<FlatNode, MaxPasses>& schedule,
                                const typename PrepareState::CacheSlot& slot) const
    {
        if (slot.uses.size() != prepare_->arenaSize) { return false; }
        size_t at = 0;
        bool same = true;
        auto checkPass = [&](size_t passIdx) {
            if (!same) { return; }
            if (at >= slot.order.size()) { same = false; return; }
            const auto& c = slot.order[at++];
            const auto& slice = prepare_->slices[passIdx];
            // The pass NAME as well as its index: a graph that changes shape can reuse an index for
            // a different pass, and then the slice alone would not tell them apart.
            if (c.index != passIdx || c.name != passes_[passIdx].name ||
                c.queue != passes_[passIdx].queue ||
                c.slice.begin != slice.begin || c.slice.count != slice.count) {
                same = false;
            }
        };
        for (const FlatNode& n : schedule) {
            const size_t gid = passes_[n.pass].groupId;
            if (gid == kNoGroup) { checkPass(n.pass); continue; }
            for (size_t m : groups_[gid]) { checkPass(m); }
        }
        if (!same || at != slot.order.size()) { return false; }
        for (std::uint32_t i = 0; i < prepare_->arenaSize; ++i) {
            const ResourceUse& a = slot.uses[i];
            const ResourceUse& b = prepare_->arena[i];
            if (a.resource != b.resource || a.state != b.state || a.point != b.point) { return false; }
        }
        return true;
    }

    // Store this compile's inputs AND its output into the frame slot.
    void SnapshotCompile(const tc::inl_vector<FlatNode, MaxPasses>& schedule,
                         typename PrepareState::CacheSlot& slot)
    {
        slot.order.clear();
        auto addPass = [&](size_t passIdx) {
            slot.order.push_back(
                typename PrepareState::CompiledPass{ passIdx, passes_[passIdx].name,
                                                     prepare_->slices[passIdx], passes_[passIdx].queue });
        };
        for (const FlatNode& n : schedule) {
            const size_t gid = passes_[n.pass].groupId;
            if (gid == kNoGroup) { addPass(n.pass); continue; }
            for (size_t m : groups_[gid]) { addPass(m); }
        }
        slot.uses.assign(prepare_->arena, prepare_->arena + prepare_->arenaSize);
        slot.barriers.assign(prepare_->barrierArena, prepare_->barrierArena + prepare_->barrierCount);
        slot.points = prepare_->barrierPoints;
    }

    // Put a cached compile back where the recording path expects it. The point table is copied
    // wholesale rather than cleared-and-refilled: it is one flat POD array either way, and a stale
    // non-empty slice left behind by a longer previous compile would hand a pass barriers that are
    // not its own.
    void RestoreCachedOutput(const typename PrepareState::CacheSlot& slot)
    {
        prepare_->barrierCount = static_cast<std::uint32_t>(slot.barriers.size());
        if (prepare_->barrierCount > 0) {
            std::memcpy(prepare_->barrierArena, slot.barriers.data(),
                        slot.barriers.size() * sizeof(D3D12_RESOURCE_BARRIER));
        }
        prepare_->barrierPoints = slot.points;
        prepare_->compiled = true;
    }

    // --barrier-cache-verify: prove the cache would have served exactly what a fresh compile
    // produces. Run it over the full --scene-stress churn; silence is the only acceptable result.
    void VerifyAgainstCachedOutput() const
    {
        auto complain = [](const char* what) {
            char msg[200];
            std::snprintf(msg, sizeof(msg), "[barrier-cache] STALE: %s\n", what);
            Renderer::DiagLog(msg);
        };
        if (prepare_->verifyBarriers.size() != prepare_->barrierCount) { complain("barrier count"); return; }
        for (std::uint32_t i = 0; i < prepare_->barrierCount; ++i) {
            const D3D12_RESOURCE_BARRIER& a = prepare_->verifyBarriers[i];
            const D3D12_RESOURCE_BARRIER& b = prepare_->barrierArena[i];
            if (a.Type != b.Type || a.Transition.pResource != b.Transition.pResource ||
                a.Transition.StateBefore != b.Transition.StateBefore ||
                a.Transition.StateAfter != b.Transition.StateAfter ||
                a.Transition.Subresource != b.Transition.Subresource) {
                char msg[260];
                char label[96] = {};
                std::snprintf(msg, sizeof(msg),
                              "[barrier-cache] STALE barrier %u: cached %p 0x%X->0x%X, fresh %p 0x%X->0x%X\n",
                              i, static_cast<const void*>(a.Transition.pResource),
                              static_cast<unsigned>(a.Transition.StateBefore),
                              static_cast<unsigned>(a.Transition.StateAfter),
                              static_cast<const void*>(b.Transition.pResource),
                              static_cast<unsigned>(b.Transition.StateBefore),
                              static_cast<unsigned>(b.Transition.StateAfter));
                (void)label;
                Renderer::DiagLog(msg);
                return;
            }
        }
        size_t at = 0;
        for (size_t p = 0; p < MaxPasses; ++p) {
            for (const auto& sl : prepare_->barrierPoints[p]) {
                if (at >= prepare_->verifyPoints.size()) { complain("point table size"); return; }
                const auto& v = prepare_->verifyPoints[at++];
                if (v.begin != sl.begin || v.count != sl.count) { complain("point slice"); return; }
            }
        }
    }

    // Step 7: flatten one pass's per-point compiled barriers into a contiguous view.
    //
    // The compile writes each pass's barriers into ONE contiguous run of the arena (points are
    // filled in ascending order), so the entries are direct arena slices — no copying. `emitted`
    // rides alongside in the PrepareState so the flags reset with the compile, not per body.
    //
    // Pass-flow S1: the view keeps EMPTY points (count = 0, the compile ate every barrier of a
    // declared point) up to the LAST point the pass declared uses for — EmitPoint markers carry
    // absolute declaration indices, so the correspondence must be 1:1. The Transition matcher
    // steps over empty points transparently, which reproduces exactly what omitting them used
    // to do.
    void BuildPassBarrierView(size_t passIdx, Renderer::CompiledBarriers& out)
    {
        out.unmatched.store(0, std::memory_order_relaxed);
        out.markerUsed.store(false, std::memory_order_relaxed);
        const auto& slice = prepare_->slices[passIdx];
        std::uint32_t declaredPoints = 0;
        for (std::uint32_t i = 0; i < slice.count; ++i) {
            const ResourceUse& use = prepare_->arena[slice.begin + i];
            if (use.point < kResourceUsesPerPassBudget) {
                declaredPoints = std::max(declaredPoints, use.point + 1u);
            }
        }
        auto& views = prepare_->barrierPointViews[passIdx];
        for (std::uint32_t p = 0; p < declaredPoints; ++p) {
            const auto& sl = prepare_->barrierPoints[passIdx][p];
            views[p].entries = sl.count > 0 ? &prepare_->barrierArena[sl.begin] : nullptr;
            views[p].count = sl.count;
            views[p].emitted.store(false, std::memory_order_relaxed);
        }
        out.points = views.data();
        out.pointCount = declaredPoints;
        out.pass = static_cast<int>(passes_[passIdx].name); // --barrier-flip-trace label only
    }

    // Step 3: diff registered-vs-observed for every converted pass and log the differences.
    //
    // Compared as MULTISETS of (resource, state), not as sequences: a pass is free to order
    // its transitions differently from its registrations as long as the same work happens,
    // and within one command list the tracker resolves order itself. Sequence equality
    // becomes required only when the compiled arrays go authoritative (step 7), and that
    // check belongs with the compile, not here.
    //
    // Logs at most a few lines per frame per pass, and only on a mismatch, so a converted
    // engine with the flag on is quiet.
    void ReportComparator()
    {
        if (!prepare_ || !render::g_barrierComparator) { return; }
        // The `i < MaxPasses` half is redundant at runtime (passesNum_ never exceeds it) but
        // /analyze cannot prove that and fires C28020 on the array index for a one-pass graph
        // (the epilogue). Stated in the loop condition, which it does follow.
        for (size_t i = 0; i < passesNum_ && i < MaxPasses; ++i) {
            if (!prepare_->fns[i]) { continue; }
            auto& obs = prepare_->observed[i];
            if (!obs.ran) { continue; } // pass early-outed before its body ran
            const auto& slice = prepare_->slices[i];

            if (obs.log.overflowed.load(std::memory_order_relaxed)) {
                LogComparator(passes_[i].name, "observation buffer overflowed - raise kResourceUsesPerPassBudget",
                              nullptr, D3D12_RESOURCE_STATE_COMMON);
                continue;
            }

            // Pass-flow S1: a body (or a sub-block of one) that used EmitPoint markers emits
            // straight from the compile without feeding the observation log, and cannot diverge
            // from its declarations by construction. Its declarations would read as "INFO extra"
            // and a purely-marker body as SKIPPED — both meaningless for it — so those two
            // directions are muted. The FATAL "MISSING" direction below stays fully armed: any
            // remaining NAMED Transition call in the same pass is still checked.
            const bool markerUsed =
                prepare_->passBarriers[i].markerUsed.load(std::memory_order_relaxed);

            // The body performed NOTHING — it early-outed. One line instead of one per
            // registration: the useful fact is "this pass did not run", and a pass that skips
            // every frame would otherwise bury the log (VsmPageRender emitted 31k lines this
            // way). It also says plainly that the pass's registration is UNVERIFIED, which a
            // pile of benign "extra" lines does not.
            const std::uint32_t observedCount = obs.count.load(std::memory_order_relaxed);
            if (observedCount == 0 && slice.count > 0 && !markerUsed) {
                LogComparator(passes_[i].name, "SKIPPED (body performed nothing - registration unverified)",
                              nullptr, D3D12_RESOURCE_STATE_COMMON);
                obs.ran = false;
                continue;
            }

            // Registered but never done.
            for (std::uint32_t u = 0; u < slice.count && !markerUsed; ++u) {
                const ResourceUse& want = prepare_->arena[slice.begin + u];
                bool found = false;
                for (std::uint32_t o = 0; o < observedCount && !found; ++o) {
                    found = (obs.entries[o].resource == want.resource && obs.entries[o].state == want.state);
                }
                // BENIGN. Almost always means the body early-outed before applying its
                // declared states. Once the compiled arrays are authoritative the barrier is
                // emitted anyway, so the running state stays consistent — it just costs one
                // extra transition on frames the pass does nothing. Reported, not fatal.
                if (!found) { LogComparator(passes_[i].name, "INFO extra (registered, pass did not perform)", want.resource, want.state); }
            }
            // FATAL direction: the body needs this transition and nothing registered it, so
            // after step 7 it would simply not be emitted. Silence HERE is the bar for
            // calling a pass converted.
            for (std::uint32_t o = 0; o < observedCount; ++o) {
                const Renderer::ObservedTransition& did = obs.entries[o];
                bool found = false;
                for (std::uint32_t u = 0; u < slice.count && !found; ++u) {
                    const ResourceUse& want = prepare_->arena[slice.begin + u];
                    found = (want.resource == did.resource && want.state == did.state);
                }
                if (!found) { LogComparator(passes_[i].name, "MISSING (performed, never registered)", did.resource, did.state); }
            }
            obs.ran = false;
        }
    }

    // Resource DEBUG NAME, not just the pointer: converting passes means reading these lines
    // and mapping them back to a `Use` call, and a bare address makes that guesswork. Most
    // engine resources get SetName() at creation; unnamed ones fall back to the address.
    static void ResourceLabel(ID3D12Resource* res, char* out, size_t outSize)
    {
        render::DebugObjectLabel(res, out, outSize);
    }

    static void LogComparator(RenderPass pass, const char* what, ID3D12Resource* res, D3D12_RESOURCE_STATES state)
    {
        char label[160];
        ResourceLabel(res, label, sizeof(label));
        char msg[320];
        std::snprintf(msg, sizeof(msg), "[barrier-cmp] pass=%d %s res=%s state=0x%X\n",
                      static_cast<int>(pass), what, label, static_cast<unsigned>(state));
        // ONCE per distinct line. The comparator runs every frame, so the two long-standing benign
        // divergences (Exposure.Value, Ocean.Wetness*) otherwise write one line each per frame and
        // bury anything new in the middle of them. The SKIPPED direction above already had to
        // solve the same problem by hand (VsmPageRender, 31k lines).
        Renderer::DiagLogOnce(msg);
    }

    // Step 3: run one pass body with its transitions observed, when it has a Prepare and
    // the comparator is on. A pass without a Prepare is run untouched (no observation
    // cost, and nothing to compare it against).
    void RunPassBody(Renderer* renderer, size_t passIdx, PassContext& ctx)
    {
        // Step 7: install this pass's compiled barriers for the duration of its body — this is
        // what its Transition calls emit. Independent of the comparator: barriers must work with
        // every diagnostic off.
        const bool flip = prepare_ && prepare_->compiled && prepare_->fns[passIdx];
        if (flip) {
            Renderer::CompiledBarriers& cbs = prepare_->passBarriers[passIdx];
            BuildPassBarrierView(passIdx, cbs);
            Renderer::SetThreadCompiledBarriers(&cbs);
        }
        struct Restore {
            bool on;
            ~Restore() { if (on) { Renderer::SetThreadCompiledBarriers(nullptr); } }
        } restore{ flip };

        if (!prepare_ || !render::g_barrierComparator || !prepare_->fns[passIdx]) {
            passes_[passIdx].exec(ctx);
            return;
        }
        auto& obs = prepare_->observed[passIdx];
        obs.count.store(0, std::memory_order_relaxed);
        obs.ran = true;
        obs.log.entries = obs.entries;
        obs.log.count = &obs.count;
        obs.log.capacity = static_cast<std::uint32_t>(std::size(obs.entries));
        obs.log.overflowed.store(false, std::memory_order_relaxed);
        Renderer::SetThreadTransitionLog(&obs.log);
        passes_[passIdx].exec(ctx);
        Renderer::SetThreadTransitionLog(nullptr);
        // Fan-out appends may still be in flight here; ExecuteParallel joins them before
        // ReportComparator reads the buffer.
    }

    // Run a node into one batch: a singleton pass, or every member of a group in
    // declaration order sharing one command list (closed once after the last).
    void RunNode(Renderer* renderer, size_t ownerIdx, size_t batch)
    {
        const size_t gid = passes_[ownerIdx].groupId;
        if (gid == kNoGroup) {
            if (!passes_[ownerIdx].exec) { return; }
            PassContext ctx;
            ctx.renderer = renderer;
            ctx.batchIndex = batch;
            ctx.pass = passes_[ownerIdx].name;
            ctx.declares = &passes_[ownerIdx].declares;
            ctx.groupCL = nullptr;
            ctx.queue = passes_[ownerIdx].queue; // step 4
            RunPassBody(renderer, ownerIdx, ctx);
            return;
        }

        PassContext::GroupCL groupCL;
        for (size_t m : groups_[gid]) {
            if (!passes_[m].exec) { continue; }
            PassContext ctx;
            ctx.renderer = renderer;
            ctx.batchIndex = batch;
            ctx.pass = passes_[m].name;
            ctx.declares = &passes_[m].declares;
            ctx.groupCL = &groupCL;
            ctx.queue = passes_[m].queue; // step 4 (a group's members are all Graphics — see BeginCL)
            RunPassBody(renderer, m, ctx);
        }
        if (groupCL.opened) {
            // The group's single shared list is the lone direct in its batch.
            renderer->EndThreadCommandList(groupCL.shared, batch, 0);
        }
    }

    // A node's runtime dependencies: the union of its members' mtDeps, each
    // remapped to the owner of the depended-on pass (a dep on any group member
    // becomes a dep on that group's task), de-duplicated, excluding self.
    void CollectNodeDeps(size_t ownerIdx, DependencyList& out) const
    {
        out.clear_fast();
        auto addFrom = [&](size_t passIdx) {
            for (size_t d : passes_[passIdx].mtDeps) {
                if (d >= passesNum_) { continue; }
                const size_t od = OwnerOf(d);
                if (od == ownerIdx || !passes_[od].exec) { continue; }
                bool dup = false;
                for (size_t e : out) { if (e == od) { dup = true; break; } }
                if (dup) { continue; }
                assert(out.size() < out.capacity() && "node dependency capacity exceeded");
                if (out.size() < out.capacity()) { out.push_back(od); }
            }
        };
        const size_t gid = passes_[ownerIdx].groupId;
        if (gid == kNoGroup) { addFrom(ownerIdx); }
        else { for (size_t m : groups_[gid]) { addFrom(m); } }
    }

    // General unrolling: build the topological order, create batches, or execute inplace
    void Unroll(Renderer* renderer, bool executeInplace, tc::inl_vector<FlatNode, MaxPasses>* outFlat)
    {
        const size_t N = passesNum_;
        if (N == 0u) { return; }

        tc::inl_vector<size_t, MaxPasses> indeg;
        indeg.resize(N, 0);
        for (size_t i = 0; i < N; ++i) {
            size_t degree = 0;
            for (size_t prereq : passes_[i].prereqs) {
                if (prereq < N) {
                    ++degree;
                }
            }
            indeg[i] = degree;
        }

        tc::inl_vector<size_t, MaxPasses> q;
        for (size_t i = 0; i < N; ++i) {
            if (indeg[i] == 0u) { q.push_back(i); }
        }

        // Step 6: pass -> its batch, filled as the topological walk assigns them. Needed to turn a
        // graph dependency into a cross-queue fence edge (see below).
        tc::inl_vector<size_t, MaxPasses> batchOfPass;
        batchOfPass.resize(N, (size_t)-1);

        if (!executeInplace && outFlat != nullptr) {
            outFlat->clear();
        }

        size_t produced = 0;
        size_t qIndex = 0;
        while (qIndex < q.size()) {
            const size_t u = q[qIndex++];

            // A node (group or singleton) materializes once, when its OWNER is
            // reached in topological order. Non-owner group members are skipped
            // here — they were run as part of their owner's node — but their
            // successor edges are still relaxed below so the topo order is correct.
            if (OwnerOf(u) == u && passes_[u].exec) {
                const size_t batch =
                    (submitBatchIndex_ == (size_t)-1)
                    ? renderer->BeginSubmitBatch(passes_[u].queue)
                    : submitBatchIndex_;
                batchOfPass[u] = batch;
                // Step 6 (D2): a cross-queue fence edge comes from a real graph DEPENDENCY, not
                // from batch order. Walk this node's prereqs and mtDeps; any that ran on the other
                // queue contributes its batch, and the LATEST one wins (waiting for the furthest
                // producer subsumes the earlier ones). Predecessors are already assigned because
                // this walk is topological.
                if (submitBatchIndex_ == (size_t)-1) {
                    size_t wait = (size_t)-1;
                    auto consider = [&](size_t dep) {
                        if (dep >= N) { return; }
                        const size_t owner = OwnerOf(dep);
                        if (passes_[owner].queue == passes_[u].queue) { return; }
                        const size_t db = batchOfPass[owner];
                        if (db == (size_t)-1) { return; }
                        if (wait == (size_t)-1 || db > wait) { wait = db; }
                    };
                    const size_t gid = passes_[u].groupId;
                    if (gid == kNoGroup) {
                        for (size_t d : passes_[u].prereqs) { consider(d); }
                        for (size_t d : passes_[u].mtDeps) { consider(d); }
                    } else {
                        for (size_t m : groups_[gid]) {
                            for (size_t d : passes_[m].prereqs) { consider(d); }
                            for (size_t d : passes_[m].mtDeps) { consider(d); }
                        }
                    }
                    if (wait != (size_t)-1) { renderer->SetSubmitBatchCrossQueueWait(batch, wait); }
                }

                if (executeInplace) {
                    RunNode(renderer, u, batch);
                }
                else if (outFlat != nullptr) {
                    outFlat->push_back(FlatNode{ u, batch });
                }
            }

            ++produced;
            for (size_t v : passes_[u].successors) {
                if (v < N) {
                    if (indeg[v] > 0u) { --indeg[v]; }
                    if (indeg[v] == 0u) { q.push_back(v); }
                }
            }
        }
        if (produced != N) { assert(false && "RenderGraph has a cycle!"); }
    }

private:
    // Pass-flow S9: PRIVATE. Attaching a Prepare to a pass added separately was the old authoring
    // shape — the one this plan removed, because a Prepare and a Record kept in step by hand is
    // exactly what drifts. `AddPass2Internal` is its only caller now, so the two-phase machinery
    // survives as an implementation detail and no new pass can be written as a mirror.
    void SetPassPrepare(size_t passIndex, PrepareFn fn)
    {
        assert(passIndex < passesNum_ && "SetPassPrepare on an unknown pass");
        if (passIndex >= passesNum_) { return; }
        if (!prepare_) { prepare_ = std::make_unique<PrepareState>(); }
        prepare_->fns[passIndex] = std::move(fn);
    }

    // Pass-flow S2: create the pass with a placeholder body (RunPrepareOne skips an empty exec),
    // then install a Prepare that runs the builder and replaces the body with its return.
    template <typename RangePrereqs, typename RangeDeps>
    size_t AddPass2Internal(RenderPass name,
        const RangePrereqs& prereqs,
        const RangeDeps& deps,
        std::initializer_list<ResourceStateDecl> declares,
        BuildFn builder,
        RenderQueue queue)
    {
        CPU_SCOPE(ProfilerScopes::kAddPass);
        const size_t idx = AddPassInternal(name, prereqs, deps, declares,
            [](PassContext) {});
        passes_[idx].builtByBuilder = true;
        // Step 4 (D1): recorded at graph-build time and never touched again. The barrier compile
        // runs after this and before any body, so by the time a body could ask, the answer is
        // already fixed — which is precisely why the queue has to be decided here.
        passes_[idx].queue = queue;
        // Step 5 (D5/R8): a CL GROUP shares ONE command list, so its members share one queue. A
        // grouped pass marked AsyncCompute would silently record onto the group's DIRECT list —
        // `BeginCL` has no way to honour it — and the result is compute work on the graphics queue
        // that the trace would happily show as "overlapping" nothing. Fail at registration.
        if (queue == RenderQueue::AsyncCompute && currentGroup_ != kNoGroup) {
            char msg[288];
            std::snprintf(msg, sizeof(msg),
                          "RenderGraph::AddPass2: pass '%ls' is marked AsyncCompute inside a CL "
                          "group. A group shares one command list, so its members share one queue; "
                          "take the pass out of the group first (design D5).",
                          RenderPassToWString(name).data());
            RendererInvariantFailure(msg);
        }
        SetPassPrepare(idx, [this, idx, builder = std::move(builder)](PassContext& ctx)
        {
            ExecFn exec = builder(ctx);
            passes_[idx].exec = exec ? std::move(exec)
                                     : ExecFn([](PassContext) {});
        });
        return idx;
    }

    template <typename RangePrereqs, typename RangeDeps>
    size_t AddPassInternal(RenderPass name,
        const RangePrereqs& prereqs,
        const RangeDeps& deps,
        std::initializer_list<ResourceStateDecl> declares,
        ExecFn fn)
    {
        assert(passesNum_ < MaxPasses && "RenderGraph capacity exceeded");
        const size_t newIndex = passesNum_;
        ++passesNum_;

        Pass& p = passes_[newIndex];
        p.name = name;
        const bool prereqCopyOk = CopyRange(p.prereqs, prereqs);
        const bool depCopyOk = CopyRange(p.mtDeps, deps);
        assert(prereqCopyOk && depCopyOk && "RenderGraph dependency list capacity exceeded");
        (void)prereqCopyOk;
        (void)depCopyOk;

        assert(declares.size() <= p.declares.capacity() && "RenderGraph declaration list capacity exceeded");
        p.declares.clear_fast();
        for (const ResourceStateDecl& decl : declares) {
            if (p.declares.size() >= p.declares.capacity()) { break; }
            p.declares.push_back(decl);
        }

        p.exec = std::move(fn);

        // CL group membership: passes added between BeginCLGroup/EndCLGroup join
        // the active group (declaration order = member order).
        p.groupId = currentGroup_;
        if (currentGroup_ != kNoGroup) {
            assert(groups_[currentGroup_].size() < groups_[currentGroup_].capacity() && "CL group member capacity exceeded");
            groups_[currentGroup_].push_back(newIndex);
        }

        for (size_t prereq : p.prereqs) {
            if (prereq < passesNum_) {
                passes_[prereq].successors.push_back(newIndex);
            } else if (prereq < MaxPasses) {
                pendingSuccessors_[prereq].push_back(newIndex);
            }
        }

        if (newIndex < pendingSuccessors_.size())
        {
            for (size_t dependent : pendingSuccessors_[newIndex]) {
                p.successors.push_back(dependent);
            }
            pendingSuccessors_[newIndex].clear_fast();
        }

        return newIndex;
    }

    template <typename Range>
    static bool CopyRange(DependencyList& dst, const Range& range)
    {
        dst.clear_fast();
        for (size_t value : range) {
            if (value >= MaxPasses) {
                assert(false && "RenderGraph dependency index exceeds graph capacity");
                continue;
            }

            if (dst.size() >= dst.capacity()) {
                return false;
            }

            dst.push_back(value);
        }
        return true;
    }

    //tc::inl_vector<Pass, MaxPasses> passes_;
    Pass passes_[MaxPasses];
    size_t passesNum_ = 0;
    size_t submitBatchIndex_ = (size_t)-1;
    std::array<SuccessorList, MaxPasses> pendingSuccessors_{};

    // CL groups (step 5): groups_[g] is the ordered member list of group g.
    std::array<GroupMemberList, MaxPasses> groups_{};
    size_t groupCount_ = 0;
    size_t currentGroup_ = kNoGroup; // active group during construction

    // A.1s: null unless some pass supplied a Prepare (see PrepareState for why it is
    // heap-side). One pointer of stack cost when unused, which is every graph today.
    std::unique_ptr<PrepareState> prepare_;
    // Step 7: the compile's running per-resource state. A member so the per-frame compile
    // reuses its buckets; cleared at the top of every CompileBarriers.
    // Step 7: the compile's running value is (state, owning queue). One value per resource still —
    // a resource HAS one state — but the owner is what says whether the next consumer's queue may
    // legally take it, so the two travel together everywhere.
    struct TrackedState {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        RenderQueue           owner = RenderQueue::Graphics;
    };
    robin_hood::unordered_map<ID3D12Resource*, TrackedState> compileState_;
    // Scratch for the per-pass return-to-canonical estimate (see CompileBarriers).
    robin_hood::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> passTouched_;
    std::uint32_t returnBarrierEstimate_ = 0;
};
