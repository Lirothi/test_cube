#pragma once
#include <functional>
#include <string>
#include <vector>
#include <queue>
#include <cassert>
#include "Renderer.h"
#include "TaskSystem.h"
#include "Profiler.h"

class RenderGraph {
public:
    struct PassContext {
        Renderer* renderer = nullptr;
        size_t      batchIndex = (size_t)-1;
        std::string passName;
    };

    using ExecFn = std::function<void(PassContext)>;

    explicit RenderGraph(size_t submitBatchIndex = (size_t)-1)
        : submitBatchIndex_(submitBatchIndex) {
    }

    struct Pass {
        std::string           name;
        std::vector<size_t>   prereqs;     // порядок открытия батчей (DAG)
        ExecFn                exec;        // тело пасса
        std::vector<size_t>   mtDeps;      // РАНТАЙМ-зависимости (какие пассы должны завершиться)
    };

    // Удобный AddPass: все prereqs являются и mt-deps (флаг), либо явно mtDeps
    size_t AddPass(const std::string& name,
        const std::vector<size_t>& prereqs,
        ExecFn fn)
    {
        Pass p{ name, prereqs, std::move(fn), {} };
        passes_.push_back(std::move(p));
        return passes_.size() - 1;
    }

    // Перегрузка с явными mt-deps (более точная)
    size_t AddPassMT(const std::string& name,
        const std::vector<size_t>& prereqs,
        const std::vector<size_t>& mtDeps,
        ExecFn fn)
    {
        Pass p{ name, prereqs, std::move(fn), mtDeps };
        passes_.push_back(std::move(p));
        return passes_.size() - 1;
    }

    struct FlatNode { size_t pass; size_t batch; };

    // Старое: последовательное исполнение (на место)
    void Execute(Renderer* renderer)
    {
        CPU_SCOPE(L"RenderGraph::Execute");
        if (renderer == nullptr) { return; }
        Unroll(renderer, /*executeInplace=*/true, nullptr);
    }

    // План без исполнения (для параллельного раннинга)
    const std::vector<FlatNode>& BuildSchedule(Renderer* renderer)
    {
        scheduleScratch_.clear();
        if (renderer == nullptr) { return scheduleScratch_; }
        Unroll(renderer, /*executeInplace=*/false, &scheduleScratch_);
        return scheduleScratch_;
    }

    // Параллель: создаём батчи в топологическом порядке, затем
    // сабмитим РЕАЛЬНЫЕ таски пассов с ожиданием их mt-deps.
    void ExecuteParallel(Renderer* renderer, TaskSystem& tasks)
    {
        CPU_SCOPE(L"RenderGraph::ExecuteParallel");
#if !TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
        (void)tasks;
        Execute(renderer);
        return;
#endif
        const auto& flat = BuildSchedule(renderer);
        if (flat.empty()) { return; }

        const size_t N = passes_.size();

        passDoneScratch_.assign(N, nullptr);

        // create all tasks first
        for (const auto& n : flat) {
            const size_t passIdx = n.pass;
            if (!passes_[passIdx].exec) { continue; }

            auto handle = tasks.CreateTask([this, renderer, n, passIdx]() {
                PassContext ctx;
                ctx.renderer = renderer;
                ctx.batchIndex = n.batch;
                ctx.passName = passes_[passIdx].name;
                passes_[passIdx].exec(ctx);
            }, passes_[passIdx].mtDeps.size());

            passDoneScratch_[passIdx] = handle;
        }

        // set up dependencies between created tasks
        for (const auto& n : flat) {
            const size_t passIdx = n.pass;
            if (!passes_[passIdx].exec) { continue; }

            dependencyScratch_.clear();
            for (size_t dep : passes_[passIdx].mtDeps) {
                if (dep < passDoneScratch_.size() && passDoneScratch_[dep]) {
                    dependencyScratch_.push_back(passDoneScratch_[dep]);
                }
            }

            tasks.SetDependencies(passDoneScratch_[passIdx], dependencyScratch_);
        }

        // finally submit tasks for execution
        for (const auto& n : flat) {
            const size_t passIdx = n.pass;
            auto& pass = passes_[passIdx];
            if (!pass.exec || pass.mtDeps.size() > 0) { continue; }
            tasks.Submit(passDoneScratch_[passIdx]);
        }

        for (size_t i = 0; i < N; ++i) {
            if (passDoneScratch_[i]) {
                tasks.Wait(passDoneScratch_[i]);
            }
        }
        for (size_t i = N; i-- > 0;) {
            tasks.Release(passDoneScratch_[i]);
        }
    }

    void Clear() { passes_.clear(); }

private:
    // Общая раскрутка: строим топопорядок, создаём batch’и, либо исполняем inplace
    void Unroll(Renderer* renderer, bool executeInplace, std::vector<FlatNode>* outFlat)
    {
        const size_t N = passes_.size();
        if (N == 0u) { return; }

        std::vector<size_t> indeg(N, 0);
        std::vector<std::vector<size_t>> adj(N);
        for (size_t i = 0; i < N; ++i) {
            for (size_t d : passes_[i].prereqs) {
                if (d < N) {
                    ++indeg[i];
                    adj[d].push_back(i);
                }
            }
        }

        std::queue<size_t> q;
        for (size_t i = 0; i < N; ++i) {
            if (indeg[i] == 0u) { q.push(i); }
        }

        if (!executeInplace && outFlat != nullptr) {
            outFlat->clear();
            outFlat->reserve(N);
        }

        size_t produced = 0;
        while (!q.empty()) {
            const size_t u = q.front(); q.pop();
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
            for (size_t v : adj[u]) {
                if (v < N) {
                    if (indeg[v] > 0u) { --indeg[v]; }
                    if (indeg[v] == 0u) { q.push(v); }
                }
            }
        }
        if (produced != N) { assert(false && "RenderGraph has a cycle!"); }
    }

private:
    std::vector<Pass>                  passes_;
    size_t                             submitBatchIndex_ = (size_t)-1;
    std::vector<FlatNode>              scheduleScratch_;
    std::vector<TaskSystem::TaskHandle> passDoneScratch_;
    std::vector<TaskSystem::TaskHandle> dependencyScratch_;
};
