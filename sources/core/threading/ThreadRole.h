#pragma once
// Which half of the frame a thread works for -- the game thread plan (docs/game_thread_plan.md section 3.4).
//
// UE's IsInGameThread / IsInRenderingThread (CoreGlobals.h:703-745) as a ROLE, not a thread id: a task inherits
// the role current where it was CREATED (core/task: TaskWithDeps captures it, as it does the memory tag), and on
// one thread (frame.renderThread 0) the PHASE sets it -- the game half of the frame runs as Game, the render half
// as Render. So an ownership check fires on one thread, deterministically, before there is a second one.
//
//     TC_CHECK_GAME("RenderableObject::SetPosition");   // Debug: logs once per call site when the role is wrong
//
// A check never stops the process: it logs an Error naming the site and the role it found (once per site) and
// counts; LogRoleCheckSummary() prints the verdict a gate reads ("thread roles: clean" / "... N violations").

#include <cstdint>

namespace threading
{
enum class ThreadRole : std::uint8_t
{
    None = 0, // outside the frame: the log writer, the MCP socket, a worker between tasks
    Game,     // the game half: input, UI, the editor, Scene::Tick
    Render,   // the render half: views, recording, submit, Present
    Sync,     // the hand-over between them (and boot / shutdown): nobody else runs, both sides may be touched
};

ThreadRole CurrentRole() noexcept;
void SetCurrentRole(ThreadRole role) noexcept;
const char* RoleName(ThreadRole role) noexcept;

class RoleScope
{
public:
    explicit RoleScope(ThreadRole role) noexcept : previous_(CurrentRole()) { SetCurrentRole(role); }
    ~RoleScope() { SetCurrentRole(previous_); }
    RoleScope(const RoleScope&) = delete;
    RoleScope& operator=(const RoleScope&) = delete;

private:
    ThreadRole previous_;
};

inline bool RoleIs(ThreadRole a, ThreadRole b) noexcept { return CurrentRole() == a || CurrentRole() == b; }

// The render half is PARKED (plan section 3.7): the game half waited the GPU out (Renderer::WaitForPreviousFrame
// from the game role) and nothing renders until the next hand-over clears it. Only then may the game half change
// what the render half walks -- the scene's membership, the renderer's targets, the queue.
void NoteRenderParked() noexcept;
void ClearRenderParked() noexcept;
bool RenderParked() noexcept;
inline bool GameParkedOrSync() noexcept
{
    return CurrentRole() == ThreadRole::Sync || (CurrentRole() == ThreadRole::Game && RenderParked());
}

void ReportRoleViolation(const char* expected, const char* what, const char* file, int line) noexcept;
std::uint32_t RoleViolationCount() noexcept;
// One line for the session log: "thread roles: clean" or the count and the number of sites. Called at the end of
// App::Run and of the scene-stress harness.
void LogRoleCheckSummary() noexcept;
} // namespace threading

#if defined(_DEBUG)
#define TC_CHECK_THREAD(condition, expected, what)                                                 \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ::threading::ReportRoleViolation((expected), (what), __FILE__, __LINE__);              \
        }                                                                                          \
    } while (0)
#else
#define TC_CHECK_THREAD(condition, expected, what) ((void)0)
#endif

#define TC_CHECK_GAME(what)                                                                        \
    TC_CHECK_THREAD(::threading::RoleIs(::threading::ThreadRole::Game, ::threading::ThreadRole::Sync), "game|sync", what)
#define TC_CHECK_RENDER(what)                                                                      \
    TC_CHECK_THREAD(::threading::RoleIs(::threading::ThreadRole::Render, ::threading::ThreadRole::Sync), "render|sync", what)
#define TC_CHECK_SYNC(what)                                                                        \
    TC_CHECK_THREAD(::threading::CurrentRole() == ::threading::ThreadRole::Sync, "sync", what)
// The scene's membership, the renderer's targets: the hand-over, or the game half with the render half parked.
#define TC_CHECK_GAME_PARKED_OR_SYNC(what)                                                         \
    TC_CHECK_THREAD(::threading::GameParkedOrSync(), "sync, or game with the render half parked", what)
