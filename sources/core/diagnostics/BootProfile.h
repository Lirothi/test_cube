#pragma once

#include <cstddef>
#include <string>

// BOOT TIMELINE. The frame profiler starts at the first frame, so everything before it -- device
// creation, PSO warmup, level load, the upload batch -- has never been measured at all. That was
// tolerable while boot took a couple of seconds; under Debug + `--scene-stress-gbv` it is minutes,
// and a run costs more than the whole investigation is worth if it only produces "it was slow".
//
// Two shapes, because boot has two kinds of cost:
//   * SCOPES  -- a tree of coarse phases (BOOT_SCOPE). Nested, one line each, wall time + self time.
//   * BUCKETS -- things that happen hundreds of times (PSO creation, shader compiles, texture
//                uploads, mesh loads). A tree entry per item would be unreadable, so they aggregate
//                into count + total + the SLOWEST FEW BY NAME. Naming the worst item is the point:
//                under GBV the cost is not spread evenly, it sits on a handful of pipelines.
//
// Always compiled in (a QPC pair per scope, and scopes are coarse). The dump lands in
// the session log (`[profiling]` records) when the first frame is presented, so a run that is slow to BOOT does not
// have to also survive to exit to tell you why.
namespace boot {

// Instant event, no duration: "we got here at T".
void Mark(const char* label);

// A phase. Nests by construction order per thread; overlapping work on other threads is recorded
// with its own thread tag rather than being folded into the caller's tree.
class Scope
{
public:
    explicit Scope(const char* label);
    Scope(const char* label, std::string detail);
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    std::size_t index_;
};

// Aggregated repeated work. `name` is the bucket (static string, e.g. "PSO create"); `item` names
// this particular one (a pipeline hash, a shader path) and is only kept if it lands in the slowest
// few. Cheap enough to call per item: one lock, no formatting unless it is a new worst case.
void AddBucket(const char* name, double milliseconds, const char* item);
void AddBucket(const char* name, double milliseconds, const std::string& item);

// Counter with no time attached ("textures loaded", "PSO cache rejects") -- context for the buckets.
void AddCount(const char* name, long long delta = 1);

// PER-FRAME instrumentation gate. Everything above is boot-only -- it runs once, so its cost is
// irrelevant. The per-pass, per-submit and per-frame buckets are different: they sit on the
// recording path, and a mutex plus a std::string per pass per frame is not something to ship on by
// default. Off unless a diagnostic run asks for it (`--scene-stress`, `--gbv`, `--boot-profile`).
//
// Read directly rather than through a function so the check inlines to a load and a branch, and
// the timing code after it is skipped entirely.
inline bool g_frameProfiling = false;
void SetFrameProfiling(bool on);

// Writes the report into the session log (summary at Info, per-scope timeline at Debug, category
// `profiling`). Safe to call more than once; each call emits a fresh report, so
// "at first frame" and "at exit" can both be dumped and compared.
void Dump(const char* reason);

// Milliseconds since the process started (the timer starts on first use, which is the first thing
// main() touches).
double ElapsedMs();

} // namespace boot

// Concatenation dance so two scopes on different lines do not collide.
#define BOOT_SCOPE_CAT2(a, b) a##b
#define BOOT_SCOPE_CAT(a, b) BOOT_SCOPE_CAT2(a, b)
#define BOOT_SCOPE(label) ::boot::Scope BOOT_SCOPE_CAT(bootScope_, __LINE__)(label)
#define BOOT_SCOPE_D(label, detail) ::boot::Scope BOOT_SCOPE_CAT(bootScope_, __LINE__)(label, detail)
