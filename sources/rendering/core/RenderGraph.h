#pragma once
#include <array>
#include <functional>
#include <string_view>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <utility>
#include "rendering/core/Renderer.h"
#include "core/task/TaskSystem.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/containers/inl_vector.h"
#include "rendering/core/RenderPass.h"

// A resource state a pass declares it needs before it runs. Declarations are
// registered as first-use states on the pass's main command list; the actual
// transition barriers are injected between command lists at submit time by the
// ResourceStateTracker machinery, exactly like manual Renderer::Transition calls.
struct ResourceStateDecl {
    ID3D12Resource* resource = nullptr;   // null entries are skipped
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
using ResourceStateDeclList = tc::inl_vector<ResourceStateDecl, 8>;

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
};

template <std::size_t MaxPasses>
class RenderGraph {
public:
    using PassContext = RenderGraphPassContext;

    using ExecFn = std::function<void(PassContext)>;

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
        Unroll(renderer, /*executeInplace=*/true, nullptr);
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
    }

    void Clear()
    {
        //passes_.clear();
        for (auto& pending : pendingSuccessors_) {
            pending.clear_fast();
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
            passes_[ownerIdx].exec(ctx);
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
            passes_[m].exec(ctx);
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
};
