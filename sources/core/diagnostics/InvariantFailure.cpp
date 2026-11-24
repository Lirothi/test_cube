#include "core/diagnostics/InvariantFailure.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "core/diagnostics/ArtifactWriter.h"
#include "core/logging/Log.h"

[[noreturn]] void InvariantFailure(logging::LogCategory category, const char* msg) noexcept
{
    // To a file FIRST. This is the last thing the process does, and DBWIN output is lost without
    // a debugger attached — so on a headless run (the stress harness, a CI-style invocation, a
    // --shot capture) the one message explaining WHY the process died went nowhere. The engine has
    // already learned this for the device caps, the stress verdict and the barrier trace; an
    // invariant failure is the one message that most needs to survive. (Artifact kept until L7.)
    const std::string_view categoryName = logging::LogCategoryName(category);
    diag::WriteArtifactf("invariant_failure.log", diag::ArtifactMode::Append,
                         "INVARIANT FAILURE [%.*s]: %s\n", static_cast<int>(categoryName.size()), categoryName.data(),
                         msg != nullptr ? msg : "(null)");
    // Then the one central Fatal record: DBWIN immediately plus an unbuffered append to the
    // session log, with no dependency on the writer thread (logging plan L4). Stack buffer only.
    char line[1024];
    std::snprintf(line, sizeof(line), "INVARIANT FAILURE: %s (also in logs/invariant_failure.log)",
                  msg != nullptr ? msg : "(null)");
    logging::EmergencyWrite(logging::LogLevel::Fatal, category, line);
    // Drain the frames queued before the failure while the writer thread is still alive; the
    // timeout keeps this from ever holding up the abort.
    logging::Flush(2000);
    std::abort();
}
