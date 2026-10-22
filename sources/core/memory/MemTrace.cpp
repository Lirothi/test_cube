// The memory trace -- memory profiling plan M4. The format and the ordering rules are in MemTrace.h;
// tools/memtrace.py reads it.
//
// File: records `[type u8][payload length u32][payload]`, little endian -- H header, T tags, P heaps,
// D modules, C chunk (a writer's slice of one thread buffer: `[buffer id u32][events]`), E end
// (the process's own totals), Y symbols. Events inside a chunk are a varint stream (LEB128; signed
// values zigzag): Reset{thread}, Alloc{serial delta, key delta, size, tag, heap, kind, stack,
// [name]}, Free{serial delta, key delta, heap}, Marker{serial delta, qpc, frame + 1 (0 = periodic)},
// Stack{id, n, frame deltas}. Deltas run per buffer fill and restart at its Reset.
//
// Nothing on the emission path allocates: buffers, the stack table and the address set live on
// VirtualAlloc pages; the per-thread pointer is a constant-initialised POD (MemNew.cpp's rules).

#include "core/memory/MemTrace.h"

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/logging/Log.h"
#include "core/memory/MemCounters.h"

#pragma comment(lib, "dbghelp.lib")

namespace memtrace
{
namespace
{
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kBufferBytes = 64u * 1024u;
constexpr std::uint32_t kNameMax = 480u;           // a GPU name longer than this is cut
constexpr std::uint32_t kMaxBuffers = 2048u;       // 128 MB, if the writer ever fell that far behind
constexpr std::uint64_t kMarkerPeriod = 4095u;     // MemoryTrace.cpp:37 -- a marker every 4096 serials
constexpr std::uint32_t kMaxFrames = 32u;
constexpr std::uint64_t kBigAlloc = 64u * 1024u;
constexpr std::size_t kKeepTraces = 10u;           // owner's Q1: the last 10
constexpr std::uint32_t kStageBytes = 1u << 20;

enum : std::uint8_t { kEvReset = 1, kEvAlloc = 2, kEvFree = 3, kEvMarker = 4, kEvStack = 5 };
enum : std::uint8_t
{
    kRecHeader = 'H', kRecTags = 'T', kRecHeaps = 'P', kRecModules = 'D', kRecChunk = 'C', kRecEnd = 'E', kRecSymbols = 'Y',
};

inline std::uint8_t* PutVar(std::uint8_t* p, std::uint64_t v) noexcept
{
    while (v >= 0x80u)
    {
        *p++ = static_cast<std::uint8_t>(v | 0x80u);
        v >>= 7;
    }
    *p++ = static_cast<std::uint8_t>(v);
    return p;
}

inline std::uint64_t ZigZag(std::int64_t v) noexcept
{
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}

inline std::uint64_t Qpc() noexcept
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<std::uint64_t>(t.QuadPart);
}

// ---- Per-thread buffers ----
struct Buffer
{
    std::atomic<std::uint32_t> committed; // bytes the owner has published (release)
    std::atomic<std::uint32_t> retired;   // the owner moved on: recycled once drained
    std::uint32_t readPos;                // the writer's
    std::uint32_t id;
    std::uint64_t prevSerial;             // the owner's encoder state, reset per fill
    std::uint64_t prevKey;
    std::uint8_t data[kBufferBytes];
};

constinit std::atomic<Buffer*> g_buffers[kMaxBuffers] = {};
constinit std::atomic<std::uint32_t> g_bufferCount{ 0 };
constinit SRWLOCK g_freeLock = SRWLOCK_INIT;
constinit std::uint32_t g_freeList[kMaxBuffers] = {};
constinit std::uint32_t g_freeCount = 0;
constinit thread_local Buffer* t_buffer = nullptr;
constinit thread_local std::uint32_t t_sampleCounter = 0;
constinit std::atomic<std::uint64_t> g_serial{ 1 };
constinit std::atomic<std::uint64_t> g_drops{ 0 };

// ---- State ----
constinit std::atomic<bool> g_cpuOn{ false };
constinit std::atomic<bool> g_gpuOn{ false };
constinit std::atomic<bool> g_markersOn{ false };
constinit std::atomic<GpuHook> g_gpuHook{ nullptr };
constinit StackMode g_stackMode = StackMode::Big; // set before g_cpuOn/g_gpuOn are released
constinit std::uint32_t g_sampleEvery = 0;
constinit HANDLE g_wake = nullptr;

Options g_opts;
bool g_configured = false;
bool g_started = false;
bool g_stopped = false;
HANDLE g_writer = nullptr;
std::atomic<bool> g_writerStop{ false };
std::string g_path;
std::uint64_t g_startQpc = 0;
std::uint64_t g_startFrameActual = 0;

Buffer* NewBuffer() noexcept
{
    Buffer* b = nullptr;
    AcquireSRWLockExclusive(&g_freeLock);
    if (g_freeCount > 0) { b = g_buffers[g_freeList[--g_freeCount]].load(std::memory_order_relaxed); }
    ReleaseSRWLockExclusive(&g_freeLock);
    if (b == nullptr)
    {
        std::uint32_t i = g_bufferCount.load(std::memory_order_relaxed);
        do
        {
            if (i >= kMaxBuffers) { return nullptr; }
        } while (!g_bufferCount.compare_exchange_weak(i, i + 1, std::memory_order_relaxed));
        void* mem = VirtualAlloc(nullptr, sizeof(Buffer), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (mem == nullptr) { return nullptr; } // slot i stays empty; the writer skips it
        b = new (mem) Buffer; // the pages are zero: committed, retired, readPos start at 0
        b->id = i;
        g_buffers[i].store(b, std::memory_order_release);
    }
    b->prevSerial = 0;
    b->prevKey = 0;
    return b;
}

// Room for `need` bytes in the calling thread's buffer (a fresh fill starts with Reset), or null:
// no buffer to be had, the event is dropped.
std::uint8_t* Reserve(Buffer*& out, std::uint32_t need) noexcept
{
    Buffer* b = t_buffer;
    if (b != nullptr)
    {
        const std::uint32_t at = b->committed.load(std::memory_order_relaxed);
        if (at + need <= kBufferBytes)
        {
            out = b;
            return b->data + at;
        }
        b->retired.store(1, std::memory_order_release);
        if (g_wake != nullptr) { SetEvent(g_wake); }
    }
    b = NewBuffer();
    t_buffer = b;
    if (b == nullptr) { return nullptr; }
    std::uint8_t* p = b->data;
    *p++ = kEvReset;
    p = PutVar(p, GetCurrentThreadId());
    b->committed.store(static_cast<std::uint32_t>(p - b->data), std::memory_order_release);
    out = b;
    return p;
}

inline void Publish(Buffer* b, const std::uint8_t* end) noexcept
{
    b->committed.store(static_cast<std::uint32_t>(end - b->data), std::memory_order_release);
}

inline std::uint8_t* PutSerial(std::uint8_t* p, Buffer* b, std::uint64_t serial) noexcept
{
    p = PutVar(p, serial - b->prevSerial);
    b->prevSerial = serial;
    return p;
}

inline std::uint8_t* PutKey(std::uint8_t* p, Buffer* b, std::uint64_t key) noexcept
{
    p = PutVar(p, ZigZag(static_cast<std::int64_t>(key - b->prevKey)));
    b->prevKey = key;
    return p;
}

// UE's time marker every 4096 events (MemoryTrace_UpdateInternal), carrying the serial of the event
// it precedes.
inline std::uint8_t* MaybeMarker(std::uint8_t* p, Buffer* b, std::uint64_t serial) noexcept
{
    if ((serial & kMarkerPeriod) != 0) { return p; }
    *p++ = kEvMarker;
    p = PutSerial(p, b, serial);
    p = PutVar(p, Qpc());
    return PutVar(p, 0);
}

// ---- Stacks: hash -> id, and the distinct return addresses for the symbol table ----
struct U64Table
{
    std::uint64_t* keys;     // 0 = empty
    std::uint32_t* values;
    std::uint32_t capacity;  // a power of two
    std::uint32_t count;
};
constinit SRWLOCK g_stackLock = SRWLOCK_INIT;
constinit U64Table g_stacks = {};
constinit U64Table g_addrs = {};
constinit std::uint32_t g_stackCount = 0;

inline std::uint32_t Slot(std::uint64_t key, std::uint32_t capacity) noexcept
{
    return static_cast<std::uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 32) & (capacity - 1);
}

std::uint32_t TableFind(const U64Table& t, std::uint64_t key) noexcept
{
    if (t.capacity == 0) { return 0; }
    for (std::uint32_t i = Slot(key, t.capacity);; i = (i + 1) & (t.capacity - 1))
    {
        if (t.keys[i] == key) { return t.values != nullptr ? t.values[i] : 1u; }
        if (t.keys[i] == 0) { return 0; }
    }
}

bool TableInsert(U64Table& t, std::uint64_t key, std::uint32_t value, bool withValues) noexcept
{
    if ((t.count + 1) * 2 > t.capacity)
    {
        const std::uint32_t capacity = t.capacity == 0 ? 4096u : t.capacity * 2;
        const std::size_t bytes = static_cast<std::size_t>(capacity) * (sizeof(std::uint64_t) + (withValues ? sizeof(std::uint32_t) : 0));
        void* block = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (block == nullptr) { return false; }
        auto* keys = static_cast<std::uint64_t*>(block);
        auto* values = withValues ? reinterpret_cast<std::uint32_t*>(keys + capacity) : nullptr;
        for (std::uint32_t i = 0; i < t.capacity; ++i)
        {
            if (t.keys[i] == 0) { continue; }
            std::uint32_t j = Slot(t.keys[i], capacity);
            while (keys[j] != 0) { j = (j + 1) & (capacity - 1); }
            keys[j] = t.keys[i];
            if (values != nullptr) { values[j] = t.values[i]; }
        }
        if (t.keys != nullptr) { VirtualFree(t.keys, 0, MEM_RELEASE); }
        t.keys = keys;
        t.values = values;
        t.capacity = capacity;
    }
    std::uint32_t i = Slot(key, t.capacity);
    while (t.keys[i] != 0 && t.keys[i] != key) { i = (i + 1) & (t.capacity - 1); }
    if (t.keys[i] == 0) { ++t.count; }
    t.keys[i] = key;
    if (t.values != nullptr) { t.values[i] = value; }
    return true;
}

std::uint32_t CaptureStack(unsigned long skip) noexcept
{
    void* frames[kMaxFrames];
    const unsigned short n = RtlCaptureStackBackTrace(skip, kMaxFrames, frames, nullptr);
    if (n == 0) { return 0; }
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned short i = 0; i < n; ++i) { h = (h ^ reinterpret_cast<std::uintptr_t>(frames[i])) * 0x100000001b3ull; }
    h ^= n;
    if (h == 0) { h = 1; }

    AcquireSRWLockShared(&g_stackLock);
    std::uint32_t id = TableFind(g_stacks, h);
    ReleaseSRWLockShared(&g_stackLock);
    if (id != 0) { return id; }

    AcquireSRWLockExclusive(&g_stackLock);
    id = TableFind(g_stacks, h);
    if (id == 0 && TableInsert(g_stacks, h, g_stackCount + 1, true))
    {
        id = ++g_stackCount;
        for (unsigned short i = 0; i < n; ++i) { TableInsert(g_addrs, reinterpret_cast<std::uintptr_t>(frames[i]), 0, false); }
        // The definition goes out once, in the stream of the thread that met it first; a use on
        // another thread may come earlier in serial order -- the reader resolves ids at the end.
        Buffer* b = nullptr;
        if (std::uint8_t* p = Reserve(b, 16u + kMaxFrames * 10u))
        {
            *p++ = kEvStack;
            p = PutVar(p, id);
            *p++ = static_cast<std::uint8_t>(n);
            std::uint64_t prev = 0;
            for (unsigned short i = 0; i < n; ++i)
            {
                const std::uint64_t a = reinterpret_cast<std::uintptr_t>(frames[i]);
                p = PutVar(p, ZigZag(static_cast<std::int64_t>(a - prev)));
                prev = a;
            }
            Publish(b, p);
        }
        else
        {
            g_drops.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ReleaseSRWLockExclusive(&g_stackLock);
    return id;
}

// ---- The file (the writer thread's while it runs; the main thread's before and after) ----
HANDLE g_file = INVALID_HANDLE_VALUE;
std::uint8_t* g_stage = nullptr;
std::uint32_t g_stageUsed = 0;
std::uint64_t g_fileBytes = 0;
bool g_fileFailed = false;

void FileFlush()
{
    if (g_stageUsed == 0 || g_file == INVALID_HANDLE_VALUE) { g_stageUsed = 0; return; }
    DWORD written = 0;
    if (!WriteFile(g_file, g_stage, g_stageUsed, &written, nullptr) || written != g_stageUsed) { g_fileFailed = true; }
    g_fileBytes += written;
    g_stageUsed = 0;
}

void FileWrite(const void* data, std::size_t n)
{
    const auto* src = static_cast<const std::uint8_t*>(data);
    while (n > 0)
    {
        if (g_stageUsed == kStageBytes) { FileFlush(); }
        const std::size_t take = std::min<std::size_t>(n, kStageBytes - g_stageUsed);
        std::memcpy(g_stage + g_stageUsed, src, take);
        g_stageUsed += static_cast<std::uint32_t>(take);
        src += take;
        n -= take;
    }
}

void FileRecordHeader(std::uint8_t type, std::uint32_t length)
{
    std::uint8_t h[5] = { type, static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8),
                          static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 24) };
    FileWrite(h, sizeof(h));
}

// A payload builder for the records written outside the trace (header, tables, end): those may allocate.
struct Bytes
{
    std::vector<std::uint8_t> v;
    void U8(std::uint8_t x) { v.push_back(x); }
    void U16(std::uint16_t x) { Raw(&x, sizeof(x)); }
    void U32(std::uint32_t x) { Raw(&x, sizeof(x)); }
    void U64(std::uint64_t x) { Raw(&x, sizeof(x)); }
    void Raw(const void* p, std::size_t n) { v.insert(v.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n); }
    void Str16(const std::string& s)
    {
        const std::size_t n = std::min<std::size_t>(s.size(), 0xFFFFu);
        U16(static_cast<std::uint16_t>(n));
        Raw(s.data(), n);
    }
    void Write(std::uint8_t type) const
    {
        FileRecordHeader(type, static_cast<std::uint32_t>(v.size()));
        FileWrite(v.data(), v.size());
    }
};

std::string Utf8(const wchar_t* w)
{
    if (w == nullptr || w[0] == L'\0') { return {}; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return {}; }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Drains every buffer's published bytes into chunks; recycles retired buffers once empty.
void Drain()
{
    const std::uint32_t count = std::min(g_bufferCount.load(std::memory_order_acquire), kMaxBuffers);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        Buffer* b = g_buffers[i].load(std::memory_order_acquire);
        if (b == nullptr) { continue; }
        const bool retired = b->retired.load(std::memory_order_acquire) != 0; // before `committed`: then it is final
        const std::uint32_t end = b->committed.load(std::memory_order_acquire);
        if (end > b->readPos)
        {
            const std::uint32_t n = end - b->readPos;
            FileRecordHeader(kRecChunk, n + 4u);
            FileWrite(&b->id, sizeof(b->id));
            FileWrite(b->data + b->readPos, n);
            b->readPos = end;
        }
        if (retired && b->readPos == end)
        {
            b->committed.store(0, std::memory_order_relaxed);
            b->readPos = 0;
            b->retired.store(0, std::memory_order_relaxed);
            AcquireSRWLockExclusive(&g_freeLock); // orders the reset before the next owner's writes
            g_freeList[g_freeCount++] = i;
            ReleaseSRWLockExclusive(&g_freeLock);
        }
    }
    FileFlush();
}

DWORD WINAPI WriterMain(void*)
{
    SetThreadDescription(GetCurrentThread(), L"MemTrace writer");
    for (;;)
    {
        WaitForSingleObject(g_wake, 10);
        const bool stop = g_writerStop.load(std::memory_order_acquire);
        Drain();
        if (stop) { break; }
    }
    return 0;
}

// ---- Records ----
void WriteHeader()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Bytes b;
    b.Raw("MTRC", 4);
    b.U32(kVersion);
    b.U64(static_cast<std::uint64_t>(f.QuadPart));
    b.U64(g_startQpc);
    b.U32(GetCurrentProcessId());
    b.U8(g_opts.cpu ? 1 : 0);
    b.U8(g_opts.gpu ? 1 : 0);
    b.U8(static_cast<std::uint8_t>(g_opts.stacks));
    b.U8(0);
    b.U32(g_opts.sampleEvery);
    b.U64(g_startFrameActual);
    b.U32(static_cast<std::uint32_t>(kMarkerPeriod + 1));
    b.U32(kMemTagCount);
    b.Str16(Utf8(exe));
    b.Write(kRecHeader);
}

void WriteTags()
{
    Bytes b;
    b.U32(kMemTagCount);
    for (std::uint32_t t = 0; t < kMemTagCount; ++t)
    {
        b.U8(static_cast<std::uint8_t>(kMemTagParents[t]));
        b.Str16(kMemTagNames[t]);
    }
    b.Write(kRecTags);
}

void WriteHeaps()
{
    static const char* const kNames[] = { "cpu", "gpu.local", "gpu.nonlocal" };
    Bytes b;
    b.U32(3);
    for (std::uint8_t h = 0; h < 3; ++h)
    {
        b.U8(h);
        b.Str16(kNames[h]);
    }
    b.Write(kRecHeaps);
}

// Every loaded module with its PDB identity (WindowsModuleDiagnostics.cpp:164-203), so a trace whose
// process died without a symbol table can still be symbolised offline.
void WriteModules()
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) { return; }
    const std::uint32_t count = std::min<std::uint32_t>(needed / sizeof(HMODULE), 1024u);
    Bytes b;
    b.U32(count);
    for (std::uint32_t m = 0; m < count; ++m)
    {
        MODULEINFO info{};
        K32GetModuleInformation(GetCurrentProcess(), mods[m], &info, sizeof(info));
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mods[m], path, MAX_PATH);
        std::uint8_t guid[16] = {};
        std::uint32_t age = 0;
        const auto base = reinterpret_cast<const std::uint8_t*>(info.lpBaseOfDll);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
        for (std::uint32_t i = 0, n = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); i < n; ++i)
        {
            struct CodeView7 { std::uint32_t signature; std::uint8_t guid[16]; std::uint32_t age; };
            if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[i].SizeOfData < sizeof(CodeView7)) { continue; }
            const auto* cv = reinterpret_cast<const CodeView7*>(base + entries[i].AddressOfRawData);
            if (cv->signature != 0x53445352u) { continue; } // 'RSDS'
            std::memcpy(guid, cv->guid, sizeof(guid));
            age = cv->age;
            break;
        }
        b.U64(reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll));
        b.U32(info.SizeOfImage);
        b.Raw(guid, sizeof(guid));
        b.U32(age);
        b.Str16(Utf8(path));
    }
    b.Write(kRecModules);
}

void WriteEnd(const mem::CpuTraceTotals& cpu, bool cpuOn, const GpuEndTotals& gpu, bool gpuOn, std::uint64_t lastSerial)
{
    Bytes b;
    b.U64(g_drops.load(std::memory_order_relaxed));
    b.U64(lastSerial);
    b.U32(g_stackCount);
    b.U32(g_bufferCount.load(std::memory_order_relaxed));
    b.U8(cpuOn ? 1 : 0);
    b.U8(gpuOn ? 1 : 0);
    b.U32(kMemTagCount);
    for (std::uint32_t t = 0; t < kMemTagCount; ++t)
    {
        b.U64(static_cast<std::uint64_t>(cpu.bytes[t]));
        b.U64(static_cast<std::uint64_t>(cpu.count[t]));
        b.U64(gpu.local[t]);
        b.U64(gpu.nonLocal[t]);
        b.U64(gpu.descriptor[t]);
        b.U32(gpu.resources[t]);
        b.U32(gpu.heaps[t]);
    }
    b.Write(kRecEnd);
}

// Symbols for every distinct return address, resolved here while the modules are loaded (dbghelp,
// deferred loads: a module's PDB is read on its first address). Returns how many resolved.
std::uint32_t WriteSymbols()
{
    std::vector<std::uint64_t> addrs;
    AcquireSRWLockShared(&g_stackLock);
    addrs.reserve(g_addrs.count);
    for (std::uint32_t i = 0; i < g_addrs.capacity; ++i)
    {
        if (g_addrs.keys[i] != 0) { addrs.push_back(g_addrs.keys[i]); }
    }
    ReleaseSRWLockShared(&g_stackLock);
    std::sort(addrs.begin(), addrs.end());

    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const bool initialised = SymInitializeW(process, nullptr, TRUE) != FALSE; // false: someone already did -- use theirs
    std::uint32_t resolved = 0;
    Bytes b;
    b.U32(static_cast<std::uint32_t>(addrs.size()));
    alignas(SYMBOL_INFOW) std::uint8_t symBuf[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)];
    for (const std::uint64_t a : addrs)
    {
        auto* sym = reinterpret_cast<SYMBOL_INFOW*>(symBuf);
        std::memset(sym, 0, sizeof(SYMBOL_INFOW));
        sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
        sym->MaxNameLen = 512;
        DWORD64 disp = 0;
        std::string func;
        if (SymFromAddrW(process, a, &disp, sym)) { func = Utf8(sym->Name); ++resolved; }
        IMAGEHLP_LINEW64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        std::string file;
        std::uint32_t lineNo = 0;
        if (SymGetLineFromAddrW64(process, a, &lineDisp, &line))
        {
            file = Utf8(line.FileName);
            lineNo = line.LineNumber;
        }
        b.U64(a);
        b.Str16(func);
        b.Str16(file);
        b.U32(lineNo);
    }
    if (initialised) { SymCleanup(process); }
    b.Write(kRecSymbols);
    return resolved;
}

void RotateTraces()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (const fs::directory_entry& e : fs::directory_iterator("traces", ec))
    {
        const std::string name = e.path().filename().string();
        if (name.rfind("memtrace_", 0) == 0 && e.path().extension() == ".mtrace")
        {
            files.emplace_back(e.last_write_time(ec), e.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (std::size_t i = 0; i + kKeepTraces <= files.size(); ++i) { fs::remove(files[i].second, ec); } // room for the new one
}

const char* StackModeName(StackMode m)
{
    switch (m)
    {
    case StackMode::Off: return "off";
    case StackMode::Big: return "big";
    case StackMode::All: return "all";
    case StackMode::Sampled: return "1/"; // + sampleEvery
    }
    return "?";
}

void EmitMarker(std::uint64_t frame) noexcept
{
    Buffer* b = nullptr;
    std::uint8_t* p = Reserve(b, 32u);
    if (p == nullptr) { g_drops.fetch_add(1, std::memory_order_relaxed); return; }
    const std::uint64_t serial = g_serial.fetch_add(1, std::memory_order_relaxed);
    *p++ = kEvMarker;
    p = PutSerial(p, b, serial);
    p = PutVar(p, Qpc());
    p = PutVar(p, frame + 1);
    Publish(b, p);
}

void Start(std::uint64_t frame)
{
    if (g_started) { return; }
    g_started = true;
    g_startFrameActual = frame;

    std::error_code ec;
    std::filesystem::create_directory("traces", ec);
    RotateTraces();
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char name[96];
    std::snprintf(name, sizeof(name), "traces/memtrace_%04d%02d%02d_%02d%02d%02d_%lu.mtrace", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, GetCurrentProcessId());
    g_path = name;
    g_file = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_stage = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kStageBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (g_file == INVALID_HANDLE_VALUE || g_stage == nullptr)
    {
        LOG_ERROR(logging::LogCategory::Core, "memtrace: cannot open {} -- no trace", g_path);
        g_stopped = true;
        return;
    }
    g_startQpc = Qpc();
    WriteHeader();
    WriteTags();
    WriteHeaps();
    WriteModules();
    FileFlush();

    g_stackMode = g_opts.stacks;
    g_sampleEvery = std::max<std::uint32_t>(g_opts.sampleEvery, 1u);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_writer = CreateThread(nullptr, 0, &WriterMain, nullptr, 0, nullptr);
    g_markersOn.store(true, std::memory_order_release);

    // Each side turns its events on and sends what is alive now under its own lock(s): from then on
    // nothing can be missed or counted twice.
    bool gpuOn = false, cpuOn = false;
    if (g_opts.gpu)
    {
        if (GpuHook hook = g_gpuHook.load(std::memory_order_acquire))
        {
            g_gpuOn.store(true, std::memory_order_release);
            hook(true, nullptr);
            gpuOn = true;
        }
    }
    if (g_opts.cpu)
    {
        if (mem::MemTrackEnabled())
        {
            g_cpuOn.store(true, std::memory_order_release);
            mem::BeginCpuTrace();
            cpuOn = true;
        }
        else
        {
            LOG_WARNING(logging::LogCategory::Core, "memtrace: no CPU events -- the pointer map was not on from the first allocation");
        }
    }
    LOG_INFO(logging::LogCategory::Core, "memtrace: started at frame {} -> {} (cpu {}, gpu {}, stacks {}{})", frame, g_path,
             cpuOn ? "on" : "off", gpuOn ? "on" : "off", StackModeName(g_opts.stacks),
             g_opts.stacks == StackMode::Sampled ? std::to_string(g_opts.sampleEvery) : std::string());
}
} // namespace

bool ParseSwitch(const char* cmdLine, Options& out)
{
    if (cmdLine == nullptr) { return false; }
    const char* flag = std::strstr(cmdLine, "--memtrace");
    if (flag == nullptr) { return false; }
    const char* p = flag + std::strlen("--memtrace");
    if (*p != '\0' && *p != '=' && *p != ' ' && *p != '\t') { return false; } // another switch that starts the same
    out = Options{};
    if (*p != '=') { return true; }
    ++p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
    {
        const char* end = p;
        while (*end != '\0' && *end != ',' && *end != ' ' && *end != '\t') { ++end; }
        const std::string token(p, end);
        if (token == "all") { out.cpu = true; out.gpu = true; }
        else if (token == "cpu") { out.cpu = true; out.gpu = false; }
        else if (token == "gpu") { out.cpu = false; out.gpu = true; }
        else if (token == "stacks=off") { out.stacks = StackMode::Off; }
        else if (token == "stacks=big") { out.stacks = StackMode::Big; }
        else if (token == "stacks=all") { out.stacks = StackMode::All; }
        else if (token.rfind("stacks=1/", 0) == 0)
        {
            out.stacks = StackMode::Sampled;
            out.sampleEvery = static_cast<std::uint32_t>(std::max(1, std::atoi(token.c_str() + 9)));
        }
        else if (token.rfind("start=", 0) == 0) { out.startFrame = static_cast<std::uint64_t>(std::max(0, std::atoi(token.c_str() + 6))); }
        else { LOG_WARNING(logging::LogCategory::Core, "--memtrace: unknown option '{}' (ignored)", token); }
        p = *end == ',' ? end + 1 : end;
    }
    return true;
}

bool WantsCpuRaw(const wchar_t* cmdLine) noexcept
{
    if (cmdLine == nullptr) { return false; }
    const wchar_t* flag = std::wcsstr(cmdLine, L"--memtrace");
    if (flag == nullptr) { return false; }
    const wchar_t* p = flag + 10;
    if (*p != L'=') { return *p == L'\0' || *p == L' ' || *p == L'\t'; }
    ++p;
    // The mode is the first token; only "gpu" leaves the CPU out.
    return !(std::wcsncmp(p, L"gpu", 3) == 0 && (p[3] == L'\0' || p[3] == L',' || p[3] == L' ' || p[3] == L'\t'));
}

void Configure(const Options& options)
{
    if (g_configured) { return; }
    g_opts = options;
    g_configured = true;
    if (options.startFrame == 0) { Start(0); }
}

void OnFrameEnd(std::uint64_t frame)
{
    if (!g_configured) { return; }
    if (!g_started && frame >= g_opts.startFrame) { Start(frame); }
    if (g_markersOn.load(std::memory_order_acquire)) { EmitMarker(frame); }
}

void Stop()
{
    if (!g_started || g_stopped) { return; }
    g_stopped = true;
    const auto t0 = std::chrono::steady_clock::now();
    // The process's own count, taken where each side turns its events off -- so it is exactly what
    // the stream leaves alive.
    GpuEndTotals gpu;
    mem::CpuTraceTotals cpu;
    const bool gpuOn = g_gpuOn.load(std::memory_order_acquire);
    const bool cpuOn = g_cpuOn.load(std::memory_order_acquire);
    if (gpuOn)
    {
        g_gpuHook.load(std::memory_order_acquire)(false, &gpu);
        g_gpuOn.store(false, std::memory_order_release);
    }
    if (cpuOn)
    {
        mem::EndCpuTrace(cpu);
        g_cpuOn.store(false, std::memory_order_release);
    }
    g_markersOn.store(false, std::memory_order_release);
    const std::uint64_t lastSerial = g_serial.load(std::memory_order_relaxed);

    g_writerStop.store(true, std::memory_order_release);
    SetEvent(g_wake);
    WaitForSingleObject(g_writer, INFINITE);
    CloseHandle(g_writer);
    g_writer = nullptr;

    WriteModules(); // again: DLLs loaded since the start (NGX, dxcompiler) are in this one
    WriteEnd(cpu, cpuOn, gpu, gpuOn, lastSerial);
    const std::uint32_t symbols = WriteSymbols();
    FileFlush();
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::int64_t cpuBytes = 0, cpuCount = 0;
    std::uint64_t gpuBytes = 0;
    for (std::uint32_t t = 0; t < kMemTagCount; ++t)
    {
        cpuBytes += cpu.bytes[t];
        cpuCount += cpu.count[t];
        gpuBytes += gpu.local[t] + gpu.nonLocal[t];
    }
    LOG_INFO(logging::LogCategory::Core,
             "memtrace: stopped -> {}: {:.1f} MB, {} serials, {} stacks ({} symbols), {} buffers, drops {}{} | alive cpu {:.1f} MB in {}, gpu {:.1f} MB | close {:.0f} ms",
             g_path, static_cast<double>(g_fileBytes) / (1024.0 * 1024.0), lastSerial - 1, g_stackCount, symbols,
             g_bufferCount.load(std::memory_order_relaxed), g_drops.load(std::memory_order_relaxed),
             g_fileFailed ? " WRITE FAILED" : "", static_cast<double>(cpuBytes) / (1024.0 * 1024.0), cpuCount,
             static_cast<double>(gpuBytes) / (1024.0 * 1024.0), ms);
}

void EmitAlloc(std::uint64_t key, std::uint64_t size, std::uint8_t tag, std::uint8_t heap, std::uint8_t kind,
               std::uint32_t stack, const char* name) noexcept
{
    const std::uint32_t nameLen = (heap != kHeapCpu && name != nullptr) ? static_cast<std::uint32_t>(strnlen(name, kNameMax)) : 0u;
    Buffer* b = nullptr;
    std::uint8_t* p = Reserve(b, 96u + nameLen);
    if (p == nullptr) { g_drops.fetch_add(1, std::memory_order_relaxed); return; }
    const std::uint64_t serial = g_serial.fetch_add(1, std::memory_order_relaxed);
    p = MaybeMarker(p, b, serial);
    *p++ = kEvAlloc;
    p = PutSerial(p, b, serial);
    p = PutKey(p, b, key);
    p = PutVar(p, size);
    *p++ = tag;
    *p++ = heap;
    *p++ = kind;
    p = PutVar(p, stack);
    if (heap != kHeapCpu)
    {
        p = PutVar(p, nameLen);
        std::memcpy(p, name, nameLen);
        p += nameLen;
    }
    Publish(b, p);
}

void EmitFree(std::uint64_t key, std::uint8_t heap) noexcept
{
    Buffer* b = nullptr;
    std::uint8_t* p = Reserve(b, 64u);
    if (p == nullptr) { g_drops.fetch_add(1, std::memory_order_relaxed); return; }
    const std::uint64_t serial = g_serial.fetch_add(1, std::memory_order_relaxed);
    p = MaybeMarker(p, b, serial);
    *p++ = kEvFree;
    p = PutSerial(p, b, serial);
    p = PutKey(p, b, key);
    *p++ = heap;
    Publish(b, p);
}

std::uint32_t CaptureCpuStack(std::uint64_t size) noexcept
{
    if (!g_cpuOn.load(std::memory_order_acquire)) { return 0; }
    switch (g_stackMode)
    {
    case StackMode::Off: return 0;
    case StackMode::Big: if (size < kBigAlloc) { return 0; } break;
    case StackMode::All: break;
    case StackMode::Sampled:
        if (++t_sampleCounter < g_sampleEvery) { return 0; }
        t_sampleCounter = 0;
        break;
    }
    return CaptureStack(3); // this, MemNew's TrackAlloc, CountAlloc (inlining permitting)
}

std::uint32_t CaptureGpuStack() noexcept
{
    if (!g_gpuOn.load(std::memory_order_acquire) || g_stackMode == StackMode::Off) { return 0; }
    return CaptureStack(2);
}

void SetGpuHook(GpuHook hook) noexcept
{
    g_gpuHook.store(hook, std::memory_order_release);
}
} // namespace memtrace
