#include "app/harness/CaptureHarnesses.h"

#include "app/BootOptions.h"
#include "app/settings/Scalability.h"
#include "core/Helpers.h" // GetTimeSeconds
#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/threading/ThreadRole.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/Screenshot.h"
#include "rendering/wind/WindState.h" // --shot-count steps the frozen wind clock (g_windStep)

#include <cstdio>

namespace harness
{
bool ShotHarness::Wanted() const { return !Boot().frame.shotPath.empty(); }

void ShotHarness::OnFrame(FrameContext& ctx)
{
    if (!started_) { started_ = true; shotStart_ = GetTimeSeconds(); }

    // Apply before the settle delay is judged, so the value is in place for the whole
    // interval rather than only for the frame the shot is taken on.
    if (!Boot().frame.sweepSetting.empty() && shotIndex_ < static_cast<int>(Boot().frame.sweepValues.size()) &&
        appliedSweepIndex_ != shotIndex_)
    {
        const float value = Boot().frame.sweepValues[static_cast<size_t>(shotIndex_)];
        const bool known = apply_(ctx.scene, ctx.settings,
                                          Boot().frame.sweepSetting, value);
        settings::TheScalability().Pin(Boot().frame.sweepSetting);
        if (!known)
        {
            // Once per process: the sweep name never changes between shots.
            LOG_WARNING_ONCE(logging::LogCategory::App, "--sweep {}: unknown setting", Boot().frame.sweepSetting);
        }
        for (const auto& [extraSetting, extraValues] : Boot().frame.sweepExtra)
        {
            if (shotIndex_ >= static_cast<int>(extraValues.size())) { continue; }
            const float extraValue = extraValues[static_cast<size_t>(shotIndex_)];
            settings::TheScalability().Pin(extraSetting);
            if (!apply_(ctx.scene, ctx.settings, extraSetting, extraValue))
            {
                LOG_WARNING_ONCE_PER_MESSAGE(logging::LogCategory::App, "--sweep {}: unknown setting", extraSetting);
            }
            LOG_INFO(logging::LogCategory::App, "--sweep shot {}: {} = {}", shotIndex_, extraSetting, extraValue);
        }
        // The camera must not carry the previous value's adaptation into this shot.
        ctx.renderer.Exposure().RequestReset();
        appliedSweepIndex_ = shotIndex_;
        LOG_INFO(logging::LogCategory::App, "--sweep shot {}: {} = {}", shotIndex_,
                 Boot().frame.sweepSetting, value);
    }

    const double delay = shotIndex_ == 0 ? Boot().frame.shotDelaySec : Boot().frame.shotIntervalSec;
    if (GetTimeSeconds() - shotStart_ >= delay)
    {
        std::string path = Boot().frame.shotPath;
        if (Boot().frame.shotCount > 1)
        {
            char suffix[8];
            std::snprintf(suffix, sizeof(suffix), "_%02d", shotIndex_);
            const size_t dot = path.rfind('.');
            if (dot != std::string::npos) { path.insert(dot, suffix); }
            else { path += suffix; }
        }
        // A --sweep with a --profdump also writes the profiler rows AT each shot
        // (<dump>_NN), so every value's cost comes out of the same process as its picture;
        // the final dump below still covers the window after the last one.
        if (!Boot().frame.profDumpPath.empty() && !Boot().frame.sweepValues.empty())
        {
            std::string dumpPath = Boot().frame.profDumpPath;
            char suffix[8];
            std::snprintf(suffix, sizeof(suffix), "_%02d", shotIndex_);
            const size_t dot = dumpPath.rfind('.');
            if (dot != std::string::npos) { dumpPath.insert(dot, suffix); }
            else { dumpPath += suffix; }
            if (Profiler::Get().DumpOverlay(dumpPath))
            {
                LOG_INFO(logging::LogCategory::Profiling, "profiler overlay dump saved: {}", dumpPath);
            }
            else
            {
                LOG_ERROR(logging::LogCategory::Profiling, "profiler overlay dump FAILED: {}", dumpPath);
            }
        }
        bool ok = false;
        {
            threading::RoleScope renderRole(threading::ThreadRole::Render); // the presented backbuffer
            ok = Screenshot::SaveBackbufferPng(ctx.renderer, path);
        }
        if (ok)
        {
            LOG_INFO(logging::LogCategory::App, "screenshot saved: {} (frame {})", path,
                     ctx.renderer.GetTotalFrameNumber());
        }
        else
        {
            LOG_ERROR(logging::LogCategory::App, "screenshot FAILED: {}", path);
        }
        ++shotIndex_;
        if (shotIndex_ >= Boot().frame.shotCount)
        {
            Boot().frame.shotPath.clear();
            ctx.running = false;
        }
        else
        {
            vfx::g_windStep_game = static_cast<float>(Boot().frame.shotStepSec);
            shotStart_ = GetTimeSeconds();
        }
    }
}

bool ProfDumpHarness::Wanted() const { return !Boot().frame.profDumpPath.empty(); }

void ProfDumpHarness::OnFrame(FrameContext& ctx)
{
    if (!started_) { started_ = true; start_ = GetTimeSeconds(); }
    // With a --sweep the dump waits for the LAST shot, so its window covers the swept
    // values instead of the settle time before the first one.
    const bool due = Boot().frame.sweepValues.empty()
        ? (GetTimeSeconds() - start_ >= Boot().frame.shotDelaySec)
        : (Boot().frame.shotPath.empty() && !ctx.running);
    if (due)
    {
        const bool ok = Profiler::Get().DumpOverlay(Boot().frame.profDumpPath);
        if (ok)
        {
            LOG_INFO(logging::LogCategory::Profiling, "profiler overlay dump saved: {}", Boot().frame.profDumpPath);
        }
        else
        {
            LOG_ERROR(logging::LogCategory::Profiling, "profiler overlay dump FAILED: {}", Boot().frame.profDumpPath);
        }
        Boot().frame.profDumpPath.clear();
        ctx.running = false;
    }
}

bool TraceHarness::Wanted() const { return Boot().frame.traceFrames != 0; }

void TraceHarness::OnFrame(FrameContext& ctx)
{
    if (!started_) { started_ = true; start_ = GetTimeSeconds(); }
    if (!requested_ && GetTimeSeconds() - start_ >= Boot().frame.shotDelaySec)
    {
        Profiler::Get().RequestTraceCapture(Boot().frame.traceFrames);
        framesLeft_ = Boot().frame.traceFrames + 30; // margin: the write happens on the last frame
        requested_ = true;
    }
    else if (requested_ && --framesLeft_ == 0)
    {
        Boot().frame.traceFrames = 0;
        ctx.running = false;
    }
}
}
