#include "rendering/core/RendererInvariantFailure.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "core/diagnostics/DiagPaths.h"
#include "core/logging/Log.h"

[[noreturn]] void RendererInvariantFailure(const char* msg)
{
    // To a file FIRST. This is the last thing the process does, and DBWIN output is lost without
    // a debugger attached — so on a headless run (the stress harness, a CI-style invocation, a
    // --shot capture) the one message explaining WHY the process died went nowhere. The engine has
    // already learned this for the device caps, the stress verdict and the barrier trace; an
    // invariant failure is the one message that most needs to survive. (Artifact kept until L7.)
    FILE* f = nullptr;
    if (fopen_s(&f, diag::LogPath("invariant_failure.log").c_str(), "a") == 0 && f) {
        std::fputs("RENDERER INVARIANT FAILURE: ", f);
        std::fputs(msg != nullptr ? msg : "(null)", f);
        std::fputc('\n', f);
        std::fclose(f);
    }
    // Then the one central Fatal record: DBWIN immediately plus an unbuffered append to the
    // session log, with no dependency on the writer thread (logging plan L4). Stack buffer only.
    char line[1024];
    std::snprintf(line, sizeof(line), "RENDERER INVARIANT FAILURE: %s (also in logs/invariant_failure.log)",
                  msg != nullptr ? msg : "(null)");
    logging::EmergencyWrite(logging::LogLevel::Fatal, logging::LogCategory::Render, line);
    std::abort();
}
