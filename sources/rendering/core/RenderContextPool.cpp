#include "rendering/core/RenderContextPool.h"
thread_local RenderContextPool::TLS RenderContextPool::tls_{};

RenderContextPool::Handle RenderContextPool::Acquire() {
    // 1) попытка из thread_local стэша
    if (!tls_.stash.empty()) {
        RenderContext* c = tls_.stash.back();
        tls_.stash.pop_back();
        c->ClearFast();
        return Handle(this, c);
    }
    // 2) глобальный пул
    {
        std::lock_guard<std::mutex> _l(mtx_);
        if (!free_.empty()) {
            RenderContext* c = free_.back();
            free_.pop_back();
            c->ClearFast();
            return Handle(this, c);
        }
    }
    // 3) создаём новый
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
    // ничего не делаем специально: контексты очищаются при Acquire().
    // При желании можно «подрезать» TLS-кеш:
    // TrimTLS(8);
}

// можно периодически триммить TLS-кеш (например, раз в N кадров)
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
    // небольшой per-thread кеш, чтобы не бегать за мьютексом
    if (tls_.stash.size() < kTLSCap) {
        tls_.stash.push_back(c);
        return;
    }
    std::lock_guard<std::mutex> _l(mtx_);
    free_.push_back(c);
}