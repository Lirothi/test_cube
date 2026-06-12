#include "rendering/core/RendererInvariantFailure.h"

#include <windows.h>

#include <cstdlib>

[[noreturn]] void RendererInvariantFailure(const char* msg)
{
    OutputDebugStringA("RENDERER INVARIANT FAILURE: ");
    OutputDebugStringA(msg != nullptr ? msg : "(null)");
    OutputDebugStringA("\n");
    std::abort();
}
