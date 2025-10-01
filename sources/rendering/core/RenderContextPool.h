#pragma once
#include "rendering/core/RenderContext.h"
#include <vector>
#include <mutex>
#include <memory>
#include <utility>

class RenderContextPool {
public:
    // Move-only RAII — automatically returns the context to the pool
    class Handle {
    public:
        Handle() = default;
        Handle(RenderContextPool* p, RenderContext* c) : pool_(p), ctx_(c) {}
        Handle(Handle&& o) noexcept { pool_ = o.pool_; ctx_ = o.ctx_; o.pool_ = nullptr; o.ctx_ = nullptr; }
        Handle& operator=(Handle&& o) noexcept {
            if (this != &o) { release_(); pool_ = o.pool_; ctx_ = o.ctx_; o.pool_ = nullptr; o.ctx_ = nullptr; }
            return *this;
        }
        ~Handle() { release_(); }

        RenderContext& ref()             { return *ctx_; }
        const RenderContext& ref() const { return *ctx_; }
        RenderContext* operator->()      { return ctx_; }
        operator const RenderContext&() const { return *ctx_; }

        // Non-copyable
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    private:
        void release_() {
            if (pool_ && ctx_) { pool_->Release(ctx_); pool_ = nullptr; ctx_ = nullptr; }
        }
        RenderContextPool* pool_ = nullptr;
        RenderContext*      ctx_  = nullptr;
    };

public:
    Handle Acquire();
    void ResetForFrame();
    void TrimTLS(size_t keepPerThread = 8);

private:
    friend class Handle;
    void Release(RenderContext* c);

    static constexpr size_t kTLSCap = 64;

    struct TLS {
        std::vector<RenderContext*> stash;
    };
    static thread_local TLS tls_;

    std::mutex mtx_;
    std::vector<RenderContext*> free_;
    std::vector<std::unique_ptr<RenderContext>> all_;
};