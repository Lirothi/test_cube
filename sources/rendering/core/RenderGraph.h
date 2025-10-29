#pragma once
#include <array>
#include <functional>
#include <string>
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

struct RenderGraphPassContext {
    Renderer* renderer = nullptr;
    size_t      batchIndex = (size_t)-1;
    std::string_view passName;
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

    struct Pass {
        std::string name;
        DependencyList prereqs; // batch opening order (DAG)
        ExecFn exec;     // pass body
        DependencyList mtDeps;  // runtime dependencies (which passes must complete)
        SuccessorList successors;
    };

    // Convenience AddPass: treat all prereqs as mt-deps (flag) or specify mtDeps explicitly
    size_t AddPass(const std::string& name,
        std::initializer_list<size_t> prereqs,
        ExecFn fn)
    {
        CPU_SCOPE(ProfilerScopes::kAddPass);
        return AddPassInternal(name, prereqs, std::initializer_list<size_t>{}, std::move(fn));
    }

    size_t AddPass(const std::string& name,
        const DependencyList& prereqs,
        ExecFn fn)
    {
        CPU_SCOPE(ProfilerScopes::kAddPass);
        return AddPassInternal(name, prereqs, DependencyList{}, std::move(fn));
    }

    // Overload with explicit mt-deps (more precise)
    size_t AddPassMT(const std::string& name,
        std::initializer_list<size_t> prereqs,
        std::initializer_list<size_t> mtDeps,
        ExecFn fn)
    {
        return AddPassInternal(name, prereqs, mtDeps, std::move(fn));
    }

    size_t AddPassMT(const std::string& name,
        const DependencyList& prereqs,
        const DependencyList& mtDeps,
        ExecFn fn)
    {
        return AddPassInternal(name, prereqs, mtDeps, std::move(fn));
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

        tc::inl_vector<TaskSystem::TaskHandle, MaxPasses> passDone;

        passDone.resize(N, nullptr);

        // create all tasks first
        for (const auto& n : schedule) {
            const size_t passIdx = n.pass;
            if (!passes_[passIdx].exec) { continue; }

            auto handle = tasks.CreateTask([this, renderer, n, passIdx]() {
                PassContext ctx;
                ctx.renderer = renderer;
                ctx.batchIndex = n.batch;
                ctx.passName = passes_[passIdx].name;
                passes_[passIdx].exec(ctx);
            }, passes_[passIdx].mtDeps.size());

            passDone[passIdx] = handle;
        }

        tc::inl_vector<TaskSystem::TaskHandle, kPassDependencyCapacity> deps;
        // set up dependencies between created tasks
        for (const auto& n : schedule) {
            const size_t passIdx = n.pass;
            if (!passes_[passIdx].exec) { continue; }

            deps.clear();
            for (size_t dep : passes_[passIdx].mtDeps) {
                if (dep < passDone.size() && passDone[dep]) {
                    deps.push_back(passDone[dep]);
                }
            }

            tasks.SetDependencies(passDone[passIdx], deps.data(), deps.size());
        }

        // finally submit tasks for execution
        for (const auto& n : schedule) {
            const size_t passIdx = n.pass;
            auto& pass = passes_[passIdx];
            if (!pass.exec || pass.mtDeps.size() > 0) { continue; }
            tasks.Submit(passDone[passIdx]);
        }

        for (size_t i = 0; i < N; ++i) {
            if (passDone[i]) {
                tasks.Wait(passDone[i]);
            }
        }
        for (size_t i = N; i-- > 0;) {
            tasks.Release(passDone[i]);
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
            const auto& p = passes_[u];

            if (p.exec) {
                const size_t batch =
                    (submitBatchIndex_ == (size_t)-1)
                    ? renderer->BeginSubmitBatch(p.name)
                    : submitBatchIndex_;

                if (executeInplace) {
                    PassContext ctx;
                    ctx.renderer = renderer;
                    ctx.batchIndex = batch;
                    ctx.passName = p.name;
                    p.exec(ctx);
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
    size_t AddPassInternal(const std::string& name,
        const RangePrereqs& prereqs,
        const RangeDeps& deps,
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
        p.exec = std::move(fn);
        //passes_.push_back(std::move(p));

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
        else
        {
            assert(newIndex < pendingSuccessors_.size() && "RenderGraph capacity exceeded");
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
};
