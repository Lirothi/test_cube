#include "rendering/core/DeviceRemovalWatch.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/logging/Log.h"

using Microsoft::WRL::ComPtr;

namespace render {
namespace {

constexpr int kMaxFrames = 40;

struct ThreadStack
{
    DWORD id = 0;
    std::string name;
    void* frames[kMaxFrames] = {};
    int count = 0;
};

ComPtr<ID3D12Fence> g_fence;
HANDLE g_event = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_done = nullptr;
std::thread g_thread;

// The process's threads, opened ahead of time: a toolhelp snapshot lists every thread of the SYSTEM and takes tens of
// milliseconds, and what the threads were doing at the removal is gone by then.
std::mutex g_threadsLock;
std::vector<std::pair<DWORD, HANDLE>> g_threads;

void WalkContext(CONTEXT context, ThreadStack& out)
{
    while (out.count < kMaxFrames && context.Rip != 0)
    {
        out.frames[out.count++] = reinterpret_cast<void*>(context.Rip);
        DWORD64 imageBase = 0;
        const PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!fn)
        {
            // A leaf with no unwind data: the return address is on top of the stack.
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(reinterpret_cast<void*>(context.Rsp), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
            {
                break;
            }
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, fn, &context, &handlerData, &establisher, nullptr);
    }
}

std::string ThreadName(HANDLE thread)
{
    PWSTR wide = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide)
    {
        char narrow[64] = "";
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow), nullptr, nullptr);
        name = narrow;
        LocalFree(wide);
    }
    return name;
}

// All of them stopped first, then walked, then all running again; no heap and no log while they are stopped.
std::vector<ThreadStack> CaptureOtherThreads()
{
    std::lock_guard<std::mutex> lock(g_threadsLock);
    std::vector<ThreadStack> stacks(g_threads.size());
    std::vector<char> stopped(g_threads.size(), 0);
    const DWORD self = GetCurrentThreadId();
    for (std::size_t i = 0; i < g_threads.size(); ++i)
    {
        stopped[i] = g_threads[i].first != self && SuspendThread(g_threads[i].second) != static_cast<DWORD>(-1);
    }
    for (std::size_t i = 0; i < g_threads.size(); ++i)
    {
        stacks[i].id = g_threads[i].first;
        if (stopped[i])
        {
            CONTEXT context = {};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(g_threads[i].second, &context))
            {
                WalkContext(context, stacks[i]);
            }
        }
    }
    for (std::size_t i = 0; i < g_threads.size(); ++i)
    {
        if (stopped[i])
        {
            ResumeThread(g_threads[i].second);
        }
    }
    for (std::size_t i = 0; i < g_threads.size(); ++i)
    {
        stacks[i].name = ThreadName(g_threads[i].second);
    }
    return stacks;
}

void AppendFrame(std::string& line, void* frame)
{
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frame), &module) && module)
    {
        char full[MAX_PATH] = "";
        if (GetModuleFileNameA(module, full, MAX_PATH) != 0)
        {
            const char* slash = std::strrchr(full, '\\');
            strncpy_s(name, slash ? slash + 1 : full, _TRUNCATE);
        }
    }
    char text[MAX_PATH + 32];
    std::snprintf(text, sizeof(text), " %s+0x%llx", name,
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frame) - reinterpret_cast<std::uintptr_t>(module)));
    line += text;
}

void WatchMain(ID3D12Device* device)
{
    logging::SetCurrentThreadName("Removal Watch");
    const HANDLE handles[2] = { g_stop, g_event };
    if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
    {
        return;
    }
    // Emergency, unbuffered: a thread that notices the removal usually takes the process down within milliseconds.
    const HRESULT reason = device->GetDeviceRemovedReason();
    char head[96];
    std::snprintf(head, sizeof(head), "device removal, the moment: reason 0x%08X", static_cast<unsigned>(reason));
    logging::EmergencyWrite(logging::LogLevel::Error, logging::LogCategory::RenderRhi, head);
    bool none = false;
    {
        std::lock_guard<std::mutex> lock(g_threadsLock);
        none = g_threads.empty();
    }
    if (none)
    {
        RefreshDeviceRemovalWatch(); // a run that did not ask for the threads ahead of time: late, better than nothing
    }
    const std::vector<ThreadStack> stacks = CaptureOtherThreads();
    for (const ThreadStack& stack : stacks)
    {
        if (stack.count == 0)
        {
            continue;
        }
        std::string line = "  ";
        line += stack.name.empty() ? std::to_string(stack.id) : stack.name;
        line += ":";
        for (int i = 0; i < stack.count; ++i)
        {
            AppendFrame(line, stack.frames[i]);
        }
        logging::EmergencyWrite(logging::LogLevel::Error, logging::LogCategory::RenderRhi, line);
    }
    SetEvent(g_done);
}

// D3DKMTMarkDeviceAsError: how the runtime and the driver finish a device from the CPU side. A removal that comes this
// way is the driver's opinion of a call on that very thread, not a GPU fault, and no breadcrumb, TDR event or fence
// tells it apart: its reason reads 0x887A0005, "removed". The call goes through win32u's syscall stub, which is
// position independent: a copy of it stays callable and the stub jumps here.
using MarkDeviceAsErrorFn = LONG(APIENTRY*)(void*);
MarkDeviceAsErrorFn g_markDeviceAsError = nullptr;

LONG APIENTRY OnMarkDeviceAsError(void* args)
{
    const auto* fields = static_cast<const std::uint32_t*>(args); // D3DKMT_MARKDEVICEASERROR { hDevice, Reason }
    char head[96];
    std::snprintf(head, sizeof(head), "device marked as error, reason 0x%X (0x80000006 = the driver's), by:", fields[1]);
    std::string line = head;
    void* frames[kMaxFrames] = {};
    const USHORT count = CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
    for (USHORT i = 0; i < count; ++i)
    {
        AppendFrame(line, frames[i]);
    }
    logging::EmergencyWrite(logging::LogLevel::Error, logging::LogCategory::RenderRhi, line);
    return g_markDeviceAsError(args);
}

void HookMarkDeviceAsError()
{
    HMODULE win32u = LoadLibraryW(L"win32u.dll");
    auto* stub = win32u ? reinterpret_cast<std::uint8_t*>(GetProcAddress(win32u, "NtGdiDdDDIMarkDeviceAsError")) : nullptr;
    if (!stub || stub[0] != 0x4C || stub[1] != 0x8B || stub[2] != 0xD1 || stub[3] != 0xB8) // mov r10, rcx; mov eax, <id>
    {
        LOG_WARNING(logging::LogCategory::RenderRhi, "device removal watch: win32u's MarkDeviceAsError is not the plain stub, not hooked");
        return;
    }
    constexpr std::size_t kStubBytes = 32; // win32u lays them out 32 bytes apart
    constexpr std::size_t kJumpBytes = 14; // jmp [rip + 0]; the address
    void* copy = VirtualAlloc(nullptr, kStubBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    DWORD old = 0;
    if (!copy || !VirtualProtect(stub, kJumpBytes, PAGE_EXECUTE_READWRITE, &old))
    {
        return;
    }
    std::memcpy(copy, stub, kStubBytes);
    g_markDeviceAsError = reinterpret_cast<MarkDeviceAsErrorFn>(copy);
    const std::uint8_t jump[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
    const auto target = reinterpret_cast<std::uint64_t>(&OnMarkDeviceAsError);
    std::memcpy(stub, jump, sizeof(jump));
    std::memcpy(stub + sizeof(jump), &target, sizeof(target));
    VirtualProtect(stub, kJumpBytes, old, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, kJumpBytes);
}

} // namespace

void StartDeviceRemovalWatch(ID3D12Device* device, bool ahead)
{
    if (!device || g_thread.joinable())
    {
        return;
    }
    if (ahead)
    {
        HookMarkDeviceAsError();
    }
    g_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_event || !g_stop || !g_done || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))) ||
        FAILED(g_fence->SetEventOnCompletion(UINT64_MAX, g_event)))
    {
        LOG_WARNING(logging::LogCategory::RenderRhi, "device removal watch: not started");
        return;
    }
    g_thread = std::thread(WatchMain, device);
}

void RefreshDeviceRemovalWatch()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_threadsLock);
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != GetCurrentProcessId())
        {
            continue;
        }
        bool known = false;
        for (const auto& t : g_threads)
        {
            known = known || t.first == entry.th32ThreadID;
        }
        if (!known)
        {
            if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID))
            {
                g_threads.emplace_back(entry.th32ThreadID, thread);
            }
        }
    }
    CloseHandle(snapshot);
}

void StopDeviceRemovalWatch()
{
    if (!g_thread.joinable())
    {
        return;
    }
    SetEvent(g_stop);
    g_thread.join();
    g_fence.Reset();
    CloseHandle(g_event);
    CloseHandle(g_stop);
    CloseHandle(g_done);
    g_event = g_stop = g_done = nullptr;
    for (const auto& t : g_threads)
    {
        CloseHandle(t.second);
    }
    g_threads.clear();
}

void WaitDeviceRemovalWatch(unsigned milliseconds)
{
    if (g_done && g_event && WaitForSingleObject(g_event, milliseconds) == WAIT_OBJECT_0)
    {
        WaitForSingleObject(g_done, milliseconds);
    }
}

} // namespace render
