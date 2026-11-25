#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/harness/Harness.h"

// The gate harnesses (architecture plan R3.3): moved out of App::Run's after-frame step.
namespace harness
{
// Levels the loop has finished loading (loadPendingLevel), so the scalability check knows when a reload is in.
inline int g_levelLoadsCompleted = 0;

// "--mem-readout" / "--mem-selftest" (rendering/core/MemGates.h): every frame the self-test's leak and the readout's
// sample; the readout's verdict ends the run, its failing series are main's exit code.
class MemGateHarness final : public Harness
{
public:
    const char* Name() const override { return "mem-readout"; }
    bool Wanted() const override { return true; } // memgate does nothing unless a switch configured it
    void OnFrame(FrameContext& ctx) override;
    int Verdict() const override;
};

// "--rt-audit" (app/diagnostics/RtAudit.h): its verdict ends the run; the moved fields are main's exit code. The audit's
// probes (the jitter, the frozen hashes) stay where they look, in App::Run's loop.
class RtAuditHarness final : public Harness
{
public:
    const char* Name() const override { return "rt-audit"; }
    bool Wanted() const override { return true; } // rtaudit::Finished is false unless --rt-audit configured it
    void OnFrame(FrameContext& ctx) override;
    int Verdict() const override;
};

struct ScalabilityCheck;

// "--scalability-check" (docs/scalability_plan.md §8.1, §8.2): one step a frame; its verdict ends the run, its failed
// reads are main's exit code (Boot().harness.scalabilityCheckExit).
class ScalabilityCheckHarness final : public Harness
{
public:
    // Reads a knob by its --set name (App.cpp's ReadSweepValue); false = it cannot be read.
    using ReadSetting = std::function<bool(Scene&, const SceneRenderSettings&, const std::string&, float&)>;
    explicit ScalabilityCheckHarness(ReadSetting read);
    ~ScalabilityCheckHarness() override;
    const char* Name() const override { return "scalability-check"; }
    bool Wanted() const override;
    void OnFrame(FrameContext& ctx) override;
    int Verdict() const override;

private:
    std::unique_ptr<ScalabilityCheck> check_;
};
}
