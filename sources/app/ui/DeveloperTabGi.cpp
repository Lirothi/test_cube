// R10.4: Developer Controls > GI (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <algorithm>
#include <cstdint>
#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "rendering/scene/SkyOcclusionMap.h" // the sky-from-above map's ranges and coverage
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/visibility/OcclusionHistory.h" // S3a: vis::g_occlusion_game
#include "rendering/shadows/ShadowSettings.h"

void DeveloperWindow::DrawGiTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    // Interior lighting plan D: the bounce (SceneRenderSettings::gi*). The rest of its knobs
    // (gi.divisor, gi.rayDistance, gi.infiniteBounce) stay --set / MCP set_settings only.
    ImGui::TextWrapped("The bounce: light arriving from geometry, from a volume of probes "
        "around the camera (DDGI, RTXGI's). Each probe traces rays a few frames apart and "
        "keeps what it sees; a surface takes the 8 probes around it. Beyond the volume the "
        "analytic ground bounce stands in. The sky's own light is RT sky visibility's, not this.");
    const bool rtSupported = renderer.IsRaytracingSupported();
    if (!rtSupported)
    {
        ImGui::TextDisabled("Needs ray-tracing hardware: no bounce on this GPU.");
    }
    ImGui::BeginDisabled(!rtSupported);
    if (ImGui::Button("Reset GI to defaults"))
    {
        graphicsSettings.ResetGi(renderer, scene, settings);
    }
    GRAPHICS_CONTROL(GiEnabled, "giEnabled",
        ImGui::Checkbox("GI (probe volume)", &settings.giEnabled));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off = no bounce: a room lit by candles keeps only their direct light (its\n"
                          "corners and the cabinet under a sink go near black), and outdoors the\n"
                          "analytic ground bounce takes over everywhere. Frees ~0.2 ms of GPU.");
    ImGui::BeginDisabled(!settings.giEnabled);
    GRAPHICS_CONTROL(GiSpacing, "giSpacing",
        ImGui::SliderFloat("Probe spacing", &settings.giSpacing, 0.25f, 2.0f, "%.2f m"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Metres between probes; the volume is 32 x 16 x 32 of them around the camera,\n"
                          "so this also sets how far the bounce reaches. Finer = smaller rooms and\n"
                          "cabinets resolved, less light through thin walls, a smaller reach; coarser =\n"
                          "the opposite. Same cost either way. A change starts the volume over.");
    ImGui::TextDisabled("Covers %.0f x %.0f x %.0f m around the camera (%.0f m each side, %.0f m up and down).",
                        32.0f * settings.giSpacing, 16.0f * settings.giSpacing, 32.0f * settings.giSpacing,
                        16.0f * settings.giSpacing, 8.0f * settings.giSpacing);
    GRAPHICS_CONTROL(GiHysteresis, "giHysteresis",
        ImGui::SliderFloat("Hysteresis", &settings.giHysteresis, 0.8f, 0.99f, "%.3f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How much of its old light a probe keeps per update (default 0.98). Higher =\n"
                          "calmer and slower: at 0.9 the bathroom's walls breathe in metre-sized\n"
                          "blotches. A large change (a light switched off, the clock jumped) is followed\n"
                          "fast whatever this is; the probes' distances keep a slower one of their own.");
    GRAPHICS_CONTROL(GiIntensity, "giIntensity",
        ImGui::SliderFloat("Intensity", &settings.giIntensity, 0.0f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Multiplies the bounce. 1 = physical (the_bathroom_free checked against UE's\n"
                          "path tracer within ~20 %%); anything else is a look, not lighting.");

    ImGui::SeparatorText("Far cascade");
    GRAPHICS_CONTROL(GiFar, "giFar",
        ImGui::Checkbox("Far cascade", &settings.giFarCascade));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A second volume of the same 32 x 16 x 32 probes, coarser, around the near one:\n"
                          "the bounce reaches past the near volume instead of ending at it (beyond both,\n"
                          "the analytic ground bounce). Lighting blends to it over the near volume's outer\n"
                          "2 m, and reads it only there. Costs a second trace + update and ~51 MB of VRAM.");
    ImGui::BeginDisabled(!settings.giFarCascade);
    GRAPHICS_CONTROL(GiFarSpacing, "giFarSpacing",
        ImGui::SliderFloat("Far spacing", &settings.giFarSpacing, 1.0f, 8.0f, "%.1f m"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Metres between the far probes (at least twice the near spacing). Coarser = farther\n"
                          "reach, blurrier bounce; the same cost. A change starts the far volume over.");
    ImGui::TextDisabled("Covers %.0f x %.0f x %.0f m around the camera.",
                        32.0f * std::max(settings.giFarSpacing, 2.0f * settings.giSpacing),
                        16.0f * std::max(settings.giFarSpacing, 2.0f * settings.giSpacing),
                        32.0f * std::max(settings.giFarSpacing, 2.0f * settings.giSpacing));
    ImGui::EndDisabled();

    // The trace's price (SceneRenderSettings::giUpdateDivisor ... giSleepPeriod). Costs measured on the
    // owner's 18:00 view (cam 37.93 4.63 71.59) with both cascades; what each costs the picture in a
    // closed room is the bathroom by day.
    ImGui::SeparatorText("Trace cost");
    {
        int divisor = static_cast<int>(std::clamp(settings.giUpdateDivisor, 1u, 16u));
        if (GRAPHICS_CONTROL(GiDivisor, "giDivisor", ImGui::SliderInt("Update divisor", &divisor, 1, 16)))
        {
            settings.giUpdateDivisor = static_cast<std::uint32_t>(divisor);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("1 / this many of the near probes trace and blend a frame (default 4). The cost\n"
                          "follows it: 8 halves the trace and the update; the light then follows a change\n"
                          "twice as slowly. The noise stays (the hysteresis sets it).");
    {
        int farDivisor = static_cast<int>(std::clamp(settings.giFarDivisor, 1u, 16u));
        if (GRAPHICS_CONTROL(GiFarDivisor, "giFarDivisor",
                ImGui::SliderInt("Far update divisor", &farDivisor, 1, 16)))
        {
            settings.giFarDivisor = static_cast<std::uint32_t>(farDivisor);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The same for the far cascade (default 4).");
    {
        int sleep = static_cast<int>(std::clamp(settings.giSleepPeriod, 1u, 16u));
        if (GRAPHICS_CONTROL(GiSleepPeriod, "giSleepPeriod", ImGui::SliderInt("Unseen probes sleep", &sleep, 1, 16)))
        {
            settings.giSleepPeriod = static_cast<std::uint32_t>(sleep);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A probe no pixel has read for 32 frames (behind the camera, under the ground, in\n"
                          "the air) updates one turn in this many (default 4; 1 = every turn). On the 18:00\n"
                          "view the GI passes 0.50 -> 0.24 ms at 4, 0.20 at 8. The picture standing still is\n"
                          "the same; a region the camera turns to has followed a change of the light more\n"
                          "slowly (2 s after a turn: 1.75 %% of pixels off its settled frame against 1.0 %%).");
    {
        int rays = static_cast<int>(std::clamp(settings.giRays, 16u, 64u));
        if (GRAPHICS_CONTROL(GiRays, "giRays", ImGui::SliderInt("Rays per probe", &rays, 16, 64)))
        {
            settings.giRays = static_cast<std::uint32_t>((rays + 8) / 16 * 16);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The blended rays a probe traces an update (default 64; steps of 16). 32 halves the\n"
                          "trace (-0.13 ms) and cuts the update by a third; a probe's estimate is noisier and,\n"
                          "blended as E^(1/5), lower: the bathroom by day 4 levels darker. Outdoors no change.");
    {
        int fixedPeriod = static_cast<int>(std::clamp(settings.giFixedRayPeriod, 1u, 16u));
        if (GRAPHICS_CONTROL(GiFixedRayPeriod, "giFixedRayPeriod",
                ImGui::SliderInt("Classification every", &fixedPeriod, 1, 16)))
        {
            settings.giFixedRayPeriod = static_cast<std::uint32_t>(fixedPeriod);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A probe's 32 fixed rays (inside geometry? where to relocate?) every this many of its\n"
                          "updates (default 4; 1 = every one). Their directions never change, so their answers\n"
                          "hold while the probe stands where it stood; a probe new to its cell or moved traces\n"
                          "them at once. Moving geometry is re-classified that much later.");
    GRAPHICS_CONTROL(GiRayDistance, "giRayDistance",
        ImGui::SliderFloat("Ray reach", &settings.giRayDistance, 10.0f, 400.0f, "%.0f m",
                           ImGuiSliderFlags_Logarithmic));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far a probe ray looks for geometry (default 200 m, UE's Lumen). 30 m: -0.04 ms\n"
                          "here, and a probe more than that over the ground never sees it (a box 45 m up took\n"
                          "0.7 of UE's light on its shaded faces).");
    GRAPHICS_CONTROL(GiInfiniteBounce, "giInfiniteBounce",
        ImGui::Checkbox("Infinite bounce", &settings.giInfiniteBounce));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A probe ray's hit also takes the volume's own last light (a bounce of a bounce).\n"
                          "Off: -0.08 / -0.04 ms (near / far trace), and rooms lit by bounce alone darker.");
    ImGui::BeginDisabled(!settings.giInfiniteBounce);
    GRAPHICS_CONTROL(GiBounceVisibility, "giBounceVisibility",
        ImGui::Checkbox("...with visibility", &settings.giBounceVisibility));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("That bounce weighs the probes round the hit by their depth moments, as lighting\n"
                          "does. Off: -0.03 / -0.02 ms, and it LEAKS through walls: the bathroom by day 12\n"
                          "levels brighter. Outdoors no visible change.");
    ImGui::EndDisabled();
    GRAPHICS_CONTROL(GiSkyRay, "giSkyRay",
        ImGui::Checkbox("Sky ray at hits", &settings.giSkyRay));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A hit's sky light through one ray of visibility (default on). Off: -0.04 / -0.02 ms,\n"
                          "every hit sees the open sky -- a room's walls send its light back in: the bathroom\n"
                          "by day washes out. Outdoors under the crowns +3 levels.");
    GRAPHICS_CONTROL(GiLights, "giLights",
        ImGui::Checkbox("Lamps at hits", &settings.giLights));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Spot and point lights light the probes' hits, each with a shadow ray (default on).\n"
                          "Off = their bounce goes: the candle-lit bathroom keeps only their direct light.\n"
                          "Costs nothing while no lamp is lit; with the Lamps group on, every hit walks them.");
    ImGui::EndDisabled();

    // Plan S: the RT sky visibility (SceneRenderSettings::skyVis*). Its own system -- the sky's fill and
    // reflection, not the bounce -- here because it is the other half of how an interior is lit.
    ImGui::SeparatorText("Sky visibility");
    GRAPHICS_CONTROL(SkyVisEnabled, "skyVisEnabled",
        ImGui::Checkbox("Sky visibility (RT)", &settings.skyVisEnabled));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How much sky each point sees (UE's DFAO bent normal, traced on the TLAS at half\n"
                          "resolution after the G-buffer, before Lighting): the sky's fill and reflection stop\n"
                          "where the sky cannot reach -- inside a room, a tunnel, under a canopy. Off = the open\n"
                          "sky everywhere (a tunnel's floor lit like the beach).");
    ImGui::BeginDisabled(!settings.skyVisEnabled);
    {
        int skyVisRays = static_cast<int>(std::clamp(settings.skyVisRays, 1u, 16u));
        if (GRAPHICS_CONTROL(SkyVisRays, "skyVisRays", ImGui::SliderInt("Rays", &skyVisRays, 1, 16)))
        {
            settings.skyVisRays = static_cast<std::uint32_t>(skyVisRays);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rays per half-resolution pixel per frame (default 8). The cost is the ray count:\n"
                          "4 rays 0.14 ms, 8 rays 0.25 ms in a tunnel (0.3 / 0.63 ms on the grove). A surface\n"
                          "that sees the sky through a small opening needs more of them, or it grains.");
    GRAPHICS_CONTROL(SkyVisHistory, "skyVisHistory",
        ImGui::SliderFloat("History", &settings.skyVisHistory, 0.8f, 0.995f, "%.3f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The weight of the reprojected past (default 0.98). Higher = calmer, but surfaces a\n"
                          "moving camera uncovers take longer to settle. A tunnel floor at 4 rays: 8.6 %% frame\n"
                          "to frame at 0.95, 5.2 %% at 0.98; with 8 rays 3.5 %%.");
    GRAPHICS_CONTROL(SkyVisMaxDistance, "skyVisMaxDistance",
        ImGui::SliderFloat("Reach", &settings.skyVisMaxDistance, 2.0f, 60.0f, "%.0f m"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far a ray looks for something in the way (default 15 m; UE's SkyLight\n"
                          "OcclusionMaxDistance is 10). An occluder farther than this does not count.");
    GRAPHICS_CONTROL(SkyVisDiffuse, "skyVisDiffuse",
        ImGui::SliderFloat("Diffuse", &settings.skyVisDiffuse, 0.0f, 1.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How much it occludes the sky's diffuse fill: 1 = UE, 0 = not at all.");
    GRAPHICS_CONTROL(SkyVisSpecular, "skyVisSpecular",
        ImGui::SliderFloat("Specular", &settings.skyVisSpecular, 0.0f, 1.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How much it occludes the sky's reflection: 1 = UE, 0 = not at all.");
    GRAPHICS_CONTROL(SkyVisFilter, "skyVisFilter",
        ImGui::Checkbox("Spatial filter", &settings.skyVisFilter));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE DFAO's depth- and normal-aware filter on each frame's rays, before the history\n"
                          "(default on; 9x9 half-resolution texels, skyVis.filterRadius).\n"
                          "Off = the rays accumulated as they are, grainier.");
    {
        int filterRadius = static_cast<int>(std::clamp(settings.skyVisFilterRadius, 1u, 4u));
        if (GRAPHICS_CONTROL(SkyVisFilterRadius, "skyVisFilterRadius",
                ImGui::SliderInt("Filter radius", &filterRadius, 1, 4)))
        {
            settings.skyVisFilterRadius = static_cast<std::uint32_t>(filterRadius);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The spatial filter's reach in half-resolution texels (default 4 = 9x9). On the\n"
                          "owner's 18:00 view 3 saves ~0.025 ms, 2 ~0.05; a tunnel's visibility grain median\n"
                          "2.3 / 3.1 / 4.7 %% at 4 / 3 / 2.");
    GRAPHICS_CONTROL(SkyVisSeed, "skyVisSeed",
        ImGui::Checkbox("Seed texels without history", &settings.skyVisSeed));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A texel that lost its history (uncovered, come in over the edge, a frond's edge under\n"
                          "the jitter) starts from its neighbours' histories (UE DFAO), 5x5 first, 9x9 if those\n"
                          "hold none (default on). Off: -0.04 ms on the 18:00 view, and the rows a camera flying\n"
                          "backwards brings in start from their own eight rays: a band of grain.");
    {
        int fogRays = static_cast<int>(std::min(settings.skyVisFogRays, 8u));
        if (GRAPHICS_CONTROL(SkyVisFogRays, "skyVisFogRays", ImGui::SliderInt("Fog rays", &fogRays, 0, 8)))
        {
            settings.skyVisFogRays = static_cast<std::uint32_t>(fogRays);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The volumetric fog's sky light: rays per cell sample in UE's 45-degree cone straight\n"
                          "up (default 2; the fog's history averages them). 0 = the air sees the open sky\n"
                          "everywhere -- a tunnel's air glows blue from the fog's start distance on.\n"
                          "2 rays +0.01-0.02 ms, 4 rays up to +0.05 ms; no measurable frame-to-frame noise.");
    ImGui::EndDisabled();
    GRAPHICS_CONTROL(SkyVisFallback, "skyVisFallback",
        ImGui::Checkbox("Without RT: sky from above", &settings.skyVisFallback));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("With the sky visibility above off (or no RT hardware): the same occlusion from the\n"
                          "world drawn from above around the camera -- crowns, roofs and rocks as slabs over the\n"
                          "ground (SkyOcclusionMap, sky_occlusion_cs.hlsl). No rays, no history; it sees a\n"
                          "crown or a roof, not walls. Off = the open sky everywhere without the trace.\n"
                          "Never runs while the RT sky visibility does.");
    ImGui::BeginDisabled(!settings.skyVisFallback);
    {
        static const char* kResolutionLabels[] = { "64", "128", "256", "512", "1024" };
        int resolutionIndex = 0;
        for (std::uint32_t r = SkyOcclusionMap::kMinResolution;
             r < SkyOcclusionMap::ClampResolution(settings.skyVisFallbackResolution); r *= 2u)
        {
            ++resolutionIndex;
        }
        if (GRAPHICS_CONTROL(SkyVisFallbackResolution, "skyVisFallbackResolution",
                ImGui::Combo("Map resolution", &resolutionIndex, kResolutionLabels, IM_ARRAYSIZE(kResolutionLabels))))
        {
            settings.skyVisFallbackResolution = SkyOcclusionMap::kMinResolution << std::clamp(resolutionIndex, 0, 4);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The occlusion map's texels a side (default 512). The three depth renders under it are\n"
                          "4x that a side (D16): 256 = 6.5 MB, 512 = 26 MB, 1024 = 104 MB. A change re-creates\n"
                          "the textures (one GPU wait). With the texel below it sets how far the map reaches.");
    GRAPHICS_CONTROL(SkyVisFallbackTexel, "skyVisFallbackTexel",
        ImGui::SliderFloat("Map texel", &settings.skyVisFallbackTexel, SkyOcclusionMap::kMinTexel,
                           SkyOcclusionMap::kMaxTexel, "%.3f m", ImGuiSliderFlags_Logarithmic));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The world size of one texel of the occlusion map (default 1 m). Coarser reaches\n"
                          "farther and blurs a crown's edge; finer keeps it and reaches less. A change redraws\n"
                          "the map.");
    GRAPHICS_CONTROL(SkyVisFallbackFade, "skyVisFallbackFade",
        ImGui::SliderFloat("Edge fade", &settings.skyVisFallbackFade, 0.0f, 1.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How softly the occlusion gives way to the open sky toward the map's edge: the share\n"
                          "of the radius the map traces in full over which it fades, around the camera\n"
                          "(default 0.25). 0 = a hard edge.");
    {
        SkyOcclusionMap::Placement p;
        p.texel = SkyOcclusionMap::ClampTexel(settings.skyVisFallbackTexel);
        const std::uint32_t resolution = SkyOcclusionMap::ClampResolution(settings.skyVisFallbackResolution);
        p.halfExtent = 0.5f * static_cast<float>(resolution) * p.texel;
        const float end = SkyOcclusionMap::FullRadius(p, std::max(settings.skyVisMaxDistance, 0.5f));
        const float start = end * (1.0f - std::clamp(settings.skyVisFallbackFade, 0.0f, 1.0f));
        ImGui::TextDisabled("Covers %.0f m around the camera; full to %.0f m, open sky from %.0f m; %.1f MB",
                            2.0f * p.halfExtent, start, end, SkyOcclusionMap::MegabytesAt(resolution));
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Debug view");
    // Not persisted: a debug view is for now, not for the next launch.
    static const char* kGiDebugLabels[] = { "Off", "Bounce only", "Probe states" };
    int giDebug = static_cast<int>(std::min<std::uint32_t>(settings.giDebug, 2u));
    if (ImGui::Combo("GI debug view", &giDebug, kGiDebugLabels, IM_ARRAYSIZE(kGiDebugLabels)))
    {
        settings.giDebug = static_cast<std::uint32_t>(giDebug);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Bounce only: the lit frame shows the bounce alone (the lamps' own passes\n"
                          "still add); RED where no usable probe surrounds a point.\n"
                          "Probe states: the 8 probes around each point -- red moved by their last\n"
                          "update (red that stays = a probe that never settles), green off their grid\n"
                          "point (most surfaces: probes inside a wall were pushed out), blue switched\n"
                          "off as inside geometry.");
    ImGui::EndDisabled();
}

#endif // !TC_SHIPPING
