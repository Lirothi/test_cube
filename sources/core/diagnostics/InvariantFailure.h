#pragma once

#include "core/logging/LogCategory.h"

// Shared fail-fast for invariant violations that would lose, duplicate, corrupt, or mis-order work -- a hang, a write past
// a buffer, a frame without a pass. Active in EVERY build config -- never assert-only, never a silent drop. Safe to call
// from a worker task: it does not throw (a task's exception is the task system's to judge, not a way out of a broken
// invariant). Its record: logs/invariant_failure.log (appended), one Fatal line of `category` straight to the session log
// (no writer thread), a flush, abort() -- whose handler (core/diagnostics/CrashHandler) writes the minidump and exits 3.
// `msg` says what was broken and by what: the numbers, the names.
[[noreturn]] void InvariantFailure(logging::LogCategory category, const char* msg) noexcept;

// The renderer's call sites (category render).
[[noreturn]] inline void RendererInvariantFailure(const char* msg) noexcept
{
    InvariantFailure(logging::LogCategory::Render, msg);
}
