#include "rhi/Frame.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/threading/Knob.h"
#include "core/threading/ThreadRole.h"
#include "core/threading/RenderThread.h"

namespace rhi
{
namespace
{
// R16.10: WaitForPreviousFrame by call site. Rare (idle points), so a mutex and a short vector.
struct GpuIdleSite
{
    const char* file = nullptr;
    std::uint32_t line = 0;
    std::uint32_t count = 0;
    std::uint32_t fromGame = 0; // of them, from the game half (a park as well as the idle)
    double ms = 0.0;
};
std::mutex g_gpuIdleMutex;
std::vector<GpuIdleSite> g_gpuIdleSites;
std::uint64_t g_gpuIdleSessionCount = 0;
double g_gpuIdleSessionMs = 0.0;

// "sources/editor/commands/SpawnMeshCommand.cpp" -> "editor/commands/SpawnMeshCommand.cpp" (the compiler's full path).
std::string_view ShortSourcePath(const char* file)
{
    std::string_view p(file ? file : "?");
    for (const std::string_view root : { std::string_view("sources\\"), std::string_view("sources/") })
    {
        const size_t at = p.rfind(root);
        if (at != std::string_view::npos) { return p.substr(at + root.size()); }
    }
    return p;
}

void NoteGpuIdleWait(const std::source_location& where, bool fromGame, double ms)
{
    std::lock_guard<std::mutex> lock(g_gpuIdleMutex);
    ++g_gpuIdleSessionCount;
    g_gpuIdleSessionMs += ms;
    auto it = std::find_if(g_gpuIdleSites.begin(), g_gpuIdleSites.end(), [&](const GpuIdleSite& s) {
        return s.line == where.line() && std::strcmp(s.file, where.file_name()) == 0;
    });
    if (it == g_gpuIdleSites.end())
    {
        g_gpuIdleSites.push_back(GpuIdleSite{ where.file_name(), where.line(), 0u, 0u, 0.0 });
        it = g_gpuIdleSites.end() - 1;
    }
    ++it->count;
    if (fromGame) { ++it->fromGame; }
    it->ms += ms;
}
} // namespace

void Frame::WaitForFrame(UINT frameIndex) {
    scheduler_.WaitForFrame(frameIndex);
}

void Frame::SignalFrame(UINT frameIndex, std::uint64_t frameNumber) {
    // Step 2: both queues, one value. device_.ComputeQueue() is null when the device refused one, which
    // FrameScheduler handles by advancing the compute fence from the CPU instead.
    scheduler_.SignalFrame(device_.Queue(), device_.ComputeQueue(), frameIndex, frameNumber);
}

void Frame::LogGpuIdleWaits(double seconds)
{
    std::vector<GpuIdleSite> sites;
    std::uint64_t sessionCount = 0;
    double sessionMs = 0.0;
    {
        std::lock_guard<std::mutex> lock(g_gpuIdleMutex);
        sites.swap(g_gpuIdleSites);
        sessionCount = g_gpuIdleSessionCount;
        sessionMs = g_gpuIdleSessionMs;
    }
    if (sites.empty()) { return; }
    std::sort(sites.begin(), sites.end(), [](const GpuIdleSite& a, const GpuIdleSite& b) { return a.ms > b.ms; });
    std::uint32_t total = 0;
    double totalMs = 0.0;
    std::string list;
    for (const GpuIdleSite& s : sites)
    {
        total += s.count;
        totalMs += s.ms;
        if (!list.empty()) { list += ", "; }
        list += std::format("{}:{} x{}{} ({:.1f} ms)", ShortSourcePath(s.file), s.line, s.count,
                            s.fromGame != 0u ? std::format(" ({} game)", s.fromGame) : std::string(), s.ms);
    }
    LOG_INFO(logging::LogCategory::RenderRhi, "gpu idle waits: {} in {:.0f} s, {:.1f} ms (session: {}, {:.1f} ms): {}", total,
             seconds, totalMs, sessionCount, sessionMs, list);
}

void Frame::WaitForPreviousFrame(std::source_location where) {
    // R16.10: counted and timed by its caller (LogGpuIdleWaits), whatever it waits for.
    CPU_SCOPE(ProfilerScopes::kGpuIdleWait);
    const auto waitStart = std::chrono::steady_clock::now();
    struct Note
    {
        const std::source_location& where;
        bool fromGame;
        std::chrono::steady_clock::time_point start;
        ~Note()
        {
            NoteGpuIdleWait(where, fromGame,
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
    } note{ where, threading::CurrentRole() == threading::ThreadRole::Game, waitStart };
    // Fully wait for the GPU (for resize/destructor) — step 2: BOTH queues.
    //
    // This is the ONE function every idle path in the engine funnels through: ~50 call sites across
    // level switch, resize, editor commands, thumbnail eviction, screenshots and shutdown all call
    // it rather than touching the scheduler. That is why making it queue-correct here makes all of
    // them queue-correct, and why the plan puts this in step 2 instead of the hardening step — the
    // gates for every step in between are exactly that churn.
    // Game thread plan section 3.7: from the game half this is also where the render half PARKS -- until the next
    // hand-over the game may change what the render half walks (the scene's membership, the targets, the queue).
    const bool fromGame = threading::CurrentRole() == threading::ThreadRole::Game;
    if (fromGame)
    {
        threading::ParkRender(); // G8: a render thread finishes its frame in flight first (no thread: nothing)
        threading::NoteRenderParked();
    }
    FlushHandOverSubmits(); // G7: the game's deferred GPU work goes first, so the idle below covers it
    rhi_.Flush();           // G10: everything handed to the queues' road has reached them
    scheduler_.WaitForGpuIdle(device_.Queue(), device_.ComputeQueue());
    if (fromGame)
    {
        // G5: a parked render half takes the game's knobs at once -- a live setter that parks (a target rebuild)
        // reads the value the game half just set.
        knob::Exchange();
    }
}

bool Frame::HandOverSubmitsGoNow() const
{
    const threading::ThreadRole role = threading::CurrentRole();
    return role == threading::ThreadRole::Render || role == threading::ThreadRole::Sync ||
           (role == threading::ThreadRole::Game && threading::RenderParked());
}

void Frame::ExecuteOrDefer(ID3D12CommandList* list)
{
    if (!list) { return; }
    NoteWork(); // R8.1: handed out now, whether it goes at once or at the hand-over
    if (HandOverSubmitsGoNow())
    {
        FlushHandOverSubmits(); // keep the queue order: anything deferred before this goes first
        Microsoft::WRL::ComPtr<ID3D12CommandList> keep(list);
        rhi_.Run([this, keep]()
        {
            ID3D12CommandList* l = keep.Get();
            device_.Queue()->ExecuteCommandLists(1, &l);
        });
        return;
    }
    std::lock_guard<std::mutex> lock(handOverMutex_);
    HandOverOp op;
    op.list = list;
    handOverOps_.push_back(std::move(op));
}

bool Frame::SignalOrDefer(ID3D12Fence* fence, UINT64 value)
{
    if (!fence) { return false; }
    if (HandOverSubmitsGoNow())
    {
        FlushHandOverSubmits();
        if (!rhi_.Threaded())
        {
            return SUCCEEDED(device_.Queue()->Signal(fence, value));
        }
        Microsoft::WRL::ComPtr<ID3D12Fence> keep(fence);
        rhi_.Run([this, keep, value]()
        {
            if (FAILED(device_.Queue()->Signal(keep.Get(), value)))
            {
                LOG_ERROR(logging::LogCategory::RenderRhi, "a fence signal failed (value {})", value);
            }
        });
        return true;
    }
    std::lock_guard<std::mutex> lock(handOverMutex_);
    HandOverOp op;
    op.fence = fence;
    op.value = value;
    handOverOps_.push_back(std::move(op));
    return true;
}

void Frame::FlushHandOverSubmits()
{
    std::vector<HandOverOp> ops;
    {
        std::lock_guard<std::mutex> lock(handOverMutex_);
        if (handOverOps_.empty()) { return; }
        ops.swap(handOverOps_);
    }
    TC_CHECK_THREAD(HandOverSubmitsGoNow(), "render|sync, or game with the render half parked",
                    "rhi::Frame::FlushHandOverSubmits");
    rhi_.Run([this, ops = std::move(ops)]()
    {
        for (const HandOverOp& op : ops)
        {
            if (op.list)
            {
                ID3D12CommandList* list = op.list.Get();
                device_.Queue()->ExecuteCommandLists(1, &list);
            }
            else if (op.fence && FAILED(device_.Queue()->Signal(op.fence.Get(), op.value)))
            {
                LOG_ERROR(logging::LogCategory::RenderRhi, "hand-over: a deferred fence signal failed (value {})",
                          op.value);
            }
        }
    });
}
} // namespace rhi
