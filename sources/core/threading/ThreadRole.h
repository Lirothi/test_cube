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

// The render half is PARKED (plan section 3.7): nothing renders until the next hand-over clears it -- the game half
// waited the GPU out (Renderer::WaitForPreviousFrame from the game role), or only the render thread's frame in flight
// (ParkRenderForGame: what the game then frees goes through render::deferred). Only then may the game half change
// what the render half walks -- the scene's membership, the renderer's targets, the queue.
void NoteRenderParked() noexcept;
void ClearRenderParked() noexcept;
bool RenderParked() noexcept;
inline bool GameParkedOrSync() noexcept
{
    return CurrentRole() == ThreadRole::Sync || (CurrentRole() == ThreadRole::Game && RenderParked());
}
// How the game half parks a render THREAD (plan G8, core/threading/RenderThread.h): wait for its frame in flight.
// Renderer::WaitForPreviousFrame calls ParkRender from the game role; with no render thread registered it does
// nothing -- on one thread the render half is the game thread's own next phase. May rethrow what that frame threw.
using ParkRenderHook = void (*)();
using RenderInFlightHook = bool (*)();
// inFlight: the game thread's question (RenderBusyForGame; a task asking it is an Error of its own). inFlightAnyThread:
// the same state for any thread, quietly (RenderThreadInFlight).
void SetParkRenderHook(ParkRenderHook hook, RenderInFlightHook inFlight = nullptr,
                       RenderInFlightHook inFlightAnyThread = nullptr) noexcept;
void ParkRender();
// Plan G8b: a game-half call that reaches what the render thread's frame in flight uses (a renderer setter, a
// subsystem the editor mutates) waits for that frame first -- correct by construction, at the price of that frame's
// overlap. Counted per site; LogGameWaits prints the sites (Info, `core`) and starts over.
void WaitRenderForGame(const char* site);
// R16.10: the park a game-half edit of the scene's membership needs (TC_CHECK_GAME_PARKED_OR_SYNC), without a GPU idle:
// WaitRenderForGame, then the render half noted PARKED until the next hand-over (Scene::Sync clears it) -- the render
// thread does not start a frame before that hand-over anyway, but the checks (and HandOverSubmitsGoNow) ask the flag.
// What the edit frees goes through render::deferred. Nothing off the game role.
void ParkRenderForGame(const char* site);
void LogGameWaits(double seconds);
// On the game thread: the render thread is drawing a frame now (the UI runs beside it), so a per-frame-slot resource
// the UI picks by Renderer::GetCurrentFrameIndex may still be the GPU's. False on one thread.
bool RenderBusyForGame();
// The render thread is drawing a frame now, asked from any thread and any role (false with no render thread:
// frame.renderThread 0).
bool RenderThreadInFlight() noexcept;
// Nothing of the frame runs beside this call (docs/architecture_review_plan.md R15.6): the hand-over (Sync), the game or
// the render role while the render thread draws no frame (between frames, the game half with it parked, the render
// half's upkeep, one thread), or the render thread's own body outside a task -- the frame's sanctioned pre-graph point
// (an ocean's caustics atlas loads there). A task of a frame in flight is not. With frame.renderThread 0 the frame's own
// workers cannot be told apart from the frame's thread: only the render-thread mode checks them.
bool FrameQuiet() noexcept;
// RenderThread::Main marks its thread; TaskSystem::RunTask marks a task's body (FrameQuiet tells the two apart).
void MarkRenderThread() noexcept;
class TaskBodyScope
{
public:
    TaskBodyScope() noexcept;
    ~TaskBodyScope();
    TaskBodyScope(const TaskBodyScope&) = delete;
    TaskBodyScope& operator=(const TaskBodyScope&) = delete;
};

void ReportRoleViolation(const char* expected, const char* what, const char* file, int line) noexcept;
std::uint32_t RoleViolationCount() noexcept;
// One line for the session log: "thread roles: clean" or the count and the number of sites. Called at the end of
// App::Run and of the scene-stress harness.
void LogRoleCheckSummary() noexcept;
} // namespace threading

// Where the checks are compiled in: Debug always, and any configuration built with /p:TcThreadChecks=true -- the define
// TC_THREAD_CHECKS=1 (docs/architecture_review_plan.md R15.5, the owner's answer В7: Release_Editor behind a build switch).
#if defined(_DEBUG) || (defined(TC_THREAD_CHECKS) && TC_THREAD_CHECKS)
#define TC_THREAD_CHECKS_ON 1
#else
#define TC_THREAD_CHECKS_ON 0
#endif

#if TC_THREAD_CHECKS_ON
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
// Shared state of the frame without a lock (texture streaming's registry, the mesh cache, a material's PSO): touched
// while no frame is being drawn (FrameQuiet).
#define TC_CHECK_FRAME_QUIET(what)                                                                 \
    TC_CHECK_THREAD(::threading::FrameQuiet(), "sync, or game/render with no frame in flight", what)
