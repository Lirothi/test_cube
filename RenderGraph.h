#pragma once
#include <functional>
#include <string>
#include <vector>
#include <queue>
#include <cassert>
#include "Renderer.h"
#include "TaskSystem.h"

class RenderGraph {
public:
    struct PassContext {
        Renderer* renderer = nullptr;
        size_t      batchIndex = (size_t)-1;
        std::string passName;
        TaskGroup* group = nullptr;   // группа для внутренних тасок пасса (опц.)
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
        if (renderer == nullptr) { return; }
        Unroll(renderer, /*executeInplace=*/true, nullptr);
    }

    // План без исполнения (для параллельного раннинга)
    std::vector<FlatNode> BuildSchedule(Renderer* renderer)
    {
        std::vector<FlatNode> flat;
        if (renderer == nullptr) { return flat; }
        Unroll(renderer, /*executeInplace=*/false, &flat);
        return flat;
    }

    // Параллель: создаём батчи в топологическом порядке, затем
    // сабмитим РЕАЛЬНЫЕ таски пассов с ожиданием их mt-deps.
    void ExecuteParallel(Renderer* renderer, TaskSystem& tasks)
    {
        auto flat = BuildSchedule(renderer);
        if (flat.empty()) { return; }

        const size_t N = passes_.size();

        // Группа «готовности пасса»: именно эти группы используются как зависимости
        std::vector<std::unique_ptr<TaskGroup>> passDone(N);
        for (size_t i = 0; i < N; ++i) {
            passDone[i] = std::make_unique<TaskGroup>();
        }

        for (const auto& n : flat) {
            const size_t passIdx = n.pass;
            if (!passes_[passIdx].exec) { continue; }

            // Обёртка-пасс: ждём mt-deps, исполняем тело, ждём внутренние саб-таски пасса
            tasks.Submit([this, renderer, n, passIdx, &passDone]() {
                // 1) дождаться всех указанных рантайм-зависимостей
                for (size_t dep : passes_[passIdx].mtDeps) {
                    if (dep < passDone.size()) {
                        passDone[dep]->Wait();
                    }
                }

                // 2) собственная группа этого пасса (для внутренних Dispatch’ей)
                PassContext ctx;
                ctx.renderer = renderer;
                ctx.batchIndex = n.batch;
                ctx.passName = passes_[passIdx].name;
                ctx.group = passDone[passIdx].get();

                // тело пасса (внутри можно вызывать Dispatch(..., ctx.group))
                passes_[passIdx].exec(ctx);

                }, passDone[passIdx].get()); // эта таска завершится — пасс «готов»
        }

        for (size_t i = 0; i < N; ++i) {
            if (passDone[i]) {
                passDone[i]->Wait();
            }
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
                    ctx.group = nullptr; // последовательный путь: группе тут делать нечего
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
    std::vector<Pass> passes_;
    size_t            submitBatchIndex_ = (size_t)-1;
};