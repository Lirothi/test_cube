#include "app/BootOptions.h"

#include <cstdio>
#include <fstream>
#include <string>

// "--boot-dump=<file>" (architecture plan R3.2b): what the command line set, every field of BootOptions as `group.field=value`,
// one a line -- the gate of moving a switch's parsing: two builds given one command line write the same file. A field
// added to BootOptions.h belongs here too (tools/check_switches.py names one that is missing). bootDumpPath itself is
// not written: two runs compared name two files.
namespace
{
std::string Num(double v)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.9g", v);
    return text;
}

template <class T, std::size_t N>
std::string Nums(const T (&values)[N])
{
    std::string out;
    for (std::size_t i = 0; i < N; ++i)
    {
        out += (i ? " " : "") + Num(static_cast<double>(values[i]));
    }
    return out;
}

std::string Floats(const std::vector<float>& values)
{
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        out += (i ? " " : "") + Num(values[i]);
    }
    return out;
}

const char* Bool(bool v) { return v ? "1" : "0"; }
}

bool WriteBootDump(const std::string& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { return false; }
    const BootOptions& b = Boot();
    const auto line = [&](const char* name, const std::string& value) { out << name << '=' << value << '\n'; };

    const BootOptions::Frame& f = b.frame;
    line("frame.levelPath", f.levelPath);
    line("frame.camOverride", Bool(f.camOverride));
    line("frame.camPos", Nums(f.camPos));
    line("frame.camRot", Nums(f.camRot));
    line("frame.camFly", Nums(f.camFly));
    line("frame.camFlyDelay", Num(f.camFlyDelay));
    line("frame.camFlyYaw", Num(f.camFlyYaw));
    line("frame.camOrbit", Nums(f.camOrbit));
    line("frame.shotPath", f.shotPath);
    line("frame.shotDelaySec", Num(f.shotDelaySec));
    line("frame.shotCount", Num(f.shotCount));
    line("frame.shotStepSec", Num(f.shotStepSec));
    line("frame.shotIntervalSec", Num(f.shotIntervalSec));
    line("frame.profDumpPath", f.profDumpPath);
    line("frame.traceFrames", Num(f.traceFrames));
    line("frame.dlssMode", Num(f.dlssMode));
    line("frame.windowSize", Nums(f.windowSize));
    line("frame.sweepSetting", f.sweepSetting);
    line("frame.sweepValues", Floats(f.sweepValues));
    std::string extra;
    for (const auto& [name, values] : f.sweepExtra) { extra += name + ':' + Floats(values) + ';'; }
    line("frame.sweepExtra", extra);
    std::string fixed;
    for (const auto& [name, value] : f.fixedSettings) { fixed += name + ':' + Num(value) + ';'; }
    line("frame.fixedSettings", fixed);
    line("frame.hudHidden", Bool(f.hudHidden));
    line("frame.scalability", Num(f.scalability));

    const BootOptions::Harness& h = b.harness;
    line("harness.sceneStressExtraLevel", h.sceneStressExtraLevel);
    line("harness.intentPhrase", h.intentPhrase);
    line("harness.intentRun", Bool(h.intentRun));
    line("harness.intentFinished", Bool(h.intentFinished));
    line("harness.intentTimeoutSec", Num(h.intentTimeoutSec));
    line("harness.intentRepeat", Num(h.intentRepeat));
    line("harness.chatPhrase", h.chatPhrase);
    line("harness.chatThenPhrase", h.chatThenPhrase);
    line("harness.scalabilityCheck", Bool(h.scalabilityCheck));
    line("harness.scalabilityCheckExit", Num(h.scalabilityCheckExit));
    line("harness.settingsDumpPath", h.settingsDumpPath);
    line("harness.sceneStress", Bool(h.sceneStress));
    line("harness.sceneStressIterations", Num(h.sceneStressIterations));
    line("harness.sceneStressGbv", Bool(h.sceneStressGbv));
    line("harness.sceneStressRoughness", Bool(h.sceneStressRoughness));
    line("harness.sceneStressSky", Bool(h.sceneStressSky));
    line("harness.sceneStressExit", Num(h.sceneStressExit));

    const BootOptions::Editor& e = b.editor;
    line("editor.logWindow", Bool(e.logWindow));
    line("editor.memoryWindow", Bool(e.memoryWindow));
    line("editor.devTab", e.devTab);
    line("editor.mcpPort", Num(e.mcpPort));
    line("editor.openEditor", Bool(e.openEditor));
    line("editor.play", Bool(e.play));
    line("editor.editMesh", e.editMesh);
    line("editor.showImport", Bool(e.showImport));
    line("editor.importItem", e.importItem);
    line("editor.inspect", e.inspect);
    line("editor.settingsMenu", e.settingsMenu);

    const BootOptions::Dist& d = b.dist;
    line("dist.buildDist", Bool(d.buildDist));
    line("dist.buildDistProfile", d.buildDistProfile);
    line("dist.playByLevelRule", Bool(d.playByLevelRule));
    return static_cast<bool>(out);
}
