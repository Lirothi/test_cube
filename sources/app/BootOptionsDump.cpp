#include "app/BootOptions.h"
#include "app/scene/SceneRenderQueue.h"
#include "ocean/OceanRenderable.h"
#include "ocean/OceanSimulationConfig.h"
#include "ocean/OceanSurfSim.h"
#include "rendering/core/CommandListBindState.h"
#include "rendering/core/FrameResource.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/VisibilityStats.h"
#include "rendering/lighting/VolumetricCloud.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/TessySettings.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/wind/WindState.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <type_traits>

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
// The dev switches' globals (architecture plan R3.2c): what the debug switches set outside BootOptions, written as
// dev.<name>=<value> -- the gate of moving their parsing out of WinMain into the switch table's rows.
template <class T>
std::string Dev(const T& v)
{
    if constexpr (std::is_same_v<T, bool>) { return Bool(v); }
    else if constexpr (std::is_enum_v<T>) { return Num(static_cast<double>(static_cast<std::underlying_type_t<T>>(v))); }
    else if constexpr (std::is_arithmetic_v<T>) { return Num(static_cast<double>(v)); }
    else
    {
        std::string out;
        for (const auto c : v) { out.push_back(static_cast<char>(c)); }
        return out;
    }
}
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
    line("harness.graphicsResetAtDump", Bool(h.graphicsResetAtDump));
    line("harness.graphicsResetSetting", h.graphicsResetSetting);
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

    // R3.2c: the dev switches' globals.
    line("dev.g_useFusedBucketizeCull", Dev(g_useFusedBucketizeCull));
    line("dev.ocean::g_foamDebugView_game", Dev(ocean::g_foamDebugView_game));
    line("dev.ocean::g_foamDebug_game", Dev(ocean::g_foamDebug_game));
    line("dev.ocean::g_geometryFadeOverride_game", Dev(ocean::g_geometryFadeOverride_game));
    line("dev.ocean::g_shoreRunup_game", Dev(ocean::g_shoreRunup_game));
    line("dev.ocean::g_shoreSinkCut_game", Dev(ocean::g_shoreSinkCut_game));
    line("dev.ocean::g_surfSimDebugView_game", Dev(ocean::g_surfSimDebugView_game));
    line("dev.ocean::g_surfSimForce_game", Dev(ocean::g_surfSimForce_game));
    line("dev.ocean::g_surfSimPokeInterval_game", Dev(ocean::g_surfSimPokeInterval_game));
    line("dev.ocean::g_vsDepthProbe_game", Dev(ocean::g_vsDepthProbe_game));
    line("dev.ocean::g_windForceOverride_game", Dev(ocean::g_windForceOverride_game));
    line("dev.render::g_asyncEmptySubmit_game", Dev(render::g_asyncEmptySubmit_game));
    line("dev.render::g_asyncOrderProbe_game", Dev(render::g_asyncOrderProbe_game));
    line("dev.render::g_asyncPassBuildAS_game", Dev(render::g_asyncPassBuildAS_game));
    line("dev.render::g_asyncPassCloudTrace_game", Dev(render::g_asyncPassCloudTrace_game));
    line("dev.render::g_asyncPassObjectCompute_game", Dev(render::g_asyncPassObjectCompute_game));
    line("dev.render::g_asyncPassRtTrace_game", Dev(render::g_asyncPassRtTrace_game));
    line("dev.render::g_barrierCacheVerify_game", Dev(render::g_barrierCacheVerify_game));
    line("dev.render::g_barrierComparator_game", Dev(render::g_barrierComparator_game));
    line("dev.render::g_barrierCompileLog_game", Dev(render::g_barrierCompileLog_game));
    line("dev.render::g_barrierFlipTrace_game", Dev(render::g_barrierFlipTrace_game));
    line("dev.render::g_bindBatchingEnabled_game", Dev(render::g_bindBatchingEnabled_game));
    line("dev.render::g_canonicalCheck_game", Dev(render::g_canonicalCheck_game));
    line("dev.render::g_computeLaneProbe_game", Dev(render::g_computeLaneProbe_game));
    line("dev.render::g_csmDebugMode_game", Dev(render::g_csmDebugMode_game));
    line("dev.render::g_csmDumpReadout_game", Dev(render::g_csmDumpReadout_game));
    line("dev.render::g_deviceRemovalCheck_game", Dev(render::g_deviceRemovalCheck_game));
    line("dev.render::g_dlssMvecMode_game", Dev(render::g_dlssMvecMode_game));
    line("dev.render::g_dumpBarriers_game", Dev(render::g_dumpBarriers_game));
    line("dev.render::g_dumpSubmitOrder_game", Dev(render::g_dumpSubmitOrder_game));
    line("dev.render::g_noAsyncCompute_game", Dev(render::g_noAsyncCompute_game));
    line("dev.render::g_noStreamline_game", Dev(render::g_noStreamline_game));
    line("dev.render::g_rtConstantsProbe", Dev(render::g_rtConstantsProbe));
    line("dev.render::g_shadowLodBias_game", Dev(render::g_shadowLodBias_game));
    line("dev.render::g_shadowLodTierStride_game", Dev(render::g_shadowLodTierStride_game));
    line("dev.render::g_shadowModeCliUnknown_game", Dev(render::g_shadowModeCliUnknown_game));
    line("dev.render::g_shadowModeFromCli_game", Dev(render::g_shadowModeFromCli_game));
    line("dev.render::g_shadowMode_game", Dev(render::g_shadowMode_game));
    line("dev.render::g_uploadRingCheck", Dev(render::g_uploadRingCheck));
    line("dev.render::g_uploadRingCheckSelftest", Dev(render::g_uploadRingCheckSelftest));
    line("dev.render::g_visDumpReadout_game", Dev(render::g_visDumpReadout_game));
    line("dev.render::sdsm::g_dumpReadout_game", Dev(render::sdsm::g_dumpReadout_game));
    line("dev.tessy::g_lodFold_game", Dev(tessy::g_lodFold_game));
    line("dev.tessy::g_lodPrune_game", Dev(tessy::g_lodPrune_game));
    line("dev.vfx::g_windFreeze_game", Dev(vfx::g_windFreeze_game));
    line("dev.vfx::g_windFrozenTime_game", Dev(vfx::g_windFrozenTime_game));
    line("dev.vsm::g_clipmapNormalBias_game", Dev(vsm::g_clipmapNormalBias_game));
    line("dev.vsm::g_pageDrawCompact_game", Dev(vsm::g_pageDrawCompact_game));
    line("dev.vsm::g_pageDrawSingle_game", Dev(vsm::g_pageDrawSingle_game));
    line("dev.vsm::g_refDist_game", Dev(vsm::g_refDist_game));
    line("dev.vsm::g_residentIterOnly_game", Dev(vsm::g_residentIterOnly_game));
    line("dev.vsm::g_scatterLocalViews_game", Dev(vsm::g_scatterLocalViews_game));
    line("dev.render::g_cloudProfilePath", Dev(render::g_cloudProfilePath));
    line("dev.render::g_cloudFormationPath", Dev(render::g_cloudFormationPath));
    line("dev.render::g_cloudCellsPath", Dev(render::g_cloudCellsPath));
    line("dev.render::g_cloudDumpDir", Dev(render::g_cloudDumpDir));
    line("dev.tessy::g_lodBuild", Dev(tessy::g_lodBuild));
    return static_cast<bool>(out);
}
