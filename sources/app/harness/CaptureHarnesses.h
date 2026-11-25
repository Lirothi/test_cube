#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "app/harness/Harness.h"

// The capture harnesses (architecture plan R3.3): moved out of App::Run's after-frame step, where they ran in this
// order -- the shot (with its series and sweep), then the profiler dump, then the trace.
namespace harness
{
// "--shot=<path>": after the warmup delay (ocean/particle sim settling), grab the
// just-presented backbuffer to a PNG and quit. Reliable on the flip-model swapchain.
//
// "--shot-count=<n> --shot-step=<sec>" turns it into a PHASE SERIES from ONE process:
// requires --wind-freeze, whose frozen clock this advances by exactly --shot-step via
// vfx::g_windStep_game between frames (path gets an _NN suffix). One boot instead of N —
// the previous way to film the shore breathing was N full relaunches. The inter-shot
// pause (--shot-interval) exists for the temporal stack: DLSS needs a few frames after
// each time jump or every frame in the series carries the previous phase's ghost.
//
// "--sweep=<setting>:<v0>,..." reuses the same series machinery for SETTINGS instead of
// time: value[i] is applied before shot i and the exposure adaptation is reset so each
// shot settles on its own value. One boot for a whole slider sweep.
class ShotHarness final : public Harness
{
public:
    // Applies one --sweep value by its --set name (App.cpp's ApplySweepValue); false = an unknown name.
    using ApplySetting = std::function<bool(Scene&, SceneRenderSettings&, const std::string&, float)>;
    explicit ShotHarness(ApplySetting apply) : apply_(std::move(apply)) {}
    const char* Name() const override { return "shot"; }
    bool Wanted() const override;
    void OnFrame(FrameContext& ctx) override;

private:
    ApplySetting apply_;
    bool started_ = false;
    double shotStart_ = 0.0;
    int shotIndex_ = 0;
    int appliedSweepIndex_ = -1;
};

// "--profdump=<path>": temporary VSM perf harness — after the same warmup delay, dump the
// profiler overlay to a file and quit. Independent of --shot so timings can be swept headlessly.
class ProfDumpHarness final : public Harness
{
public:
    const char* Name() const override { return "profdump"; }
    bool Wanted() const override;
    void OnFrame(FrameContext& ctx) override;

private:
    bool started_ = false;
    double start_ = 0.0;
};

// "--trace=<frames>": the same capture the CaptureTrace key requests, but headless.
// Without it a profiler change cannot be verified without driving the GUI — which is
// how an 82 us hole in the GPU timeline went unexplained for so long. Requests after
// the warmup delay, then keeps running until the capture has written itself out.
class TraceHarness final : public Harness
{
public:
    const char* Name() const override { return "trace"; }
    bool Wanted() const override;
    void OnFrame(FrameContext& ctx) override;

private:
    bool started_ = false;
    double start_ = 0.0;
    uint32_t framesLeft_ = 0;
    bool requested_ = false;
};
}
