#pragma once
// Memory tag scopes -- memory profiling plan M3 (docs/memory_profiling_plan.md §4.3).
//
//     MEM_SCOPE(MemTag::Scene);   // every operator new on this thread, until the scope ends, is the scene's
//
// UE's LLM_SCOPE (LowLevelMemTracker.h:351-409): a per-thread stack of tags; the top is the owner of
// whatever the thread allocates. Here the stack is 16 deep (deeper scopes are counted but keep the
// 16th tag), lives in static TLS, and costs two thread-local writes per scope -- put scopes on
// owners (a loader, a system's Tick, a worker thread's body), not in loops. A task inherits the tag
// that was current where it was CREATED (core/task: TaskWithDeps captures it), so a scope around a
// fan-out covers the workers too.
//
// The tags only count anything with `--memtrack` (MemCounters.h, MemTrackEnabled); without it a
// scope still pushes and pops, which is all it costs.

#include <cstdint>

#include "core/memory/MemTags.h"

namespace mem
{
void PushTag(MemTag tag) noexcept;
void PopTag() noexcept;
// The calling thread's current tag (Untagged at depth 0).
MemTag CurrentTag() noexcept;

class TagScope
{
public:
    explicit TagScope(MemTag tag) noexcept { PushTag(tag); }
    ~TagScope() { PopTag(); }
    TagScope(const TagScope&) = delete;
    TagScope& operator=(const TagScope&) = delete;
};
} // namespace mem

#define MEM_SCOPE_CONCAT_INNER(a, b) a##b
#define MEM_SCOPE_CONCAT(a, b) MEM_SCOPE_CONCAT_INNER(a, b)
#define MEM_SCOPE(tag) ::mem::TagScope MEM_SCOPE_CONCAT(memTagScope_, __LINE__)(tag)
