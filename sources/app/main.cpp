#include <windows.h>
#include <mimalloc.h>
#pragma warning(push)
#pragma warning(disable: 28251)
#include "mimalloc-new-delete.h"
#pragma warning(pop)
#include "app/App.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

int ForceMi() { return mi_version(); }


namespace
{
void EnableDpiAwareness()
{
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

    // Try to opt-in to the most precise DPI awareness available on the host OS.
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setProcessDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setProcessDpiAwarenessContext &&
            setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        {
            return;
        }
    }

    HMODULE shcore = GetModuleHandleW(L"shcore.dll");
    if (!shcore)
    {
        shcore = LoadLibraryW(L"shcore.dll");
    }
    if (shcore)
    {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto setProcessDpiAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setProcessDpiAwareness &&
            SUCCEEDED(setProcessDpiAwareness(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/)))
        {
            return;
        }
    }

    if (user32)
    {
        using SetProcessDpiAwareFn = BOOL(WINAPI*)();
        if (auto setProcessDpiAware = reinterpret_cast<SetProcessDpiAwareFn>(
                GetProcAddress(user32, "SetProcessDPIAware")))
        {
            setProcessDpiAware();
        }
    }
}
} // namespace

// Entry point
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nShowCmd
)
{
    EnableDpiAwareness();
    App app;
    app.Run(hInstance, nShowCmd);
    return 0;
}
