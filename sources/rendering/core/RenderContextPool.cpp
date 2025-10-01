#include "rendering/core/RenderContextPool.h"
thread_local RenderContextPool::TLS RenderContextPool::tls_{};

RenderContextPool::Handle RenderContextPool::Acquire() {
    // 1) Try the thread-local stash first
    if (!tls_.stash.empty()) {
        RenderContext* c = tls_.stash.back();
        tls_.stash.pop_back();
        c->ClearFast();
        return Handle(this, c);
    }
    // 2) Global pool
    {
        std::lock_guard<std::mutex> _l(mtx_);
        if (!free_.empty()) {
            RenderContext* c = free_.back();
            free_.pop_back();
            c->ClearFast();
            return Handle(this, c);
        }
    }
    // 3) Create a new context
    auto up = std::make_unique<RenderContext>();
    RenderContext* c = up.get();
    {
        std::lock_guard<std::mutex> _l(mtx_);
        all_.push_back(std::move(up));
    }
    c->ClearFast();
    return Handle(this, c);
}

void RenderContextPool::ResetForFrame() {
    // Nothing specific to do: contexts are cleared during Acquire().
    // Optionally trim the TLS cache:
    // TrimTLS(8);
}

// TLS cache can be trimmed periodically (e.g., once every N frames)
void RenderContextPool::TrimTLS(size_t keepPerThread) {
    if (tls_.stash.size() > keepPerThread) {
        std::lock_guard<std::mutex> _l(mtx_);
        while (tls_.stash.size() > keepPerThread) {
            free_.push_back(tls_.stash.back());
            tls_.stash.pop_back();
        }
    }
}

void RenderContextPool::Release(RenderContext* c) {
    if (!c) { return; }
    // Small per-thread cache to avoid locking
    if (tls_.stash.size() < kTLSCap) {
        tls_.stash.push_back(c);
        return;
    }
    std::lock_guard<std::mutex> _l(mtx_);
    free_.push_back(c);
}