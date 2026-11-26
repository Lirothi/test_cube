#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "app/Systems.h"
#include "core/settings/SettingsRegistry.h"
#include "ocean/OceanRenderable.h"
#include "ocean/OceanSimulation.h"
#include "ocean/OceanSimulationConfig.h"
#include "ocean/OceanWakeKnobs.h"
#include "ocean/WaterMask.h"
#include "rendering/core/Renderer.h"
#include "vfx/ParticleTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// --set / --sweep / MCP set_settings names ocean.* and spray.* (the ocean surface, the wakes' knobs, the spray)
// (architecture plan R2.2). Each row is the branch App.cpp's ApplySweepValue had, its clamp included, and the reader
// ReadSweepValue had (R2.2 gave every simple name one first, so --settings-dump compared the move); a row without a
// reader had none.
namespace settings
{
namespace
{
// The ocean's render config, changed and handed back -- what App.cpp's spray. and ocean.wake blocks did. Without an
// ocean (or its simulation) the name is taken and nothing happens, as there.
template <class Edit>
bool EditOceanConfig(SettingContext& c, Edit&& edit)
{
    OceanRenderable* ocean = c.scene.FindOceanRenderable();
    OceanSimulation* sim = ocean ? ocean->GetSimulation() : nullptr;
    if (!sim) { return true; }
    OceanRenderConfig cfg = sim->GetRenderConfig();
    edit(cfg);
    sim->SetRenderConfig(cfg);
    return true;
}

const OceanRenderConfig* OceanConfig(SettingContext& c)
{
    const OceanRenderable* ocean = c.scene.FindOceanRenderable();
    const OceanSimulation* sim = ocean ? ocean->GetSimulation() : nullptr;
    return sim ? &sim->GetRenderConfig() : nullptr;
}

// The sun's glint on the water (OceanSunSpecular): its brightness multiplier and the unresolved roughness scale (the
// Bruneton slope variance it takes its GGX alpha from), and how much of that variance the glint takes (sunGlintSmear:
// 0 = the glitter path alone, 1 = the broad GGX patch). One branch answered the three.
bool SetGlint(SettingContext& c, std::string_view setting, float value)
{
    if (OceanRenderable* ocean = c.scene.FindOceanRenderable())
    {
        if (OceanSimulation* sim = ocean->GetSimulation())
        {
            OceanRenderConfig cfg = sim->GetRenderConfig();
            if (setting == "ocean.specularStrength") { cfg.specularStrength = std::clamp(value, 0.0f, 100.0f); }
            else if (setting == "ocean.sunGlintSmear") { cfg.sunGlintSmear = Math::Saturate(value); }
            else { cfg.roughnessScale = Math::Saturate(value); }
            sim->SetRenderConfig(cfg);
        }
    }
    return true;
}

// The water's colours by component, to tune them live against a reference instead of a level edit and a relaunch:
// `ocean.deepScatterG`, `ocean.diffuseB`, `ocean.sssR`, `ocean.absorption<key 0-3><R|G|B>` (the gradient key's position
// .w stays). OceanRenderConfig's deepScatterColor / diffuseColor / sssColor / absorptionColors -- what the Ocean
// controls' colour pickers edit. `key` is the name past "ocean.".
bool IsOceanColour(std::string_view key)
{
    if (key.size() < 2) { return false; }
    const char last = key.back();
    const int ch = last == 'R' ? 0 : last == 'G' ? 1 : last == 'B' ? 2 : -1;
    const std::string_view stem = key.substr(0, key.size() - 1);
    const bool absorption = stem.size() == 11 && stem.substr(0, 10) == "absorption" && stem[10] >= '0' && stem[10] <= '3';
    return ch >= 0 && (stem == "deepScatter" || stem == "diffuse" || stem == "sss" || absorption);
}

// The colour a key names in a config, and its component (0 R, 1 G, 2 B); null for an absorption key past the gradient.
template <class Config>
auto OceanColour(Config& cfg, std::string_view key, int& ch) -> decltype(&cfg.deepScatterColor)
{
    const char last = key.back();
    ch = last == 'R' ? 0 : last == 'G' ? 1 : 2;
    const std::string_view stem = key.substr(0, key.size() - 1);
    return stem == "deepScatter" ? &cfg.deepScatterColor
         : stem == "diffuse"     ? &cfg.diffuseColor
         : stem == "sss"         ? &cfg.sssColor
         : (static_cast<size_t>(stem[10] - '0') < cfg.absorptionColors.size()
                ? &cfg.absorptionColors[static_cast<size_t>(stem[10] - '0')] : nullptr);
}

const ocean::WakeKnob* FindWakeKnob(std::string_view key)
{
    for (const ocean::WakeKnob& knob : ocean::WakeKnobs())
    {
        if (key == knob.key) { return &knob; }
    }
    return nullptr;
}
const SettingDesc kOceanRows[] = {
    // The boats' spray, capped at runtime over the ocean's own rate and mist (the Effects group): never written into
    // the ocean's config, which the editor saves into the level.
    TC_SETTING("spray.rateMax", Float, SettingTransient | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { vfx::g_sprayRateMax = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(vfx::g_sprayRateMax); return true; }),
    TC_SETTING("spray.mistMax", Float, SettingTransient | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { vfx::g_sprayMistMax = std::clamp(value, 0.0f, 4.0f); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(vfx::g_sprayMistMax); return true; }),
    // P16.3b: the sky's horizontal illuminance in lux. 0 = un-authored, the legacy behaviour.
    // Shore contact foam, as a CAPTURE switch rather than a level edit: it is the thing that
    // litters a lighting comparison with moving white speckle, and turning it off in the level
    // would mean editing content that was tuned on purpose. 0 = off.
    // The whole ocean surface, as a CAPTURE switch (same rationale as contactFoam below):
    // shadow/terrain artifacts live on dune slopes the water covers, and the user's repro
    // screenshots are taken with the ocean hidden. 0 = hidden.
    TC_SETTING("ocean.visible", Float, SettingTransient,
               [](SettingContext& c, float value) { c.scene.SetOceanVisible(value != 0.0f); return true; },
               nullptr),
    // The sun's shadow from geometry on the water (OceanSunShadow): 0 = off (the A/B); its VSM SMRT rays, 0 = one tap.
    TC_SETTING("ocean.sunShadow", Bool, SettingTransient,
               [](SettingContext&, float value) { ocean::g_sunShadow_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(ocean::g_sunShadow_game); return true; }),
    TC_SETTING("ocean.sunShadowRays", Int, SettingTransient | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { ocean::g_sunShadowRays_game = std::clamp(static_cast<int>(value), 0, 16); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(ocean::g_sunShadowRays_game); return true; }),
    TC_SETTING("ocean.sunShadowBody", Float, SettingTransient,
               [](SettingContext&, float value) { ocean::g_sunShadowBody_game = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(ocean::g_sunShadowBody_game); return true; }),
    // The water kept out of the marked hulls (ocean/WaterMask.h): 0 = drawn inside them, the A/B.
    TC_SETTING("ocean.waterMask", Int, SettingTransient,
               [](SettingContext&, float value) { ocean::g_waterMask_game = std::clamp(static_cast<int>(value), 0, 3); return true; },
               [](SettingContext&, float& out) { out = AsFloat(ocean::g_waterMask_game); return true; }),
    TC_SETTING("spray.sidePush", Bool, SettingTransient,
               [](SettingContext&, float value) { vfx::g_spraySidePush_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(vfx::g_spraySidePush_game); return true; }),
    // The water reflection's normal filter in mips (SceneRenderSettings::waterReflectionNormalFilter); 0 = the shading normal.
    TC_SETTING("ocean.reflectionNormalFilter", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.waterReflectionNormalFilter = std::clamp(value, 0.0f, 4.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.waterReflectionNormalFilter); return true; }),
    // The water reflection's prefilter (water_composite_cs): sigma across the columns in render px, 0 = off.
    TC_SETTING("ocean.reflectionPrefilter", Float, SettingPersistent,
               [](SettingContext& c, float value) { c.renderSettings.waterReflectionPrefilter = std::clamp(value, 0.0f, 3.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.waterReflectionPrefilter); return true; }),
    // Sky-reflection horizon pull (OceanSkyReflectDir; 1 = off). Here so the streak A/B is a
    // command line rather than a level edit — same one-binary discipline as the VSM levers.
    TC_SETTING("ocean.skyHorizonPull", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (OceanRenderable* ocean = c.scene.FindOceanRenderable())
                   {
                       if (OceanSimulation* sim = ocean->GetSimulation())
                       {
                           OceanRenderConfig cfg = sim->GetRenderConfig();
                           cfg.reflectionSkyHorizonPull = Math::Clamp(value, 0.05f, 1.0f);
                           sim->SetRenderConfig(cfg);
                       }
                   }
                   return true;
               },
               nullptr),
    // How far the water's reflection normal is pulled toward +Y (OceanRenderConfig::
    // reflectionNormalStrength; 1 = a flat mirror) -- the sky sample's AND, since the ocean G-buffer
    // plan's S3, the screen-space trace's, so this is the A/B for that look.
    TC_SETTING("ocean.reflectionNormalStrength", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (OceanRenderable* ocean = c.scene.FindOceanRenderable())
                   {
                       if (OceanSimulation* sim = ocean->GetSimulation())
                       {
                           OceanRenderConfig cfg = sim->GetRenderConfig();
                           cfg.reflectionNormalStrength = Math::Clamp(value, 0.0f, 1.0f);
                           sim->SetRenderConfig(cfg);
                       }
                   }
                   return true;
               },
               nullptr),
    // A boat's wake (ocean/OceanWake): the preset's knobs, and the debug view (the height, opaque).
    TC_SETTING("ocean.wakeDebug", Int, SettingTransient,
               [](SettingContext&, float value) { ocean::g_wakeDebugView_game = std::clamp(static_cast<int>(std::lround(value)), 0, 2); return true; },
               [](SettingContext&, float& out) { out = AsFloat(ocean::g_wakeDebugView_game); return true; }),
    TC_SETTING("ocean.wakeShore", Float, SettingTransient,
               [](SettingContext&, float value) { ocean::g_wakeShore_game = value != 0.0f ? 1 : 0; return true; },
               [](SettingContext&, float& out) { out = AsFloat(ocean::g_wakeShore_game); return true; }),
    TC_SETTING("ocean.wakeHz", Float, SettingTransient | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { ocean::g_wakeStepHz_game = std::max(value, 0.0f); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(ocean::g_wakeStepHz_game); return true; }),
    TC_SETTING("ocean.wakeBowBreaks", Bool, SettingTransient,
               [](SettingContext&, float value) { ocean::g_wakeBowBreaks_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(ocean::g_wakeBowBreaks_game); return true; }),
    TC_SETTING("ocean.wakeExtrapolate", Bool, SettingTransient,
               [](SettingContext&, float value) { ocean::g_wakeExtrapolate_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(ocean::g_wakeExtrapolate_game); return true; }),
    // The surf sim's wave height on the surface (OceanRenderConfig::surfSimDisplacement, 0 = the sim runs, the water does
    // not rise by it): the A/B of what the shore's breakers do to what floats -- OceanReadback adds the same height.
    TC_SETTING("ocean.surfDisplacement", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (OceanRenderable* ocean = c.scene.FindOceanRenderable())
                   {
                       if (OceanSimulation* sim = ocean->GetSimulation())
                       {
                           OceanRenderConfig cfg = sim->GetRenderConfig();
                           cfg.surfSimDisplacement = std::clamp(value, 0.0f, 2.0f);
                           sim->SetRenderConfig(cfg);
                       }
                   }
                   return true;
               },
               nullptr),
    // The water body's floor under a low sun (OceanRenderConfig::lowSunBodyLight), for a headless
    // sweep at a sunset elevation.
    TC_SETTING("ocean.lowSunBodyLight", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (OceanRenderable* ocean = c.scene.FindOceanRenderable())
                   {
                       if (OceanSimulation* sim = ocean->GetSimulation())
                       {
                           OceanRenderConfig cfg = sim->GetRenderConfig();
                           cfg.lowSunBodyLight = Math::Saturate(value);
                           sim->SetRenderConfig(cfg);
                       }
                   }
                   return true;
               },
               nullptr),
    // The sea state live (the level's ocean.windForce, 0..1 across the preset's keys -- what the Inspector's "Wind
    // Force" slider sets), to pick it on a view; `--ocean-wind=<v>` is the boot-time override.
    TC_SETTING("ocean.windForce", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (OceanRenderable* ocean = c.scene.FindOceanRenderable())
                   {
                       if (OceanSimulation* sim = ocean->GetSimulation())
                       {
                           sim->SetSceneVariables(&Systems::GetRenderer(), sim->GetLocalWindDirectionDegrees(),
                                                  sim->GetSwellDirectionDegrees(), Math::Saturate(value));
                       }
                   }
                   return true;
               },
               nullptr),
    // The ocean preset's readbackCascades (0 None, 1 One, 2 Two), applied through the same
    // SetSettings the Ocean controls combo calls. Its point is the LIVE switch: a sweep
    // `ocean.readbackCascades:1,0,1` adopts a surface, drops it, adopts again -- the case a
    // cold start in None never reaches (docs/bug_review_2026-10-01.md, BUG-05).
    TC_SETTING("ocean.readbackCascades", Int, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (OceanRenderable* ocean = c.scene.FindOceanRenderable())
                   {
                       if (OceanSimulation* sim = ocean->GetSimulation())
                       {
                           using Mode = OceanSimulationSettings::ReadbackCascadesMode;
                           const int cascades = std::clamp(static_cast<int>(value), 0, 2);
                           OceanSimulationSettings settings = sim->GetSettings();
                           settings.SetReadbackMode(cascades == 2 ? Mode::Two : cascades == 1 ? Mode::One : Mode::None);
                           sim->SetSettings(&Systems::GetRenderer(), settings);
                       }
                   }
                   return true;
               },
               nullptr),
    TC_SETTING("ocean.contactFoam", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (OceanRenderable* ocean = c.scene.FindOceanRenderable())
                   {
                       if (OceanSimulation* sim = ocean->GetSimulation())
                       {
                           OceanRenderConfig cfg = sim->GetRenderConfig();
                           cfg.shoreContactFoamOpacity = value;
                           cfg.shoreLegacyContactFoamStrength *= (value > 0.0f) ? 1.0f : 0.0f;
                           sim->SetRenderConfig(cfg);
                       }
                   }
                   return true;
               },
               nullptr),
    TC_SETTING("spray.enabled", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayEnabled = value != 0.0f; }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayEnabled); return true; }),
    TC_SETTING("spray.rate", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayRate = std::clamp(value, 0.0f, 1.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayRate); return true; }),
    TC_SETTING("spray.mist", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayMist = std::clamp(value, 0.0f, 4.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayMist); return true; }),
    TC_SETTING("spray.out", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayOut = std::clamp(value, 0.0f, 2.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayOut); return true; }),
    TC_SETTING("spray.up", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayUp = std::clamp(value, 0.0f, 2.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayUp); return true; }),
    TC_SETTING("spray.forward", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayForward = std::clamp(value, -1.0f, 1.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayForward); return true; }),
    TC_SETTING("spray.slam", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.spraySlam = std::clamp(value, 0.0f, 4.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->spraySlam); return true; }),
    TC_SETTING("spray.colorR", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayColor.x = std::clamp(value, 0.0f, 1.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayColor.x); return true; }),
    TC_SETTING("spray.colorG", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayColor.y = std::clamp(value, 0.0f, 1.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayColor.y); return true; }),
    TC_SETTING("spray.colorB", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayColor.z = std::clamp(value, 0.0f, 1.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayColor.z); return true; }),
    TC_SETTING("spray.forwardScatter", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayForwardScatter = std::clamp(value, 0.0f, 1.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayForwardScatter); return true; }),
    TC_SETTING("spray.opacity", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayOpacity = std::clamp(value, 0.0f, 3.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayOpacity); return true; }),
    TC_SETTING("spray.softFade", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.spraySoftFade = std::clamp(value, 0.0f, 2.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->spraySoftFade); return true; }),
    TC_SETTING("spray.foamLight", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.sprayFoamLight = std::clamp(value, 0.0f, 1.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->sprayFoamLight); return true; }),
    TC_SETTING("ocean.wakeEnabled", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.wakeEnabled = value != 0.0f; }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->wakeEnabled); return true; }),
    TC_SETTING("ocean.wakeStrength", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.wakeStrength = std::clamp(value, 0.0f, 4.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->wakeStrength); return true; }),
    TC_SETTING("ocean.wakeDisplacement", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.wakeDisplacement = std::clamp(value, 0.0f, 2.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->wakeDisplacement); return true; }),
    TC_SETTING("ocean.wakeNormal", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.wakeNormal = std::clamp(value, 0.0f, 3.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->wakeNormal); return true; }),
    TC_SETTING("ocean.wakeDamping", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.wakeDamping = std::clamp(value, 0.0f, 20.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->wakeDamping); return true; }),
    TC_SETTING("ocean.wakeBreaking", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.wakeBreaking = std::clamp(value, 0.0f, 20.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->wakeBreaking); return true; }),
    TC_SETTING("ocean.wakeTexel", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.wakeTexel = std::clamp(value, 0.25f, 2.0f); }); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = AsFloat(cfg->wakeTexel); return true; }),
    TC_SETTING("ocean.specularStrength", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return SetGlint(c, "ocean.specularStrength", value); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = cfg->specularStrength; return true; }),
    TC_SETTING("ocean.roughnessScale", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return SetGlint(c, "ocean.roughnessScale", value); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = cfg->roughnessScale; return true; }),
    TC_SETTING("ocean.sunGlintSmear", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { return SetGlint(c, "ocean.sunGlintSmear", value); },
               [](SettingContext& c, float& out) { const OceanRenderConfig* cfg = OceanConfig(c); if (!cfg) { return false; } out = cfg->sunGlintSmear; return true; }),
};
TC_SETTINGS(kOceanRows);

const SettingFamily kOceanFamilies[] = {
    // The water's colours by component (IsOceanColour above): ocean.<deepScatter|diffuse|sss|absorption0-3><R|G|B>.
    TC_SETTING_FAMILY("ocean.", "ocean colours", Float, SettingLevelOwned,
                      [](std::string_view key) { return IsOceanColour(key); },
                      [](SettingContext& c, std::string_view key, float value)
                      {
                          if (OceanRenderable* ocean = c.scene.FindOceanRenderable())
                          {
                              if (OceanSimulation* sim = ocean->GetSimulation())
                              {
                                  OceanRenderConfig cfg = sim->GetRenderConfig();
                                  int ch = 0;
                                  if (Math::float4* color = OceanColour(cfg, key, ch))
                                  {
                                      (ch == 0 ? color->x : ch == 1 ? color->y : color->z) = std::max(value, 0.0f);
                                      sim->SetRenderConfig(cfg);
                                  }
                              }
                          }
                          return true;
                      },
                      [](SettingContext& c, std::string_view key, float& out)
                      {
                          const OceanRenderConfig* cfg = OceanConfig(c);
                          if (!cfg) { return false; }
                          int ch = 0;
                          const Math::float4* color = OceanColour(*cfg, key, ch);
                          if (!color) { return false; }
                          out = ch == 0 ? color->x : ch == 1 ? color->y : color->z;
                          return true;
                      },
                      [](std::vector<std::string>& out)
                      {
                          for (const char* stem : { "deepScatter", "diffuse", "sss", "absorption0", "absorption1", "absorption2", "absorption3" })
                          {
                              for (const char* ch : { "R", "G", "B" }) { out.push_back(std::string(stem) + ch); }
                          }
                      }),
    // A boat's wake (ocean/OceanWake): the foam and the bow wave's arms -- ocean/OceanWakeKnobs.h is their one list.
    TC_SETTING_FAMILY("ocean.", "ocean::WakeKnobs", Float, SettingLevelOwned,
                      [](std::string_view key) { return FindWakeKnob(key) != nullptr; },
                      [](SettingContext& c, std::string_view key, float value)
                      {
                          const ocean::WakeKnob* knob = FindWakeKnob(key);
                          return EditOceanConfig(c, [&](OceanRenderConfig& cfg) { cfg.*knob->field = std::clamp(value, knob->min, knob->max); });
                      },
                      [](SettingContext& c, std::string_view key, float& out)
                      {
                          const OceanRenderConfig* cfg = OceanConfig(c);
                          if (!cfg) { return false; }
                          out = AsFloat(cfg->*FindWakeKnob(key)->field);
                          return true;
                      },
                      [](std::vector<std::string>& out) { for (const ocean::WakeKnob& knob : ocean::WakeKnobs()) { out.emplace_back(knob.key); } }),
};
TC_SETTING_FAMILIES(kOceanFamilies);
}
}
