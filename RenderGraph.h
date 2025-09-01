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
        size_t       batchIndex = (size_t)-1;
        std::string  passName;
    };

    using ExecFn = std::function<void(PassContext)>;

    explicit RenderGraph(size_t submitBatchIndex = (size_t)-1)
        : submitBatchIndex_(submitBatchIndex) {
    }

    struct Pass {
        std::string           name;
        std::vector<size_t>   prereqs;   // индексы пассов, которые должны быть ДО этого
        ExecFn                exec;      // может быть пустым — «барьер/алиас» без работы
    };

    // плоский узел — используется при параллельном выполнении
    struct FlatNode { size_t pass; size_t batch; };

    size_t AddPass(const std::string& name,
        const std::vector<size_t>& prereqs,
        ExecFn fn) {
        passes_.push_back(Pass{ name, prereqs, std::move(fn) });
        return passes_.size() - 1;
    }

    // Однопоточное исполнение «как раньше».
    void Execute(Renderer* renderer) {
        if (renderer == nullptr) {
            return;
        }
        Unroll(renderer, /*executeInplace=*/true, nullptr);
    }

    // Построить плоский план (batch уже открыт), без выполнения.
    std::vector<FlatNode> BuildSchedule(Renderer* renderer) {
        std::vector<FlatNode> flat;
        if (renderer == nullptr) {
            return flat;
        }
        Unroll(renderer, /*executeInplace=*/false, &flat);
        return flat;
    }

    // Параллельное выполнение: разворачиваем → кидаем в ParallelFor.
    void ExecuteParallel(Renderer* renderer, TaskSystem& tasks) {
        auto flat = BuildSchedule(renderer);
        if (flat.empty()) {
            return;
        }

        tasks.ParallelFor(flat.size(), [this, renderer, &flat](size_t i) {
            const auto& n = flat[i];
            const auto& p = passes_[n.pass];
            if (!p.exec) {
                return;
            }
            PassContext ctx;
            ctx.renderer = renderer;
            ctx.batchIndex = n.batch;
            ctx.passName = p.name;
            p.exec(ctx);
            }, /*batchSize=*/1);
    }

    void Clear() {
        passes_.clear();
    }

private:
    // Общая развёртка: строим топологию, для каждого узла (по порядку):
    // - если executeInplace=true и есть exec → создаём batch (если нужен) и вызываем exec сразу;
    // - если executeInplace=false и есть exec → создаём batch (если нужен) и кладём FlatNode в outFlat.
    void Unroll(Renderer* renderer, bool executeInplace, std::vector<FlatNode>* outFlat) {
        const size_t N = passes_.size();
        if (N == 0u) {
            return;
        }

        // in-degree и список смежности
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
            if (indeg[i] == 0u) {
                q.push(i);
            }
        }

        if (!executeInplace && outFlat != nullptr) {
            outFlat->clear();
            outFlat->reserve(N);
        }

        size_t produced = 0;
        while (!q.empty()) {
            const size_t u = q.front();
            q.pop();

            const auto& pass = passes_[u];

            // только если у пасса есть работа — создаём/берём batch
            if (pass.exec) {
                const size_t batch =
                    (submitBatchIndex_ == (size_t)-1)
                    ? renderer->BeginSubmitBatch(pass.name)
                    : submitBatchIndex_;

                if (executeInplace) {
                    PassContext ctx;
                    ctx.renderer = renderer;
                    ctx.batchIndex = batch;
                    ctx.passName = pass.name;
                    pass.exec(ctx);
                }
                else {
                    if (outFlat != nullptr) {
                        outFlat->push_back(FlatNode{ u, batch });
                    }
                }
            }

            ++produced;

            for (size_t v : adj[u]) {
                if (v < N) {
                    if (indeg[v] > 0u) {
                        --indeg[v];
                    }
                    if (indeg[v] == 0u) {
                        q.push(v);
                    }
                }
            }
        }

        if (produced != N) {
            assert(false && "RenderGraph has a cycle!");
        }
    }

private:
    std::vector<Pass> passes_;
    size_t submitBatchIndex_ = (size_t)-1; // «родительский» batch; если -1 — открываем сами
};