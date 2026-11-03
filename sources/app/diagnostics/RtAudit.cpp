#include "app/diagnostics/RtAudit.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "core/threading/ThreadRole.h" // RenderBusyForGame: the self-test writes only beside a frame in flight

namespace rtaudit
{
namespace
{
bool g_enabled = false;
bool g_selfTest = false;
double g_seconds = 600.0;
double g_start = -1.0;
bool g_finished = false;
int g_exitCode = 0;

// The render thread's: the hashes at its frame's start. Read at its end, on the same thread.
Scene::FrozenStateHash g_begin;
Scene::FrozenStateHash g_end;

std::mutex g_mutex; // the findings: written by the render thread, read by the game thread after its wait
std::map<std::string, std::uint32_t> g_moved;
std::uint64_t g_framesAudited = 0;
std::size_t g_objects = 0;
std::uint32_t g_selfTestWrites = 0;
std::uint64_t g_gameFrames = 0;

std::mt19937& Rng()
{
    thread_local std::mt19937 rng(static_cast<std::uint32_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
        static_cast<std::size_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
    return rng;
}
} // namespace

void Configure(double seconds, bool selfTest)
{
    g_enabled = true;
    g_selfTest = selfTest;
    g_seconds = seconds > 0.0 ? seconds : 600.0;
    LOG_INFO(logging::LogCategory::Core, "rt audit: on for {:.0f} s{} (the render thread forced on)", g_seconds,
             selfTest ? ", with the self-test" : "");
}

bool Enabled() { return g_enabled; }

void Jitter()
{
    if (!g_enabled)
    {
        return;
    }
    std::uniform_int_distribution<int> micros(0, 2000);
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(micros(Rng()));
    while (std::chrono::steady_clock::now() < until)
    {
        std::this_thread::yield();
    }
}

void BeginRenderFrame(const Scene& scene)
{
    if (!g_enabled)
    {
        return;
    }
    scene.HashFrozenState(g_begin);
}

void EndRenderFrame(const Scene& scene)
{
    if (!g_enabled)
    {
        return;
    }
    scene.HashFrozenState(g_end);
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_framesAudited;
    g_objects = g_end.objects;
    if (g_begin.objects != g_end.objects || g_begin.pointLights != g_end.pointLights ||
        g_begin.spotLights != g_end.spotLights)
    {
        ++g_moved["the scene's membership (objects / lights added or removed)"];
        return;
    }
    for (std::size_t i = 0; i < g_end.hashes.size(); ++i)
    {
        if (g_begin.hashes[i] != g_end.hashes[i])
        {
            ++g_moved[scene.FrozenFieldName(g_end, i)];
        }
    }
}

void GameHalf(Scene& scene)
{
    if (!g_enabled || !g_selfTest)
    {
        return;
    }
    // Only while the render thread draws (a write with nothing in flight is no race), and then one frame in twenty:
    // often enough for a short scene-stress run, rare enough to leave most frames clean.
    if (!threading::RenderBusyForGame())
    {
        return;
    }
    if (++g_gameFrames % 20u == 10u && scene.ScribbleFrozenStateForAudit())
    {
        if (g_selfTestWrites++ == 0u)
        {
            LOG_INFO(logging::LogCategory::Core,
                     "rt audit self-test: wrote object #0's model matrix from the game half during the render thread's "
                     "frame (one frame in twenty while it draws); the audit must FAIL naming it");
        }
    }
}

bool Finished(double nowSeconds, bool now)
{
    if (!g_enabled || g_finished)
    {
        return g_finished;
    }
    if (g_start < 0.0)
    {
        g_start = nowSeconds;
    }
    if (!now && nowSeconds - g_start < g_seconds)
    {
        return false;
    }
    g_finished = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_moved.empty())
    {
        LOG_INFO(logging::LogCategory::Core,
                 "rt audit: PASS -- {} frames, {} objects: nothing the hand-over froze moved while the render thread "
                 "drew{}", g_framesAudited, g_objects,
                 g_selfTest ? " (the self-test's writes were NOT seen: the audit is blind)" : "");
        g_exitCode = g_selfTest ? 1 : 0;
        return true;
    }
    std::vector<std::pair<std::string, std::uint32_t>> moved(g_moved.begin(), g_moved.end());
    std::sort(moved.begin(), moved.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string list;
    for (std::size_t i = 0; i < moved.size() && i < 12u; ++i)
    {
        list += (i ? ", " : "") + moved[i].first + " x" + std::to_string(moved[i].second);
    }
    if (moved.size() > 12u)
    {
        list += ", ... (" + std::to_string(moved.size() - 12u) + " more)";
    }
    LOG_ERROR(logging::LogCategory::Core, "rt audit: FAIL -- {} fields moved during the render thread's frame in {} "
              "frames: {}{}", moved.size(), g_framesAudited, list,
              g_selfTest ? " (self-test: this FAIL is the expected answer)" : "");
    // The self-test's FAIL is its pass (the audit sees a write): exit 0 there, 1 when it saw nothing (above).
    g_exitCode = g_selfTest ? 0 : static_cast<int>(std::min<std::size_t>(moved.size(), 255u));
    return true;
}

int ExitCode() { return g_exitCode; }
} // namespace rtaudit
