#include "core/threading/ThreadRole.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"

namespace threading
{
namespace
{
thread_local ThreadRole t_role = ThreadRole::None;

std::atomic<bool> g_renderParked{false};
std::atomic<std::uint32_t> g_violations{0};
std::mutex g_sitesMutex;
std::unordered_set<std::string> g_sites; // "file:line", the first report of each
} // namespace

ThreadRole CurrentRole() noexcept { return t_role; }

void SetCurrentRole(ThreadRole role) noexcept { t_role = role; }

const char* RoleName(ThreadRole role) noexcept
{
    switch (role)
    {
    case ThreadRole::None: return "none";
    case ThreadRole::Game: return "game";
    case ThreadRole::Render: return "render";
    case ThreadRole::Sync: return "sync";
    }
    return "?";
}

void NoteRenderParked() noexcept { g_renderParked.store(true, std::memory_order_release); }

void ClearRenderParked() noexcept { g_renderParked.store(false, std::memory_order_release); }

bool RenderParked() noexcept { return g_renderParked.load(std::memory_order_acquire); }

namespace
{
std::atomic<ParkRenderHook> g_parkRenderHook{nullptr};
std::atomic<RenderInFlightHook> g_renderInFlightHook{nullptr};

struct GameWaitSite
{
    const char* site = nullptr;
    std::uint32_t count = 0;
};
std::mutex g_gameWaitMutex;
std::vector<GameWaitSite> g_gameWaits;
} // namespace

void SetParkRenderHook(ParkRenderHook hook, RenderInFlightHook inFlight) noexcept
{
    g_parkRenderHook.store(hook, std::memory_order_release);
    g_renderInFlightHook.store(inFlight, std::memory_order_release);
}

void ParkRender()
{
    if (const ParkRenderHook hook = g_parkRenderHook.load(std::memory_order_acquire))
    {
        hook();
    }
}

void WaitRenderForGame(const char* site)
{
    if (t_role != ThreadRole::Game)
    {
        return; // the render half itself, or the hand-over (nothing renders then)
    }
    const RenderInFlightHook inFlight = g_renderInFlightHook.load(std::memory_order_acquire);
    if (!inFlight || !inFlight())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_gameWaitMutex);
        auto it = std::find_if(g_gameWaits.begin(), g_gameWaits.end(),
                               [site](const GameWaitSite& s) { return s.site == site; });
        if (it == g_gameWaits.end())
        {
            g_gameWaits.push_back(GameWaitSite{ site, 0u });
            it = g_gameWaits.end() - 1;
        }
        ++it->count;
    }
    CPU_SCOPE(ProfilerScopes::kGameParkRender); // the game half waiting out the render thread's frame (a trace shows it)
    ParkRender();
}

bool RenderBusyForGame()
{
    if (t_role != ThreadRole::Game)
    {
        return false;
    }
    const RenderInFlightHook inFlight = g_renderInFlightHook.load(std::memory_order_acquire);
    return inFlight && inFlight();
}

void LogGameWaits(double seconds)
{
    std::vector<GameWaitSite> sites;
    {
        std::lock_guard<std::mutex> lock(g_gameWaitMutex);
        sites.swap(g_gameWaits);
    }
    if (sites.empty())
    {
        return;
    }
    std::sort(sites.begin(), sites.end(), [](const GameWaitSite& a, const GameWaitSite& b) { return a.count > b.count; });
    std::uint32_t total = 0;
    std::string list;
    for (const GameWaitSite& s : sites)
    {
        total += s.count;
        if (!list.empty()) { list += ", "; }
        list += std::string(s.site ? s.site : "?") + " x" + std::to_string(s.count);
    }
    LOG_INFO(logging::LogCategory::Core, "render thread: the game half waited for its frame {} times in {:.0f} s: {}",
             total, seconds, list);
}

void ReportRoleViolation(const char* expected, const char* what, const char* file, int line) noexcept
{
    g_violations.fetch_add(1u, std::memory_order_relaxed);
    try
    {
        std::string site = std::string(file ? file : "?") + ":" + std::to_string(line);
        {
            std::lock_guard<std::mutex> lock(g_sitesMutex);
            if (!g_sites.insert(site).second) { return; } // once per site; the count keeps going
        }
        LOG_ERROR(logging::LogCategory::Core, "thread role: {} ran as {}{}, needs {} ({})", what ? what : "?",
                  RoleName(t_role), RenderParked() ? " (render parked)" : "", expected ? expected : "?", site);
    }
    catch (...)
    {
    }
}

std::uint32_t RoleViolationCount() noexcept { return g_violations.load(std::memory_order_relaxed); }

void LogRoleCheckSummary() noexcept
{
#if defined(_DEBUG)
    const std::uint32_t count = RoleViolationCount();
    std::size_t sites = 0;
    {
        std::lock_guard<std::mutex> lock(g_sitesMutex);
        sites = g_sites.size();
    }
    if (count == 0u)
    {
        LOG_INFO(logging::LogCategory::Core, "thread roles: clean");
    }
    else
    {
        LOG_WARNING(logging::LogCategory::Core, "thread roles: {} violations at {} sites (see 'thread role:' errors)",
                    count, sites);
    }
#else
    LOG_DEBUG(logging::LogCategory::Core, "thread roles: checks compiled out (Release)");
#endif
}
} // namespace threading
