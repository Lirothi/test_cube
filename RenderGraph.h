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

//class RenderGraph2 {
//public:
//    struct PassContext {
//        Renderer* renderer = nullptr;
//        size_t      batchIndex = (size_t)-1;
//        std::string passName;
//    };
//
//    using ExecFn = std::function<void(PassContext)>;
//
//    class Handle {
//    public:
//        Handle() = default;
//        bool Valid() const { return g_ && id_ != (size_t)-1; }
//
//        // Добавить дочерний КОНТЕЙНЕР (без тела; удобно как "группа")
//        Handle AddPass(const std::string& name) const {
//            if (!Valid()) { return {}; }
//            return g_->AddNode_(name, /*parent=*/id_, /*deps=*/{}, /*fn=*/nullptr, /*isContainer=*/true);
//        }
//
//        // Добавить дочерний ИСПОЛНЯЕМЫЙ пасс
//        Handle AddPass(const std::string& name,
//            const std::vector<Handle>& prereqs,
//            ExecFn fn) const {
//            if (!Valid()) { return {}; }
//            return g_->AddNode_(name, /*parent=*/id_, g_->Unwrap_(prereqs), std::move(fn), /*isContainer=*/false);
//        }
//
//        // Повесить зависимости на существующий узел (сюда можно передавать и контейнеры, и пассы)
//        void DependsOn(const std::vector<Handle>& prereqs) const {
//            if (!Valid()) { return; }
//            auto& n = g_->nodes_[id_];
//            auto v = g_->Unwrap_(prereqs);
//            n.deps.insert(n.deps.end(), v.begin(), v.end());
//        }
//
//        // Сделать узел исполняемым (если раньше был контейнером)
//        void SetExec(ExecFn fn) const {
//            if (!Valid()) { return; }
//            g_->nodes_[id_].exec = std::move(fn);
//        }
//
//        // Дать наружу индекс для диагностики (не обязателен)
//        size_t Idx() const { return id_; }
//
//    private:
//        friend class RenderGraph2;
//        Handle(RenderGraph2* g, size_t id) : g_(g), id_(id) {}
//        RenderGraph2* g_ = nullptr;
//        size_t       id_ = (size_t)-1;
//    };
//
//    // Корневой КОНТЕЙНЕР
//    Handle AddPass(const std::string& name) {
//        return AddNode_(name, /*parent=*/(size_t)-1, /*deps=*/{}, /*fn=*/nullptr, /*isContainer=*/true);
//    }
//
//    // Корневой ИСПОЛНЯЕМЫЙ пасс
//    Handle AddPass(const std::string& name,
//        const std::vector<Handle>& prereqs,
//        ExecFn fn) {
//        return AddNode_(name, /*parent=*/(size_t)-1, Unwrap_(prereqs), std::move(fn), /*isContainer=*/false);
//    }
//
//    // Добавить зависимость между ЛЮБЫМИ узлами (контейнеры/пассы)
//    void AddDependency(const Handle& before, const Handle& after) {
//        if (!before.Valid() || !after.Valid()) {
//            assert(false && "AddDependency: invalid handle");
//            return;
//        }
//        nodes_[after.id_].deps.push_back(before.id_);
//    }
//
//    void Clear() { nodes_.clear(); }
//
//    // Разворачивание и исполнение
//    void Execute(Renderer* renderer) {
//        if (renderer == nullptr) {
//            return;
//        }
//        if (nodes_.empty()) {
//            return;
//        }
//
//        // 1) Соберём список ИСПОЛНЯЕМЫХ узлов (exec != nullptr)
//        std::vector<size_t> passNodes;
//        passNodes.reserve(nodes_.size());
//        for (size_t i = 0; i < nodes_.size(); ++i) {
//            if (nodes_[i].exec) {
//                passNodes.push_back(i);
//            }
//        }
//        const size_t P = passNodes.size();
//        if (P == 0u) {
//            return;
//        }
//
//        std::unordered_map<size_t, size_t> nodeToPass;
//        nodeToPass.reserve(P * 2);
//        for (size_t i = 0; i < P; ++i) {
//            nodeToPass[passNodes[i]] = i;
//        }
//
//        // 2) Построим рёбра только между ИСПОЛНЯЕМЫМИ узлами,
//        //    раскрывая зависимости через контейнеры
//        std::vector<std::vector<size_t>> out(P);
//        std::vector<size_t> indeg(P, 0);
//
//        std::unordered_map<size_t, std::vector<size_t>> cacheAllPasses;
//        std::unordered_map<size_t, std::vector<size_t>> cacheRoots;
//
//        auto allPassesOf = [&](size_t nodeId) -> const std::vector<size_t>&{
//            auto it = cacheAllPasses.find(nodeId);
//            if (it != cacheAllPasses.end()) { return it->second; }
//            cacheAllPasses[nodeId] = CollectAllExec_(nodeId);
//            return cacheAllPasses[nodeId];
//            };
//        auto rootsOf = [&](size_t nodeId) -> const std::vector<size_t>&{
//            auto it = cacheRoots.find(nodeId);
//            if (it != cacheRoots.end()) { return it->second; }
//            cacheRoots[nodeId] = CollectRootsExec_(nodeId);
//            return cacheRoots[nodeId];
//            };
//        auto addEdgePP = [&](size_t fromNode, size_t toNode) {
//            auto ia = nodeToPass.find(fromNode);
//            auto ib = nodeToPass.find(toNode);
//            if (ia == nodeToPass.end() || ib == nodeToPass.end()) {
//                return;
//            }
//            out[ia->second].push_back(ib->second);
//            };
//
//        // 2.a deps у ИСПОЛНЯЕМЫХ узлов
//        for (size_t pi = 0; pi < P; ++pi) {
//            const size_t nidx = passNodes[pi];
//            const Node& n = nodes_[nidx];
//            for (size_t d : n.deps) {
//                if (d >= nodes_.size()) { continue; }
//                const Node& dn = nodes_[d];
//                if (dn.exec) {
//                    addEdgePP(d, nidx);
//                }
//                else {
//                    const auto& all = allPassesOf(d);
//                    for (size_t a : all) {
//                        addEdgePP(a, nidx);
//                    }
//                }
//            }
//        }
//
//        // 2.b deps у КОНТЕЙНЕРОВ: корни контейнера зависят от его deps
//        for (size_t nid = 0; nid < nodes_.size(); ++nid) {
//            const Node& g = nodes_[nid];
//            if (!g.children.size() || g.exec) {
//                continue;
//            }
//            if (g.deps.empty()) {
//                continue;
//            }
//            const auto& roots = rootsOf(nid);
//            if (roots.empty()) {
//                continue;
//            }
//            for (size_t d : g.deps) {
//                if (d >= nodes_.size()) { continue; }
//                const Node& dn = nodes_[d];
//                if (dn.exec) {
//                    for (size_t r : roots) {
//                        addEdgePP(d, r);
//                    }
//                }
//                else {
//                    const auto& allA = allPassesOf(d);
//                    for (size_t a : allA) {
//                        for (size_t r : roots) {
//                            addEdgePP(a, r);
//                        }
//                    }
//                }
//            }
//        }
//
//        // 3) indegree + топосорт
//        for (size_t a = 0; a < P; ++a) {
//            for (size_t b : out[a]) {
//                if (b < P) {
//                    ++indeg[b];
//                }
//            }
//        }
//        std::queue<size_t> q;
//        for (size_t i = 0; i < P; ++i) {
//            if (indeg[i] == 0u) {
//                q.push(i);
//            }
//        }
//        std::vector<size_t> topo; topo.reserve(P);
//        while (!q.empty()) {
//            const size_t u = q.front();
//            q.pop();
//            topo.push_back(u);
//            for (size_t v : out[u]) {
//                if (v < P) {
//                    if (indeg[v] > 0u) {
//                        --indeg[v];
//                    }
//                    if (indeg[v] == 0u) {
//                        q.push(v);
//                    }
//                }
//            }
//        }
//        if (topo.size() != P) {
//            assert(false && "RenderGraph: cycle detected after flattening!");
//            return;
//        }
//
//        // 4) Исполнение
//        for (size_t ord = 0; ord < topo.size(); ++ord) {
//            const size_t passIdx = topo[ord];
//            const size_t nodeIdx = passNodes[passIdx];
//            Node& n = nodes_[nodeIdx];
//
//            const size_t batch = renderer->BeginSubmitBatch(n.name);
//            if (n.exec) {
//                PassContext ctx;
//                ctx.renderer = renderer;
//                ctx.batchIndex = batch;
//                ctx.passName = n.name;
//                n.exec(ctx);
//            }
//        }
//    }
//
//private:
//    struct Node {
//        std::string name;
//        size_t parent = (size_t)-1;         // контейнер (если есть)
//        std::vector<size_t> deps;           // зависимости на ЛЮБЫЕ узлы
//        std::vector<size_t> children;       // дети (контейнеры/пассы)
//        ExecFn exec;                        // null => контейнер, non-null => исполняемый
//    };
//    std::vector<Node> nodes_;
//
//    Handle AddNode_(const std::string& name,
//        size_t parent,
//        std::vector<size_t> deps,
//        ExecFn fn,
//        bool isContainer) {
//        Node n;
//        n.name = name;
//        n.parent = parent;
//        n.deps = std::move(deps);
//        n.exec = (isContainer ? ExecFn{} : std::move(fn));
//        nodes_.push_back(std::move(n));
//        const size_t idx = nodes_.size() - 1;
//        if (parent != (size_t)-1) {
//            if (parent < nodes_.size()) {
//                nodes_[parent].children.push_back(idx);
//            }
//        }
//        return Handle(this, idx);
//    }
//
//    std::vector<size_t> Unwrap_(const std::vector<Handle>& hv) const {
//        std::vector<size_t> r;
//        r.reserve(hv.size());
//        for (const auto& h : hv) {
//            r.push_back(h.id_);
//        }
//        return r;
//    }
//
//    // собрать все ИСПОЛНЯЕМЫЕ узлы в поддереве
//    std::vector<size_t> CollectAllExec_(size_t nodeId) const {
//        std::vector<size_t> out;
//        if (nodeId >= nodes_.size()) {
//            return out;
//        }
//        const Node& n = nodes_[nodeId];
//        if (n.exec) {
//            out.push_back(nodeId);
//        }
//        if (!n.children.empty()) {
//            std::vector<size_t> st = n.children;
//            while (!st.empty()) {
//                size_t u = st.back(); st.pop_back();
//                if (u >= nodes_.size()) {
//                    continue;
//                }
//                const Node& m = nodes_[u];
//                if (m.exec) {
//                    out.push_back(u);
//                }
//                if (!m.children.empty()) {
//                    for (size_t c : m.children) {
//                        st.push_back(c);
//                    }
//                }
//            }
//        }
//        return out;
//    }
//
//    // корни поддерева: исполняемые узлы без ВНУТРЕННИХ предков
//    std::vector<size_t> CollectRootsExec_(size_t nodeId) const {
//        std::vector<size_t> roots;
//        const std::vector<size_t> all = CollectAllExec_(nodeId);
//        if (all.empty()) {
//            return roots;
//        }
//        std::unordered_set<size_t> setAll(all.begin(), all.end());
//        std::unordered_map<size_t, size_t> indeg;
//        for (size_t p : all) {
//            indeg[p] = 0;
//        }
//        auto addEdgeIfInternal = [&](size_t a, size_t b) {
//            if (setAll.count(a) != 0u && setAll.count(b) != 0u) {
//                ++indeg[b];
//            }
//            };
//        for (size_t p : all) {
//            const Node& n = nodes_[p];
//            for (size_t d : n.deps) {
//                if (d >= nodes_.size()) { continue; }
//                const Node& dn = nodes_[d];
//                if (dn.exec) {
//                    addEdgeIfInternal(d, p);
//                }
//                else {
//                    const auto sub = CollectAllExec_(d);
//                    for (size_t a : sub) {
//                        addEdgeIfInternal(a, p);
//                    }
//                }
//            }
//        }
//        for (auto& kv : indeg) {
//            if (kv.second == 0u) {
//                roots.push_back(kv.first);
//            }
//        }
//        return roots;
//    }
//};