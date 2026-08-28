#include "rendering/core/RendererInvariantFailure.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "core/diagnostics/DiagPaths.h"

[[noreturn]] void RendererInvariantFailure(const char* msg)
{
    OutputDebugStringA("RENDERER INVARIANT FAILURE: ");
    OutputDebugStringA(msg != nullptr ? msg : "(null)");
    OutputDebugStringA("\n");
    // ALSO to a file. This is the last thing the process does, and DBWIN output is lost without a
    // debugger attached — so on a headless run (the stress harness, a CI-style invocation, a
    // --shot capture) the one message explaining WHY the process died went nowhere. The engine has
    // already learned this for the device caps, the stress verdict and the barrier trace; an
    // invariant failure is the one message that most needs to survive.
    FILE* f = nullptr;
    if (fopen_s(&f, diag::LogPath("invariant_failure.log").c_str(), "a") == 0 && f) {
        std::fputs("RENDERER INVARIANT FAILURE: ", f);
        std::fputs(msg != nullptr ? msg : "(null)", f);
        std::fputc('\n', f);
        std::fclose(f);
    }
    std::abort();
}
