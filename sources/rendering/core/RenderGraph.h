#pragma once
#include <atomic>
#include <array>
#include <functional>
#include <string_view>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include "rendering/core/Renderer.h"
#include "rendering/core/RendererInvariantFailure.h"
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
using ResourceStateDeclList = tc::inl_vector<ResourceStateDecl, 10>;

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
inline bool g_barrierComparator = false;
// Step 7: one line per frame with the compiled barrier count. DEFAULT OFF.
inline bool g_barrierCompileLog = false;
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

    // Provision the pass's command list. Ungrouped: a fresh DIRECT list, closed
    // into the batch by EndCL. Grouped: the group's shared list (opened on first
    // call), and EndCL is a no-op — the group closes it after its last member.
    // Pass bodies are agnostic: same shape either way, only the provider differs.
    Renderer::ThreadCL BeginCL(ID3D12PipelineState* initialPSO = nullptr) const
    {
        if (groupCL) {
            if (!groupCL->opened) {
                groupCL->shared = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT, initialPSO);
                groupCL->opened = true;
            }
            return groupCL->shared;
        }
        return renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT, initialPSO);
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
        useArena[*useCount] = ResourceUse{ resource, state, usePoint ? *usePoint : 0u };
        ++(*useCount);
    }

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
        struct CompiledPass { size_t index; RenderPass name; Slice slice; };
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

    // A.1s: attach a Prepare callback to a pass added above. A separate setter rather
    // than more AddPass overloads — there are already six, and every combination of
    // (prereqs form, mtDeps, declares) would need one.
    //
    //   const size_t p = rg.AddPass(RenderPass::X, { prev }, [..](PassContext ctx) { ... });
    //   rg.SetPassPrepare(p, [..](PassContext& ctx) { ctx.Use(res, state); ... });
    //
    void SetPassPrepare(size_t passIndex, PrepareFn fn)
    {
        assert(passIndex < passesNum_ && "SetPassPrepare on an unknown pass");
        if (passIndex >= passesNum_) { return; }
        if (!prepare_) { prepare_ = std::make_unique<PrepareState>(); }
        prepare_->fns[passIndex] = std::move(fn);
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
                    (it == compileState_.end()) ? renderer->GetPredictedState(use.resource) : it->second;
                if (before == use.state) { continue; } // already there — no barrier
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
                compileState_[use.resource] = use.state;
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
        bool fixedPoint = true;
        for (const auto& kv : compileState_) {
            if (kv.second != renderer->GetPredictedState(kv.first)) { fixedPoint = false; }
            renderer->SetPredictedState(kv.first, kv.second);
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

        // Throttled: one line per compile drowns any run long enough to leave the churn behind and
        // reach steady state, which is the only regime where the cache's hit rate means anything.
        // (Measured the hard way: a 15-frame --scene-stress read 1 hit / 14 misses, all of it
        // level-reload and resize traffic.)
        if (render::g_barrierCompileLog &&
            ((prepare_->cacheHits + prepare_->cacheMisses) % 512u) == 0u) {
            char msg[280];
            std::snprintf(msg, sizeof(msg),
                          "[barrier-compile] %u barriers over %zu resources; return-to-canonical would add %u; "
                          "cache hits=%u misses=%u not-fixed-point=%u\n",
                          prepare_->barrierCount, compileState_.size(), returnBarrierEstimate_,
                          prepare_->cacheHits, prepare_->cacheMisses, prepare_->cacheNotFixedPoint);
            Renderer::DiagLog(msg);
        }
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
                typename PrepareState::CompiledPass{ passIdx, passes_[passIdx].name, prepare_->slices[passIdx] });
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
    // filled in ascending order), so the view is just the span from the first non-empty point to
    // the last — no copying. `claimed` rides alongside in the PrepareState so the flags reset with
    // the compile, not per body.
    void BuildPassBarrierView(size_t passIdx, Renderer::CompiledBarriers& out)
    {
        out.unmatched.store(0, std::memory_order_relaxed);
        auto& views = prepare_->barrierPointViews[passIdx];
        std::uint32_t n = 0;
        for (std::uint32_t p = 0; p < kResourceUsesPerPassBudget; ++p) {
            const auto& slice = prepare_->barrierPoints[passIdx][p];
            if (slice.count == 0) { continue; }
            views[n].entries = &prepare_->barrierArena[slice.begin];
            views[n].count = slice.count;
            views[n].emitted.store(false, std::memory_order_relaxed);
            ++n;
        }
        out.points = views.data();
        out.pointCount = n;
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

            // The body performed NOTHING — it early-outed. One line instead of one per
            // registration: the useful fact is "this pass did not run", and a pass that skips
            // every frame would otherwise bury the log (VsmPageRender emitted 31k lines this
            // way). It also says plainly that the pass's registration is UNVERIFIED, which a
            // pile of benign "extra" lines does not.
            const std::uint32_t observedCount = obs.count.load(std::memory_order_relaxed);
            if (observedCount == 0 && slice.count > 0) {
                LogComparator(passes_[i].name, "SKIPPED (body performed nothing - registration unverified)",
                              nullptr, D3D12_RESOURCE_STATE_COMMON);
                obs.ran = false;
                continue;
            }

            // Registered but never done.
            for (std::uint32_t u = 0; u < slice.count; ++u) {
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

    static void LogComparator(RenderPass pass, const char* what, ID3D12Resource* res, D3D12_RESOURCE_STATES state)
    {
        char label[160];
        ResourceLabel(res, label, sizeof(label));
        char msg[320];
        std::snprintf(msg, sizeof(msg), "[barrier-cmp] pass=%d %s res=%s state=0x%X\n",
                      static_cast<int>(pass), what, label, static_cast<unsigned>(state));
        Renderer::DiagLog(msg);
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
                    ? renderer->BeginSubmitBatch()
                    : submitBatchIndex_;

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
    robin_hood::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> compileState_;
    // Scratch for the per-pass return-to-canonical estimate (see CompileBarriers).
    robin_hood::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> passTouched_;
    std::uint32_t returnBarrierEstimate_ = 0;
};
