#include <windows.h>
#include <mimalloc.h>
#pragma warning(push)
#pragma warning(disable: 28251)
#include "mimalloc-new-delete.h"
#pragma warning(pop)
#include "App.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

int ForceMi() { return mi_version(); }

// Entry point
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nShowCmd
)
{
    App app;
    app.Run(hInstance, nShowCmd);
    return 0;
}
