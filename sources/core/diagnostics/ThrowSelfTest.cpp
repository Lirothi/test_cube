#include "core/diagnostics/ThrowSelfTest.h"

#include "core/logging/Log.h"

#include <stdexcept>
#include <string>

namespace diag
{
namespace
{
constexpr int kNeverFrames = 1800;     // the site never ran: the run cannot judge it
constexpr int kLandFrames = 600;       // a background site still waiting this long after its throw never landed
constexpr int kFrameTaskFrames = 8;    // a frame task's throw: the render half may run a frame behind the game half

struct SiteName
{
    ThrowSite site;
    const char* name;
};
constexpr SiteName kSites[] = {
    { ThrowSite::TexStream, "texstream" },
    { ThrowSite::TexDecode, "texdecode" },
    { ThrowSite::FsProbe, "fsprobe" },
    { ThrowSite::ViewCamera, "view" },
    { ThrowSite::ViewOther, "view-other" },
};

const char* NameOf(ThrowSite site)
{
    for (const SiteName& s : kSites)
    {
        if (s.site == site) { return s.name; }
    }
    return "none";
}

bool IsFrameTaskSite(ThrowSite site)
{
    return site == ThrowSite::ViewCamera || site == ThrowSite::ViewOther;
}

std::atomic<bool> g_thrown{ false };
std::atomic<bool> g_landed{ false };
std::atomic<int> g_frames{ 0 };          // ThrowSelfTestFrame's count (the game thread writes it)
std::atomic<int> g_thrownAtFrame{ -1 };
int g_exitCode = -1;
} // namespace

namespace detail
{
bool TakeThrow(ThrowSite)
{
    return !g_thrown.exchange(true, std::memory_order_acq_rel);
}

void ThrowAtSite(ThrowSite site)
{
    g_thrownAtFrame.store(g_frames.load(std::memory_order_relaxed), std::memory_order_relaxed);
    LOG_WARNING(logging::LogCategory::Task, "throw self-test: the {} site throws once inside its task (game frame {})",
                NameOf(site), g_frames.load(std::memory_order_relaxed));
    throw std::runtime_error(std::string("the throw self-test's exception at the ") + NameOf(site) + " site");
}

void NoteLanded(ThrowSite)
{
    if (g_thrown.load(std::memory_order_acquire)) { g_landed.store(true, std::memory_order_release); }
}
} // namespace detail

bool ArmThrowSelfTest(std::string_view site)
{
    for (const SiteName& s : kSites)
    {
        if (site == s.name)
        {
            detail::g_throwSiteArmed.store(static_cast<std::uint8_t>(s.site), std::memory_order_relaxed);
            LOG_INFO(logging::LogCategory::Task, "throw self-test: armed -- the {} site throws once inside its {}", s.name,
                     IsFrameTaskSite(s.site) ? "frame task (the process must end with a Fatal naming it)"
                                             : "background task (its owner must see it land and start again)");
            return true;
        }
    }
    LOG_ERROR(logging::LogCategory::Task, "throw self-test: no site '{}' (texstream, texdecode, fsprobe, view, view-other)",
              site);
    return false;
}

bool ThrowSelfTestFrame()
{
    const auto site = static_cast<ThrowSite>(detail::g_throwSiteArmed.load(std::memory_order_relaxed));
    if (site == ThrowSite::None || g_exitCode >= 0) { return false; }
    const int frames = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool thrown = g_thrown.load(std::memory_order_acquire);
    if (!thrown)
    {
        if (frames < kNeverFrames) { return false; }
        LOG_ERROR(logging::LogCategory::Task, "throw self-test {}: FAIL -- the site never ran in {} frames; nothing judged",
                  NameOf(site), frames);
        g_exitCode = 1;
        return true;
    }
    const int thrownAt = g_thrownAtFrame.load(std::memory_order_relaxed);
    const int since = frames - (thrownAt >= 0 ? thrownAt : frames);
    if (IsFrameTaskSite(site))
    {
        if (since < kFrameTaskFrames) { return false; }
        LOG_ERROR(logging::LogCategory::Task, "throw self-test {}: FAIL -- the frame task threw and {} frames went on: its "
                  "exception was taken for a finished cull; did not crash", NameOf(site), since);
        g_exitCode = 1;
        return true;
    }
    if (g_landed.load(std::memory_order_acquire))
    {
        LOG_INFO(logging::LogCategory::Task, "throw self-test {}: PASS -- the task threw in game frame {}, its owner saw it "
                 "land {} frame(s) later", NameOf(site), thrownAt, since);
        g_exitCode = 0;
        return true;
    }
    if (since < kLandFrames) { return false; }
    LOG_ERROR(logging::LogCategory::Task, "throw self-test {}: FAIL -- the task threw in game frame {} and {} frames later its "
              "owner still waits for it: it never landed", NameOf(site), thrownAt, since);
    g_exitCode = 1;
    return true;
}

int ThrowSelfTestExitCode()
{
    return g_exitCode;
}
} // namespace diag
