#pragma once
#include <functional>
#include <string>
#include <vector>
#include <queue>
#include <cassert>
#include <unordered_set>
#include "Renderer.h"


class RenderGraph {
public:
    struct PassContext {
        Renderer* renderer = nullptr;
        size_t    batchIndex = (size_t)-1;
        std::string passName;
    };

    using ExecFn = std::function<void(PassContext)>;

    RenderGraph(size_t submitBatchIndex = (size_t)-1)
		: submitBatchIndex_(submitBatchIndex) {
	}

    struct Pass {
        std::string name;
        std::vector<size_t> prereqs; // индексы пассов, которые должны быть ДО этого
        ExecFn exec;
    };

    // Добавить пасс, вернуть его индекс — используйте для зависимостей следующих пассов.
    size_t AddPass(const std::string& name,
        const std::vector<size_t>& prereqs,
        ExecFn fn) {
        passes_.push_back(Pass{ name, prereqs, std::move(fn) });
        return passes_.size() - 1;
    }

    // Выполнить: топологическая сортировка (Kahn), без внутренних вейтов.
    void Execute(Renderer* renderer) {
        if (renderer == nullptr) {
            return;
        }
        const size_t N = passes_.size();
        if (N == 0u) {
            return;
        }

        // посчитаем входящие рёбра
        std::vector<size_t> indeg(N, 0);
        std::vector<std::vector<size_t>> out(N);
        for (size_t i = 0; i < N; ++i) {
            for (size_t d : passes_[i].prereqs) {
                if (d < N) {
                    ++indeg[i];
                    out[d].push_back(i);
                }
            }
        }

        // очередь «готовых» (in-degree == 0) — стабильно по порядку добавления
        std::queue<size_t> q;
        for (size_t i = 0; i < N; ++i) {
            if (indeg[i] == 0u) {
                q.push(i);
            }
        }

        size_t done = 0;
        while (!q.empty()) {
            const size_t u = q.front();
            q.pop();

            // регистрируем бакет под этот пасс
            const size_t batch = submitBatchIndex_ == (size_t)-1 ? renderer->BeginSubmitBatch(passes_[u].name) : submitBatchIndex_;

            // исполняем лямбду
            if (passes_[u].exec) {
                PassContext ctx;
                ctx.renderer = renderer;
                ctx.batchIndex = batch;
                ctx.passName = passes_[u].name;
                passes_[u].exec(ctx);
            }
            ++done;

            // раскрываем зависящие
            for (size_t v : out[u]) {
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

        // если цикл — лучше упасть ассершкой (в дебаге)
        if (done != N) {
            assert(false && "RenderGraph has a cycle!");
        }
    }

    void Clear() {
        passes_.clear();
    }

private:
    std::vector<Pass> passes_;
	size_t submitBatchIndex_ = (size_t)-1;
};
