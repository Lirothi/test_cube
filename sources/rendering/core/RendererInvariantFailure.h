#pragma once

// Shared fail-fast for renderer invariant violations that would lose,
// duplicate, corrupt, or mis-order GPU work. Active in EVERY build config —
// never assert-only, never a silent drop. Safe to call from render-pass worker
// tasks: it does not throw (the task system swallows task exceptions).
[[noreturn]] void RendererInvariantFailure(const char* msg);
