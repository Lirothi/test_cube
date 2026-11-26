#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app/harness/Harness.h"

namespace harness
{
// "--settings-dump=<file>" (architecture plan R2.0, the move into the settings registry's gate): after the warmup
// (--shot-delay) every setting name of tools/settings_names.txt that can be read back is written to the file as
// `name value`, one a line, and the run ends. Two runs of one binary give the same file; a --set of one name changes
// its line alone; the dump before and after a move of a prefix into the registry is the same file.
class SettingsDumpHarness final : public Harness
{
public:
    // Reads a setting by its --set name (App.cpp's ReadSweepValue, then the registry); false = it cannot be read.
    using ReadSetting = std::function<bool(Scene&, const SceneRenderSettings&, const std::string&, float&)>;
    // The graphics file's lines (R2.n): GraphicsSettingsManager::DescribeFile, and after a Reset all when
    // --graphics-reset-at-dump asks (reset = true).
    using GraphicsLines = std::function<void(FrameContext&, bool reset, std::vector<std::string>&)>;
    SettingsDumpHarness(ReadSetting read, GraphicsLines graphics) : read_(std::move(read)), graphics_(std::move(graphics)) {}
    const char* Name() const override { return "settings-dump"; }
    bool Wanted() const override;
    void OnFrame(FrameContext& ctx) override;
    int Verdict() const override { return failed_ ? 2 : 0; }

private:
    ReadSetting read_;
    GraphicsLines graphics_;
    bool started_ = false;
    bool failed_ = false;
    double start_ = 0.0;
};
}
