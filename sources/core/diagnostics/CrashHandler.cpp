// Crash reports -- see CrashHandler.h. docs/distribution_build_plan.md §7.

#include "core/diagnostics/CrashHandler.h"

#include "core/logging/Log.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#pragma comment(lib, "dbghelp.lib")

namespace crash
{
namespace
{
    Options g_options;
    std::filesystem::path g_folder;              // absolute, made at Install
    std::atomic<bool> g_installed{ false };
    std::atomic<bool> g_reporting{ false };      // the first crash reports; any other thread that crashes meanwhile waits
    std::atomic<DWORD> g_reportingThread{ 0 };   // ... and a fault inside the report on that thread ends the process
    HANDLE g_request = nullptr;
    HANDLE g_done = nullptr;
    EXCEPTION_POINTERS* g_pointers = nullptr;
    DWORD g_threadId = 0;
    wchar_t g_dumpPath[1024] = {};
    bool g_dumpWritten = false;
    std::atomic<unsigned> g_deviceRemovedReason{ 0 };

    // Made at Install and waiting: a thread out of stack cannot call MiniDumpWriteDump itself, and a dump of the process is
    // best written from outside the thread that broke.
    DWORD WINAPI DumpWriter(void*)
    {
        WaitForSingleObject(g_request, INFINITE);
        SYSTEMTIME t{};
        GetLocalTime(&t);
        swprintf_s(g_dumpPath, L"%s\\%04u%02u%02u_%02u%02u%02u_%lu.dmp", g_folder.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour,
                   t.wMinute, t.wSecond, GetCurrentProcessId());
        const HANDLE file = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION info{};
            info.ThreadId = g_threadId;
            info.ExceptionPointers = g_pointers;
            info.ClientPointers = FALSE;
            const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                                                  MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
            g_dumpWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                              g_pointers ? &info : nullptr, nullptr, nullptr) != FALSE;
            CloseHandle(file);
        }
        SetEvent(g_done);
        return 0;
    }

    std::string ModuleAndOffset(const void* address)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(address), &module) || !module)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "0x%p", address);
            return text;
        }
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        char narrow[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        char text[MAX_PATH + 32];
        std::snprintf(text, sizeof(text), "%s+0x%llx", narrow,
                      static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
        return text;
    }

    std::string Describe(const EXCEPTION_POINTERS* pointers)
    {
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        char text[256];
        switch (record.ExceptionCode)
        {
        case EXCEPTION_ACCESS_VIOLATION:
            std::snprintf(text, sizeof(text), "access violation %s 0x%llx",
                          record.NumberParameters > 0 && record.ExceptionInformation[0] == 1 ? "writing" :
                          record.NumberParameters > 0 && record.ExceptionInformation[0] == 8 ? "executing" : "reading",
                          static_cast<unsigned long long>(record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0));
            break;
        case EXCEPTION_STACK_OVERFLOW: std::snprintf(text, sizeof(text), "stack overflow"); break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: std::snprintf(text, sizeof(text), "illegal instruction"); break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: std::snprintf(text, sizeof(text), "integer division by zero"); break;
        case EXCEPTION_IN_PAGE_ERROR: std::snprintf(text, sizeof(text), "in-page error (a mapped file went away)"); break;
        case 0xE06D7363u: std::snprintf(text, sizeof(text), "an uncaught C++ exception"); break; // MSVC's throw
        default: std::snprintf(text, sizeof(text), "exception 0x%08lx", record.ExceptionCode); break;
        }
        return std::string(text) + " at " + ModuleAndOffset(record.ExceptionAddress);
    }

    void ShowBox(const std::wstring& text, unsigned timeoutMs)
    {
        const std::wstring title = g_options.product;
        if (timeoutMs == 0)
        {
            wchar_t value[32] = {};
            if (GetEnvironmentVariableW(L"TEST_CUBE_MESSAGE_TIMEOUT_MS", value, 32) > 0) { timeoutMs = static_cast<unsigned>(_wtoi(value)); }
        }
        // MessageBoxTimeoutW: user32's own (since XP, not in the headers) -- the self-test's box closes itself.
        using TimeoutBox = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT, WORD, DWORD);
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        const auto timeoutBox = user32 ? reinterpret_cast<TimeoutBox>(GetProcAddress(user32, "MessageBoxTimeoutW")) : nullptr;
        const UINT style = MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST;
        if (timeoutMs != 0 && timeoutBox)
        {
            timeoutBox(nullptr, text.c_str(), title.c_str(), style, 0, timeoutMs);
        }
        else
        {
            MessageBoxW(nullptr, text.c_str(), title.c_str(), style);
        }
    }

    std::wstring Widen(const std::string& text)
    {
        if (text.empty()) { return {}; }
        const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
        return out;
    }

    // `what` null = described from `pointers`, after the dump. Nothing allocates, formats or calls into a module before the
    // dump is asked for: a crash inside the heap, or a thread out of stack (only the main thread has a stack guarantee),
    // faulted again in that work and the process ended with no dump. The writer thread names the file and writes it.
    [[noreturn]] void Report(const char* what, EXCEPTION_POINTERS* pointers, unsigned exitCode)
    {
        const DWORD self = GetCurrentThreadId();
        if (g_reporting.exchange(true))
        {
            // A fault inside this report on the reporting thread (the log's flush, the message box's modal loop running
            // the window procedure over the broken frame): end at once -- sleeping there hung the process for good.
            if (g_reportingThread.load() == self) { TerminateProcess(GetCurrentProcess(), exitCode); }
            for (;;) { Sleep(INFINITE); } // another thread is reporting: its report ends the process
        }
        g_reportingThread.store(self);
        g_pointers = pointers;
        g_threadId = self;
        bool dumped = false;
        if (g_request && g_done)
        {
            SetEvent(g_request);
            dumped = WaitForSingleObject(g_done, 60000) == WAIT_OBJECT_0 && g_dumpWritten;
        }
        const std::string described = what ? std::string(what) : pointers ? Describe(pointers) : std::string("a crash");
        char dumpUtf8[1024] = {};
        WideCharToMultiByte(CP_UTF8, 0, g_dumpPath, -1, dumpUtf8, sizeof(dumpUtf8) - 1, nullptr, nullptr);
        char logPath[1024] = {};
        logging::GetSessionFilePath(logPath, sizeof(logPath));
        LOG_FATAL(logging::LogCategory::Core, "crash: {} -- {} {}", described, dumped ? "minidump" : "NO minidump", dumpUtf8);
        logging::Flush(2000);
        if (g_options.messageBox)
        {
            wchar_t device[256] = {};
            if (const unsigned reason = g_deviceRemovedReason.load())
            {
                swprintf_s(device, L"The graphics card stopped responding (device removed, 0x%08X) -- usually its driver. "
                                   L"Update the graphics driver and start the game again.\n\n", reason);
            }
            const std::wstring text = std::wstring(device) + g_options.product + L" stopped unexpectedly: " + Widen(described) + L".\n\n" +
                                      (dumped ? L"A crash report was saved:\n" + std::wstring(g_dumpPath) + L"\n\n" : std::wstring()) +
                                      L"The log:\n" + Widen(logPath);
            LOG_INFO(logging::LogCategory::Core, "crash: message box shown");
            logging::Flush(1000);
            ShowBox(text, g_options.messageBoxTimeoutMs);
        }
        TerminateProcess(GetCurrentProcess(), exitCode);
        for (;;) { Sleep(INFINITE); }
    }

    LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* pointers)
    {
        Report(nullptr, pointers, pointers->ExceptionRecord->ExceptionCode);
    }

    // A stack overflow is reported where it happens, first chance: a handler further up might swallow it, and the thread
    // has little stack left for anything later.
    LONG WINAPI StackOverflowHandler(EXCEPTION_POINTERS* pointers)
    {
        if (pointers->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
        {
            Report(nullptr, pointers, pointers->ExceptionRecord->ExceptionCode);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void OnTerminate()
    {
        std::string what = "std::terminate";
        try
        {
            if (const std::exception_ptr current = std::current_exception()) { std::rethrow_exception(current); }
        }
        catch (const std::exception& e)
        {
            what += " (uncaught exception: " + std::string(e.what()) + ")";
        }
        catch (...)
        {
            what += " (uncaught exception of an unknown type)";
        }
        Report(what.c_str(), nullptr, 3);
    }

    void OnAbort(int) { Report("abort()", nullptr, 3); }
    void OnPureCall() { Report("pure virtual function call", nullptr, 3); }
    void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
    {
        Report("invalid parameter passed to a C runtime function", nullptr, 3);
    }

    // --crash-selftest's ways to die, each out of the optimiser's reach.
    __declspec(noinline) int Recurse(volatile int depth)
    {
        volatile char pad[4096];
        pad[0] = static_cast<char>(depth);
        if (depth < 0) { return pad[0]; } // never: keeps the compiler from calling it endless
        return Recurse(depth + 1) + pad[0];
    }

    struct PureBase
    {
        PureBase() { CallPure(this); }
        static __declspec(noinline) void CallPure(PureBase* self) { self->Pure(); }
        virtual void Pure() = 0;
        virtual ~PureBase() = default;
    };
    struct PureDerived final : PureBase
    {
        void Pure() override {}
    };
}

void Install(const Options& options)
{
    if (g_installed.exchange(true)) { return; }
    g_options = options;
    std::error_code ec;
    g_folder = std::filesystem::absolute(options.dumpFolder, ec);
    std::filesystem::create_directories(g_folder, ec); // vfs-ok: crash dumps are written, never read
    g_request = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (const HANDLE writer = CreateThread(nullptr, 256 * 1024, DumpWriter, nullptr, 0, nullptr)) { CloseHandle(writer); }

    SetUnhandledExceptionFilter(UnhandledFilter);
    AddVectoredExceptionHandler(1, StackOverflowHandler);
    std::signal(SIGABRT, OnAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // abort(): our handler, then no WER dialog
    _set_purecall_handler(OnPureCall);
    _set_invalid_parameter_handler(OnInvalidParameter);
    InstallThisThread();
    LOG_INFO(logging::LogCategory::Core, "crash reports: on (minidumps in {}{})", g_folder.string(),
             options.messageBox ? ", a message box" : "");
}

bool Installed() { return g_installed.load(); }

void NoteDeviceRemoved(unsigned reason) { g_deviceRemovedReason.store(reason == 0 ? 1u : reason); }

void InstallThisThread()
{
    if (!g_installed.load()) { return; }
    std::set_terminate(OnTerminate);
    ULONG guarantee = 64 * 1024; // room for the report on a thread that ran out of stack
    SetThreadStackGuarantee(&guarantee);
}

int SelfTest(const std::string& arm, Options options)
{
    options.messageBoxTimeoutMs = 3000;
    const char* arms[] = { "av", "stack", "abort", "terminate", "throw", "purecall", "invalidparam", "fastfail" };
    bool known = false;
    for (const char* a : arms) { known = known || arm == a; }
    if (!known)
    {
        LOG_ERROR(logging::LogCategory::Core, "--crash-selftest={}: arms are av, stack, abort, terminate, throw, purecall, "
                  "invalidparam, fastfail", arm);
        return 2;
    }
    Install(options);
    LOG_INFO(logging::LogCategory::Core, "crash self-test: {}", arm);
    logging::Flush(2000);
    if (arm == "av")
    {
        volatile int* volatile nowhere = nullptr;
        *nowhere = 1;
    }
    else if (arm == "stack")
    {
        Recurse(0);
    }
    else if (arm == "abort")
    {
        std::abort();
    }
    else if (arm == "terminate")
    {
        // On a thread that never installed our terminate handler: the CRT's ends in abort(), which SIGABRT reports.
        std::thread([] { std::terminate(); }).join();
    }
    else if (arm == "throw")
    {
        throw std::runtime_error("the crash self-test's uncaught exception");
    }
    else if (arm == "purecall")
    {
        PureDerived derived;
        (void)derived;
    }
    else if (arm == "invalidparam")
    {
        std::FILE* volatile none = nullptr;
        std::fclose(none);
    }
    else if (arm == "fastfail")
    {
        __fastfail(FAST_FAIL_FATAL_APP_EXIT); // nothing in the process runs: exit code 0xC0000409, the log ends here
    }
    LOG_ERROR(logging::LogCategory::Core, "crash self-test: {} did not crash", arm);
    return 1;
}
}
