#include "app/harness/GateHarnesses.h"

#include "app/BootOptions.h"
#include "app/diagnostics/RtAudit.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "app/settings/Scalability.h"
#include "core/Helpers.h" // GetTimeSeconds
#include "core/logging/Log.h"
#include "rhi/MemGates.h"
#include "rendering/core/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace harness
{
void MemGateHarness::OnFrame(FrameContext& ctx)
{
    // Riding a --scene-stress, the readout's end does not end the churn (the stress's own loop never read it).
    if (memgate::OnFrameEnd(ctx.renderer.GetDevice(), GetTimeSeconds()) && !Boot().harness.sceneStress)
    {
        ctx.running = false; // the readout's verdict is in; main returns its exit code
    }
}

int MemGateHarness::Verdict() const { return memgate::ExitCode(); }

void RtAuditHarness::OnFrame(FrameContext& ctx)
{
    // Riding a --scene-stress, the audit ends with the churn (the stress's Final_ calls Finished(now)), not on its clock.
    if (Boot().harness.sceneStress) { return; }
    if (rtaudit::Finished(GetTimeSeconds()))
    {
        ctx.running = false; // the audit's verdict is in; main returns its exit code
    }
}

int RtAuditHarness::Verdict() const { return rtaudit::ExitCode(); }

namespace
{
    // The knob reader the check was given (ScalabilityCheckHarness): the moved code below calls it by its old name.
    ScalabilityCheckHarness::ReadSetting g_readSetting;
    bool ReadSweepValue(Scene& scene, const SceneRenderSettings& rs, const std::string& name, float& out)
    {
        return g_readSetting && g_readSetting(scene, rs, name, out);
    }
}

// "--scalability-check" (docs/scalability_plan.md §8.1, §8.2): in the running game, from an unmanaged start with the
// level's own values, every level (Low .. Ultra, the render resolution with it) is applied and every knob of every
// group read back against what the table asks -- a ceiling against the level's own value; then the level is loaded
// again at Low and the ceilings must hold over the reloaded values; then the start is restored. One line per
// mismatch and the verdict `scalability check: PASS|FAIL` in the session log. Returns true when it is done.
struct ScalabilityCheck
{
    int frame = 0;
    int phase = 0;
    int waitUntil = 0;
    int loadsAtReload = 0;
    int waitedForLoad = 0;
    int failed = 0;
    int checked = 0;
    std::vector<int> savedGroups;
    int savedResolution = -1;
    std::map<std::string, float> own; // the level's own value of every content knob, and every knob's start value
};

void ScalabilityCheckCell(ScalabilityCheck& c, Scene& scene, const SceneRenderSettings& rs, const settings::ScalabilityCell& cell,
                          const std::string& where)
{
    const settings::Scalability& s = settings::TheScalability();
    for (const auto& [name, op] : cell)
    {
        if (s.Pins().count(name)) { continue; }
        float expected = op.value;
        const auto own = c.own.find(name);
        const float base = own != c.own.end() ? own->second : 0.0f;
        switch (op.kind)
        {
        case settings::ScalabilityOp::Kind::Level: expected = base; break;
        case settings::ScalabilityOp::Kind::Max: expected = std::min(base, op.value); break;
        case settings::ScalabilityOp::Kind::Min: expected = std::max(base, op.value); break;
        default: break;
        }
        float actual = 0.0f;
        ++c.checked;
        if (!ReadSweepValue(scene, rs, name, actual))
        {
            ++c.failed;
            LOG_ERROR(logging::LogCategory::App, "scalability check: {} {}: cannot be read", where, name);
        }
        else if (std::abs(actual - expected) > 1e-3f * std::max(1.0f, std::abs(expected)))
        {
            ++c.failed;
            LOG_ERROR(logging::LogCategory::App, "scalability check: {} {}: reads {} where the table asks {}{}", where, name,
                      actual, expected, op.kind == settings::ScalabilityOp::Kind::Set ? "" : " (over the level's own value)");
        }
    }
}

bool StepScalabilityCheck(ScalabilityCheck& c, Scene& scene, SceneRenderSettings& rs, Renderer& renderer, LevelManager& levels)
{
    settings::Scalability& s = settings::TheScalability();
    ++c.frame;
    if (c.frame < c.waitUntil) { return false; }
    const auto verify = [&](int level, const std::string& where)
    {
        for (const settings::ScalabilityGroup& g : s.Table().groups)
        {
            ScalabilityCheckCell(c, scene, rs, g.levels[static_cast<std::size_t>(level)], where + " sg." + g.key);
        }
        const auto& resolution = renderer.IsDlssAvailable() ? s.Table().resolutionDlss : s.Table().resolutionNative;
        ScalabilityCheckCell(c, scene, rs, resolution[static_cast<std::size_t>(level)], where + " sg.resolution");
    };
    const auto applyLevel = [&](int level, const std::string& reason)
    {
        s.SetPreset(level);
        s.SetResolutionLevel(level);
        s.Apply(reason);
        c.waitUntil = c.frame + 8; // dlss.mode and aa.renderScale land at the next frame's safe point
    };
    if (c.phase == 0)
    {
        if (c.frame < 60) { return false; } // the level settles first
        if (!s.Loaded())
        {
            LOG_ERROR(logging::LogCategory::App, "scalability check: FAIL -- the table did not load");
            Boot().harness.scalabilityCheckExit = 1;
            return true;
        }
        for (std::size_t g = 0; g < s.Table().groups.size(); ++g) { c.savedGroups.push_back(s.GroupLevel(static_cast<int>(g))); }
        c.savedResolution = s.ResolutionLevel();
        // An unmanaged start: what the level asks for, before any ceiling. A boot that applied the player's saved
        // choice capped the level's values; loading the level again gives them back.
        s.SetPreset(-1);
        s.SetResolutionLevel(-1);
        c.loadsAtReload = g_levelLoadsCompleted;
        levels.RequestLevelPathChange(std::string(levels.GetActiveLevelSourcePath()));
        c.phase = 1;
        return false;
    }
    if (c.phase == 1)
    {
        if (g_levelLoadsCompleted == c.loadsAtReload)
        {
            if (++c.waitedForLoad < 3000) { return false; } // a failed or refused reload: a FAIL, not a run that never ends
            LOG_ERROR(logging::LogCategory::App, "scalability check: FAIL -- the level reload did not finish in 3000 frames");
            Boot().harness.scalabilityCheckExit = std::max(c.failed, 1);
            return true;
        }
        c.waitedForLoad = 0;
        if (c.waitUntil == 0) { c.waitUntil = c.frame + 30; return false; }
        int differ = 0;
        for (const settings::ScalabilityKnob& k : settings::ScalabilityKnobs())
        {
            float v = 0.0f;
            if (ReadSweepValue(scene, rs, k.name, v)) { c.own[k.name] = v; }
        }
        // Ultra = today (§1 item 2): the Ultra column against the values the build starts with.
        for (const settings::ScalabilityGroup& g : s.Table().groups)
        {
            for (const auto& [name, op] : g.levels[settings::kScalabilityLevels - 1])
            {
                if (op.kind != settings::ScalabilityOp::Kind::Set) { continue; }
                const float today = c.own[name];
                if (std::abs(today - op.value) > 1e-3f * std::max(1.0f, std::abs(op.value)))
                {
                    ++differ;
                    LOG_INFO(logging::LogCategory::App, "scalability check: Ultra differs from today: sg.{} {} today {} Ultra {}",
                             g.key, name, today, op.value);
                }
            }
        }
        LOG_INFO(logging::LogCategory::App, "scalability check: Ultra against today's values: {} knob(s) differ", differ);
        applyLevel(0, "check Low");
        c.phase = 2;
        return false;
    }
    if (c.phase >= 2 && c.phase <= 5)
    {
        const int level = c.phase - 2;
        verify(level, s.Table().levelNames[static_cast<std::size_t>(level)]);
        if (level < settings::kScalabilityLevels - 1)
        {
            applyLevel(level + 1, "check " + s.Table().levelNames[static_cast<std::size_t>(level + 1)]);
            ++c.phase;
            return false;
        }
        // The level loaded again at Low: the ceilings must hold over its reloaded values.
        applyLevel(0, "check Low before a reload");
        c.loadsAtReload = g_levelLoadsCompleted;
        c.phase = 6;
        return false;
    }
    if (c.phase == 6)
    {
        c.loadsAtReload = g_levelLoadsCompleted;
        levels.RequestLevelPathChange(std::string(levels.GetActiveLevelSourcePath()));
        c.phase = 7;
        return false;
    }
    if (c.phase == 7)
    {
        if (g_levelLoadsCompleted == c.loadsAtReload)
        {
            if (++c.waitedForLoad < 3000) { return false; } // a failed or refused reload: a FAIL, not a run that never ends
            LOG_ERROR(logging::LogCategory::App, "scalability check: FAIL -- the level reload did not finish in 3000 frames");
            Boot().harness.scalabilityCheckExit = std::max(c.failed, 1);
            return true;
        }
        c.waitedForLoad = 0;
        c.waitUntil = c.frame + 30;
        c.phase = 8;
        return false;
    }
    if (c.phase == 8)
    {
        verify(0, "Low after a level reload");
        // "Not managed" puts every knob back (the menu's Cancel, the tab's Not managed): to the start's values.
        s.SetPreset(-1);
        s.SetResolutionLevel(-1);
        s.Apply("check: not managed again");
        for (const auto& [name, start] : c.own)
        {
            float now = 0.0f;
            ++c.checked;
            if (!ReadSweepValue(scene, rs, name, now) || std::abs(now - start) > 1e-3f * std::max(1.0f, std::abs(start)))
            {
                ++c.failed;
                LOG_ERROR(logging::LogCategory::App, "scalability check: not managed again: {} reads {} where it started at {}",
                          name, now, start);
            }
        }
        for (std::size_t g = 0; g < c.savedGroups.size(); ++g) { s.SetGroupLevel(static_cast<int>(g), c.savedGroups[g]); }
        s.SetResolutionLevel(c.savedResolution);
        s.Apply("check done: the start restored");
        Boot().harness.scalabilityCheckExit = c.failed;
        LOG_INFO(logging::LogCategory::App, "scalability check: {} -- {} reads, {} failed", c.failed == 0 ? "PASS" : "FAIL",
                 c.checked, c.failed);
        return true;
    }
    return true;
}

ScalabilityCheckHarness::ScalabilityCheckHarness(ReadSetting read) : check_(std::make_unique<ScalabilityCheck>())
{
    g_readSetting = std::move(read);
}

ScalabilityCheckHarness::~ScalabilityCheckHarness() = default;

bool ScalabilityCheckHarness::Wanted() const { return Boot().harness.scalabilityCheck; }

void ScalabilityCheckHarness::OnFrame(FrameContext& ctx)
{
    if (StepScalabilityCheck(*check_, ctx.scene, ctx.settings, ctx.renderer, ctx.levelManager))
    {
        Boot().harness.scalabilityCheck = false;
        ctx.running = false;
    }
}

int ScalabilityCheckHarness::Verdict() const { return Boot().harness.scalabilityCheckExit; }
}
