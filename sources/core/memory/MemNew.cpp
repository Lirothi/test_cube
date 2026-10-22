// The process's operator new/delete -- memory profiling plan M0 + M3 (docs/memory_profiling_plan.md).
//
// Replaces `#include "mimalloc-new-delete.h"` in main.cpp. The allocator calls are the header's,
// one for one (mimalloc 2.2.5, third_party/mimalloc/include/mimalloc-new-delete.h:34-62): same
// mi_* entry point per overload, same throwing/nothrow behaviour, same SAL annotations. What is
// added is a count per call into the calling thread's block (core/memory/MemCounters.h), and with
// `--memtrack` the CPU tags: the pointer and the thread's MemScope tag go into a striped map, the
// usable size into the tag's per-thread total (M3).
//
// Constraints, all of them from running inside operator new:
// * Nothing here may allocate through operator new (it would recurse). The blocks, the tag stack
//   and the map's stripes are static, constant-initialised, so they exist before any dynamic
//   initialiser runs -- static constructors allocate long before WinMain. The map's tables come
//   from VirtualAlloc.
// * Thread-local state is a plain `thread_local` POD with a constant initialiser: static TLS, no
//   dynamic TLS init and no destructor, so it is safe on a thread that is starting up or tearing
//   down.
// * A block is written only by its owning thread (load + store, no lock prefix); readers race by
//   design. The overflow block is shared and uses real atomic adds.

#include "core/memory/MemCounters.h"
#include "core/memory/MemScope.h"
#include "core/memory/MemTrace.h"

#include <atomic>
#include <cstddef>
#include <cwchar>
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
    // --memtrack only: live bytes / allocations by tag, as this thread changed them (a free on
    // another thread subtracts there; the sum over blocks is exact).
    std::atomic<std::int64_t> tagBytes[kMemTagCount]{};
    std::atomic<std::int64_t> tagCount[kMemTagCount]{};
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

inline void AddSigned(std::atomic<std::int64_t>& counter, std::int64_t v, bool shared) noexcept
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

// ---- --memtrack: decided once, at the first allocation, from the raw command line ----
// 0 = not read yet, 1 = off, 2 = on. Reading it twice from two threads gives the same answer.
constinit std::atomic<int> g_trackState{ 0 };

__declspec(noinline) bool ReadTrackSwitch() noexcept
{
    const wchar_t* cmd = GetCommandLineW();
    // A CPU memory trace (M4) and the leak readout's per-tag verdict (M6) need the map as much as --memtrack does.
    const bool on = cmd != nullptr && (std::wcsstr(cmd, L"--memtrack") != nullptr || std::wcsstr(cmd, L"--mem-readout") != nullptr ||
                                       memtrace::WantsCpuRaw(cmd));
    g_trackState.store(on ? 2 : 1, std::memory_order_relaxed);
    return on;
}

inline bool TrackingOn() noexcept
{
    const int s = g_trackState.load(std::memory_order_relaxed);
    return s == 2 || (s == 0 && ReadTrackSwitch());
}

// ---- The per-thread tag stack (MemScope.h) ----
constexpr std::uint32_t kTagStackDepth = 16;
struct TagStack
{
    std::uint8_t tags[kTagStackDepth];
    std::uint32_t depth;
};
constinit thread_local TagStack t_tags{};

inline std::uint8_t CurrentTagRaw() noexcept
{
    const std::uint32_t d = t_tags.depth;
    if (d == 0) { return static_cast<std::uint8_t>(MemTag::Untagged); }
    return t_tags.tags[(d < kTagStackDepth ? d : kTagStackDepth) - 1];
}

// ---- The pointer map: pointer -> tag, 256 stripes, linear probing, backward-shift deletion ----
// The size is not stored: mi_usable_size gives it back at the free. Load stays <= 50 %.
// 256 stripes, not UE's 16: --mem-bench measured 16 stripes at +356 ns per new/delete pair with 8
// threads allocating (every other operation found its stripe taken) against +26 ns on one thread.
constexpr std::uint32_t kStripeShift = 56; // the top 8 bits of the hash pick the stripe
constexpr std::uint32_t kStripeCount = 1u << (64 - kStripeShift);
constexpr std::uint32_t kStripeMinCapacity = 256;

struct alignas(64) Stripe
{
    SRWLOCK lock;          // SRWLOCK_INIT is all-zero, so constant initialisation is the init
    std::uintptr_t* keys;  // 0 = empty
    std::uint8_t* tags;
    std::uint32_t capacity;
    std::uint32_t count;
    bool traced;           // M4: this stripe's inserts and removes go to the memory trace (under `lock`)
};
constinit Stripe g_stripes[kStripeCount] = {};
constinit std::atomic<std::uint64_t> g_mapBytes{ 0 };
constinit std::atomic<std::uint64_t> g_unknownFrees{ 0 };

inline std::uint64_t HashPtr(std::uintptr_t p) noexcept
{
    return (static_cast<std::uint64_t>(p) >> 4) * 0x9E3779B97F4A7C15ull;
}

inline std::uint32_t HomeSlot(std::uint64_t h, std::uint32_t capacity) noexcept
{
    // Bits 24..55 for the slot, the top eight for the stripe: independent.
    return static_cast<std::uint32_t>(h >> 24) & (capacity - 1);
}

// Doubles the stripe's table (VirtualAlloc, never the heap being measured). Under the stripe lock.
bool Grow(Stripe& s) noexcept
{
    const std::uint32_t newCapacity = s.capacity == 0 ? kStripeMinCapacity : s.capacity * 2;
    const std::size_t bytes = static_cast<std::size_t>(newCapacity) * (sizeof(std::uintptr_t) + 1);
    void* block = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == nullptr) { return false; }
    auto* keys = static_cast<std::uintptr_t*>(block);
    auto* tags = reinterpret_cast<std::uint8_t*>(keys + newCapacity);
    for (std::uint32_t i = 0; i < s.capacity; ++i)
    {
        const std::uintptr_t k = s.keys[i];
        if (k == 0) { continue; }
        std::uint32_t j = HomeSlot(HashPtr(k), newCapacity);
        while (keys[j] != 0) { j = (j + 1) & (newCapacity - 1); }
        keys[j] = k;
        tags[j] = s.tags[i];
    }
    if (s.keys != nullptr)
    {
        VirtualFree(s.keys, 0, MEM_RELEASE);
        g_mapBytes.fetch_sub(static_cast<std::uint64_t>(s.capacity) * (sizeof(std::uintptr_t) + 1), std::memory_order_relaxed);
    }
    g_mapBytes.fetch_add(bytes, std::memory_order_relaxed);
    s.keys = keys;
    s.tags = tags;
    s.capacity = newCapacity;
    return true;
}

// `size` and `stack` only feed the trace (M4): the event is emitted under the stripe lock, so it
// is ordered with the free of the same address and with the trace's start snapshot.
bool MapInsert(std::uintptr_t p, std::uint8_t tag, std::uint64_t size, std::uint32_t stack) noexcept
{
    const std::uint64_t h = HashPtr(p);
    Stripe& s = g_stripes[h >> kStripeShift];
    AcquireSRWLockExclusive(&s.lock);
    bool ok = true;
    if ((s.count + 1) * 2 > s.capacity) { ok = Grow(s); }
    if (ok)
    {
        const std::uint32_t mask = s.capacity - 1;
        std::uint32_t i = HomeSlot(h, s.capacity);
        while (s.keys[i] != 0 && s.keys[i] != p) { i = (i + 1) & mask; }
        const bool existed = s.keys[i] != 0;
        if (!existed) { ++s.count; }
        s.keys[i] = p; // a key already there was freed behind our back (a foreign free); its tag is replaced
        s.tags[i] = tag;
        if (s.traced)
        {
            if (existed) { memtrace::EmitFree(p, memtrace::kHeapCpu); }
            memtrace::EmitAlloc(p, size, tag, memtrace::kHeapCpu, memtrace::kKindCpu, stack, nullptr);
        }
    }
    ReleaseSRWLockExclusive(&s.lock);
    return ok;
}

bool MapRemove(std::uintptr_t p, std::uint8_t& outTag) noexcept
{
    const std::uint64_t h = HashPtr(p);
    Stripe& s = g_stripes[h >> kStripeShift];
    AcquireSRWLockExclusive(&s.lock);
    bool found = false;
    if (s.capacity != 0)
    {
        const std::uint32_t mask = s.capacity - 1;
        std::uint32_t i = HomeSlot(h, s.capacity);
        while (s.keys[i] != 0 && s.keys[i] != p) { i = (i + 1) & mask; }
        if (s.keys[i] == p)
        {
            found = true;
            outTag = s.tags[i];
            // Backward-shift deletion: pull later entries of the run into the hole when their home
            // slot is not between the hole and themselves.
            std::uint32_t j = i;
            for (;;)
            {
                j = (j + 1) & mask;
                const std::uintptr_t k = s.keys[j];
                if (k == 0) { break; }
                const std::uint32_t home = HomeSlot(HashPtr(k), s.capacity);
                const bool between = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
                if (!between)
                {
                    s.keys[i] = k;
                    s.tags[i] = s.tags[j];
                    i = j;
                }
            }
            s.keys[i] = 0;
            --s.count;
            if (s.traced) { memtrace::EmitFree(p, memtrace::kHeapCpu); }
        }
    }
    ReleaseSRWLockExclusive(&s.lock);
    return found;
}

__declspec(noinline) void TrackAlloc(void* p, CounterBlock* b, bool shared) noexcept
{
    const std::uint8_t tag = CurrentTagRaw();
    const std::uint64_t size = mi_usable_size(p);
    const std::uint32_t stack = memtrace::CaptureCpuStack(size); // 0 unless a CPU trace wants this one
    if (MapInsert(reinterpret_cast<std::uintptr_t>(p), tag, size, stack))
    {
        AddSigned(b->tagBytes[tag], static_cast<std::int64_t>(size), shared);
        AddSigned(b->tagCount[tag], 1, shared);
    }
}

// Before mi_free: mi_usable_size needs the block alive.
__declspec(noinline) void TrackFree(void* p, CounterBlock* b, bool shared) noexcept
{
    std::uint8_t tag = 0;
    if (MapRemove(reinterpret_cast<std::uintptr_t>(p), tag))
    {
        AddSigned(b->tagBytes[tag], -static_cast<std::int64_t>(mi_usable_size(p)), shared);
        AddSigned(b->tagCount[tag], -1, shared);
    }
    else
    {
        g_unknownFrees.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void CountAlloc(void* p, std::size_t n) noexcept
{
    CounterBlock* b = ThisThreadBlock();
    const bool shared = b == &g_overflow;
    Add(b->allocs, 1, shared);
    Add(b->allocBytes, n, shared);
    if (TrackingOn()) { TrackAlloc(p, b, shared); }
}

inline void CountFree(void* p) noexcept
{
    if (p == nullptr) { return; }
    CounterBlock* b = ThisThreadBlock();
    const bool shared = b == &g_overflow;
    Add(b->frees, 1, shared);
    if (TrackingOn()) { TrackFree(p, b, shared); }
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

void PushTag(MemTag tag) noexcept
{
    const std::uint32_t d = t_tags.depth;
    if (d < kTagStackDepth) { t_tags.tags[d] = static_cast<std::uint8_t>(tag); }
    t_tags.depth = d + 1;
}

void PopTag() noexcept
{
    if (t_tags.depth > 0) { --t_tags.depth; }
}

MemTag CurrentTag() noexcept
{
    return static_cast<MemTag>(CurrentTagRaw());
}

bool MemTrackEnabled()
{
    return TrackingOn();
}

void ReadCpuTagTotals(CpuTagTotals& out)
{
    out = CpuTagTotals{};
    if (!TrackingOn()) { return; }
    const std::uint32_t taken = g_nextBlock.load(std::memory_order_relaxed);
    const std::uint32_t count = taken < kMaxThreadCounterBlocks ? taken : kMaxThreadCounterBlocks;
    const auto add = [&](const CounterBlock& b) {
        for (std::uint32_t t = 0; t < kMemTagCount; ++t)
        {
            out.bytes[t] += b.tagBytes[t].load(std::memory_order_relaxed);
            out.count[t] += b.tagCount[t].load(std::memory_order_relaxed);
        }
    };
    for (std::uint32_t i = 0; i < count; ++i) { add(g_blocks[i]); }
    add(g_overflow);
    out.unknownFrees = g_unknownFrees.load(std::memory_order_relaxed);
    out.mapBytes = g_mapBytes.load(std::memory_order_relaxed);
    for (std::uint32_t s = 0; s < kStripeCount; ++s)
    {
        out.mapEntries += g_stripes[s].count; // racy read of a counter, fine for a report
    }
}

void BeginCpuTrace()
{
    if (!TrackingOn()) { return; }
    for (Stripe& s : g_stripes)
    {
        AcquireSRWLockExclusive(&s.lock);
        s.traced = true;
        for (std::uint32_t i = 0; i < s.capacity; ++i)
        {
            const std::uintptr_t p = s.keys[i];
            if (p == 0) { continue; }
            // Alive: its free waits on this lock before mi_free, so the usable size can be read.
            memtrace::EmitAlloc(p, mi_usable_size(reinterpret_cast<void*>(p)), s.tags[i], memtrace::kHeapCpu,
                                memtrace::kKindCpu | memtrace::kKindSynthetic, 0, nullptr);
        }
        ReleaseSRWLockExclusive(&s.lock);
    }
}

void EndCpuTrace(CpuTraceTotals& out)
{
    out = CpuTraceTotals{};
    for (Stripe& s : g_stripes)
    {
        AcquireSRWLockExclusive(&s.lock);
        s.traced = false;
        for (std::uint32_t i = 0; i < s.capacity; ++i)
        {
            const std::uintptr_t p = s.keys[i];
            if (p == 0) { continue; }
            out.bytes[s.tags[i]] += static_cast<std::int64_t>(mi_usable_size(reinterpret_cast<void*>(p)));
            out.count[s.tags[i]] += 1;
        }
        ReleaseSRWLockExclusive(&s.lock);
    }
}
} // namespace mem

// ---- The replaced operators: mimalloc-new-delete.h:34-62, plus one count each. ----

void operator delete(void* p) noexcept { CountFree(p); mi_free(p); }
void operator delete[](void* p) noexcept { CountFree(p); mi_free(p); }

void operator delete  (void* p, const std::nothrow_t&) noexcept { CountFree(p); mi_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { CountFree(p); mi_free(p); }

MEM_DECL_NEW(n) void* operator new(std::size_t n) noexcept(false) { void* p = mi_new(n); CountAlloc(p, n); return p; }
MEM_DECL_NEW(n) void* operator new[](std::size_t n) noexcept(false) { void* p = mi_new(n); CountAlloc(p, n); return p; }

MEM_DECL_NEW_NOTHROW(n) void* operator new  (std::size_t n, const std::nothrow_t&) noexcept
{
    void* p = mi_new_nothrow(n);
    if (p != nullptr) { CountAlloc(p, n); }
    return p;
}
MEM_DECL_NEW_NOTHROW(n) void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    void* p = mi_new_nothrow(n);
    if (p != nullptr) { CountAlloc(p, n); }
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

void* operator new  (std::size_t n, std::align_val_t al) noexcept(false) { void* p = mi_new_aligned(n, static_cast<size_t>(al)); CountAlloc(p, n); return p; }
void* operator new[](std::size_t n, std::align_val_t al) noexcept(false) { void* p = mi_new_aligned(n, static_cast<size_t>(al)); CountAlloc(p, n); return p; }
void* operator new  (std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
    void* p = mi_new_aligned_nothrow(n, static_cast<size_t>(al));
    if (p != nullptr) { CountAlloc(p, n); }
    return p;
}
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept
{
    void* p = mi_new_aligned_nothrow(n, static_cast<size_t>(al));
    if (p != nullptr) { CountAlloc(p, n); }
    return p;
}

#pragma warning(pop)
