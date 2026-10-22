// The process's operator new/delete -- memory profiling plan M0 (docs/memory_profiling_plan.md).
//
// Replaces `#include "mimalloc-new-delete.h"` in main.cpp. The allocator calls are the header's,
// one for one (mimalloc 2.2.5, third_party/mimalloc/include/mimalloc-new-delete.h:34-62): same
// mi_* entry point per overload, same throwing/nothrow behaviour, same SAL annotations. What is
// added is a count per call into the calling thread's block (core/memory/MemCounters.h).
//
// Constraints, all of them from running inside operator new:
// * Nothing here may allocate through operator new (it would recurse). The blocks are a static
//   array, constant-initialised, so they exist before any dynamic initialiser runs -- static
//   constructors allocate long before WinMain.
// * The thread's block pointer is a plain `thread_local` pointer with a constant initialiser:
//   static TLS, no dynamic TLS init and no destructor, so it is safe on a thread that is starting
//   up or tearing down.
// * A block is written only by its owning thread (load + store, no lock prefix); readers race by
//   design. The overflow block is shared and uses real atomic adds.

#include "core/memory/MemCounters.h"

#include <atomic>
#include <cstddef>
#include <new>

#include <windows.h>

#pragma warning(push)
#pragma warning(disable: 28251) // inconsistent annotation for 'new' -- as main.cpp had it around the header
#include <mimalloc.h>

// Stay consistent with the VCRT declarations, exactly as mimalloc-new-delete.h does.
#if defined(_MSC_VER) && defined(_Ret_notnull_) && defined(_Post_writable_byte_size_)
#define MEM_DECL_NEW(n)         mi_decl_nodiscard mi_decl_restrict _Ret_notnull_ _Post_writable_byte_size_(n)
#define MEM_DECL_NEW_NOTHROW(n) mi_decl_nodiscard mi_decl_restrict _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(n)
#else
#define MEM_DECL_NEW(n)         mi_decl_nodiscard mi_decl_restrict
#define MEM_DECL_NEW_NOTHROW(n) mi_decl_nodiscard mi_decl_restrict
#endif

namespace
{
struct alignas(64) CounterBlock
{
    std::atomic<std::uint64_t> allocs{ 0 };
    std::atomic<std::uint64_t> frees{ 0 };
    std::atomic<std::uint64_t> allocBytes{ 0 };
    std::atomic<std::uint32_t> threadId{ 0 };
};

constinit CounterBlock g_blocks[mem::kMaxThreadCounterBlocks];
constinit CounterBlock g_overflow;
constinit std::atomic<std::uint32_t> g_nextBlock{ 0 };
constinit thread_local CounterBlock* t_block = nullptr;

__declspec(noinline) CounterBlock* AcquireBlock() noexcept
{
    const std::uint32_t i = g_nextBlock.fetch_add(1, std::memory_order_relaxed);
    CounterBlock* b = &g_overflow;
    if (i < mem::kMaxThreadCounterBlocks)
    {
        b = &g_blocks[i];
        b->threadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    }
    t_block = b;
    return b;
}

inline CounterBlock* ThisThreadBlock() noexcept
{
    CounterBlock* b = t_block;
    return b != nullptr ? b : AcquireBlock();
}

inline void Add(std::atomic<std::uint64_t>& counter, std::uint64_t v, bool shared) noexcept
{
    if (shared)
    {
        counter.fetch_add(v, std::memory_order_relaxed);
    }
    else
    {
        counter.store(counter.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
    }
}

inline void CountAlloc(std::size_t n) noexcept
{
    CounterBlock* b = ThisThreadBlock();
    const bool shared = b == &g_overflow;
    Add(b->allocs, 1, shared);
    Add(b->allocBytes, n, shared);
}

inline void CountFree(const void* p) noexcept
{
    if (p == nullptr) { return; }
    CounterBlock* b = ThisThreadBlock();
    Add(b->frees, 1, b == &g_overflow);
}

void Read(const CounterBlock& b, mem::ThreadAllocCounters& out) noexcept
{
    out.threadId = b.threadId.load(std::memory_order_relaxed);
    out.allocs = b.allocs.load(std::memory_order_relaxed);
    out.frees = b.frees.load(std::memory_order_relaxed);
    out.allocBytes = b.allocBytes.load(std::memory_order_relaxed);
}
} // namespace

namespace mem
{
void ForEachThreadCounters(ThreadCountersVisitor visit, void* user)
{
    if (visit == nullptr) { return; }
    const std::uint32_t taken = g_nextBlock.load(std::memory_order_relaxed);
    const std::uint32_t count = taken < kMaxThreadCounterBlocks ? taken : kMaxThreadCounterBlocks;
    ThreadAllocCounters c;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        Read(g_blocks[i], c);
        visit(i, c, user);
    }
    if (taken > kMaxThreadCounterBlocks)
    {
        Read(g_overflow, c);
        visit(kMaxThreadCounterBlocks, c, user);
    }
}

std::uint32_t CurrentThreadCounterIndex()
{
    const CounterBlock* b = ThisThreadBlock();
    return b == &g_overflow ? kMaxThreadCounterBlocks : static_cast<std::uint32_t>(b - g_blocks);
}
} // namespace mem

// ---- The replaced operators: mimalloc-new-delete.h:34-62, plus one count each. ----

void operator delete(void* p) noexcept { CountFree(p); mi_free(p); }
void operator delete[](void* p) noexcept { CountFree(p); mi_free(p); }

void operator delete  (void* p, const std::nothrow_t&) noexcept { CountFree(p); mi_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { CountFree(p); mi_free(p); }

MEM_DECL_NEW(n) void* operator new(std::size_t n) noexcept(false) { void* p = mi_new(n); CountAlloc(n); return p; }
MEM_DECL_NEW(n) void* operator new[](std::size_t n) noexcept(false) { void* p = mi_new(n); CountAlloc(n); return p; }

MEM_DECL_NEW_NOTHROW(n) void* operator new  (std::size_t n, const std::nothrow_t&) noexcept
{
    void* p = mi_new_nothrow(n);
    if (p != nullptr) { CountAlloc(n); }
    return p;
}
MEM_DECL_NEW_NOTHROW(n) void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    void* p = mi_new_nothrow(n);
    if (p != nullptr) { CountAlloc(n); }
    return p;
}

void operator delete  (void* p, std::size_t n) noexcept { CountFree(p); mi_free_size(p, n); }
void operator delete[](void* p, std::size_t n) noexcept { CountFree(p); mi_free_size(p, n); }

void operator delete  (void* p, std::align_val_t al) noexcept { CountFree(p); mi_free_aligned(p, static_cast<size_t>(al)); }
void operator delete[](void* p, std::align_val_t al) noexcept { CountFree(p); mi_free_aligned(p, static_cast<size_t>(al)); }
void operator delete  (void* p, std::size_t n, std::align_val_t al) noexcept { CountFree(p); mi_free_size_aligned(p, n, static_cast<size_t>(al)); }
void operator delete[](void* p, std::size_t n, std::align_val_t al) noexcept { CountFree(p); mi_free_size_aligned(p, n, static_cast<size_t>(al)); }
void operator delete  (void* p, std::align_val_t al, const std::nothrow_t&) noexcept { CountFree(p); mi_free_aligned(p, static_cast<size_t>(al)); }
void operator delete[](void* p, std::align_val_t al, const std::nothrow_t&) noexcept { CountFree(p); mi_free_aligned(p, static_cast<size_t>(al)); }

void* operator new  (std::size_t n, std::align_val_t al) noexcept(false) { void* p = mi_new_aligned(n, static_cast<size_t>(al)); CountAlloc(n); return p; }
void* operator new[](std::size_t n, std::align_val_t al) noexcept(false) { void* p = mi_new_aligned(n, static_cast<size_t>(al)); CountAlloc(n); return p; }
void* operator new  (std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
    void* p = mi_new_aligned_nothrow(n, static_cast<size_t>(al));
    if (p != nullptr) { CountAlloc(n); }
    return p;
}
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
    void* p = mi_new_aligned_nothrow(n, static_cast<size_t>(al));
    if (p != nullptr) { CountAlloc(n); }
    return p;
}

#pragma warning(pop)
