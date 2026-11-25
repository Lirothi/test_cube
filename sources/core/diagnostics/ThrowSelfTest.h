#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

// One exception thrown on purpose inside a task, at a named site, and the verdict on what came of it (the second review
// of wave 1, docs/architecture_review_plan.md). Two roads, after the owner's answer В6:
//   a frame task's (--crash-selftest=view-throw[@other]: Scene::PrepareViews' culling tasks) -- the process must end with
//     a Fatal naming the task and its dump; frames going on after the throw is the failure;
//   a detached background task's (--throw-selftest=texstream|texdecode|fsprobe) -- its owner must see the work land and
//     start again (ThrowSelfTestLanded); a site still waiting 600 frames after the throw is the failure: the task's
//     `done` flag ended its body, and the throw skipped it (M1's class).
// The game starts as always; ThrowSelfTestFrame (App.cpp, once a frame) says when the verdict is in.
namespace diag
{
enum class ThrowSite : std::uint8_t
{
    None,
    TexStream,  // TextureStreamingManager's cycle task; landed = the next cycle launched
    TexDecode,  // texdecode::Request's decode task (driven by texdecode::ThrowSelfTestFrame); landed = no claim left
    FsProbe,    // MaterialManager::RequestFSProbeAsync's probe task; landed = the next probe started
    ViewCamera, // Scene::PrepareViews' camera-view culling task (a frame task: must end the process)
    ViewOther,  // the first of its other views' culling tasks (likewise)
};

namespace detail
{
inline std::atomic<std::uint8_t> g_throwSiteArmed{ 0 }; // a ThrowSite; set once at boot
[[noreturn]] void ThrowAtSite(ThrowSite site);
void NoteLanded(ThrowSite site);
bool TakeThrow(ThrowSite site); // true once: the armed site's first arrival
} // namespace detail

// Boot (main.cpp): arms the site of that name (texstream, texdecode, fsprobe, view, view-other). False (and an Error)
// for an unknown one.
bool ArmThrowSelfTest(std::string_view site);
inline bool ThrowSelfTestArmed(ThrowSite site)
{
    return detail::g_throwSiteArmed.load(std::memory_order_relaxed) == static_cast<std::uint8_t>(site);
}
// Inside the site's task, before its work: throws once when the site is armed. One relaxed load otherwise.
inline void ThrowSelfTestPoint(ThrowSite site)
{
    if (ThrowSelfTestArmed(site) && detail::TakeThrow(site)) { detail::ThrowAtSite(site); }
}
// The site's owner saw its work land after the throw and start again. Nothing when unarmed or not thrown yet.
inline void ThrowSelfTestLanded(ThrowSite site)
{
    if (ThrowSelfTestArmed(site)) { detail::NoteLanded(site); }
}
// The game thread, once a frame after Renderer::BeginFrame (App.cpp): true once, when the verdict is in and the app
// should quit.
bool ThrowSelfTestFrame();
// After App::Run: 0 PASS, 1 FAIL, -1 not armed (or no verdict).
int ThrowSelfTestExitCode();
} // namespace diag
