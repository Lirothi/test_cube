// R10.4: Developer Controls > CSM (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/visibility/OcclusionHistory.h" // S3a: vis::g_occlusion_game
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/ShadowGpuData.h" // Tessy N3: the cluster counter
#include "rendering/core/RasterSettings.h" // A6: the bindless checkbox

void DeveloperWindow::DrawCsmTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    // S0: the enabler for the whole CSM improvement plan (docs/csm_improvement_plan.md).
    // Without a live readout there is nothing to judge the fit/density steps on, and
    // without live config there is nothing to A/B them against.
    ImGui::TextWrapped("Legacy cascaded shadow maps \xE2\x80\x94 the fast alternative to VSM. "
        "Every number in the readout is what the cascade was actually built with this "
        "frame (Scene::UpdateCascades), not a value the UI re-derived.");
    ImGui::Separator();

    // S15: three modes, so this is a 3-way pick, not a checkbox. A checkbox that reads
    // "Legacy CSM active" while the mode is SDSM would be lying about two different
    // things at once (which algorithm runs, and whether the table below is what the
    // picture came from).
    DrawShadowModeCombo("csmShadowMode");
    if (render::VsmActive())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
            "VSM drives directional shadows right now. The cascades below are still\n"
            "computed every frame, but nothing samples them \xE2\x80\x94 the readout is live,\n"
            "the picture is not. Switch the mode above before judging any change.");
    else if (render::SdsmActive())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
            "SDSM drives directional shadows right now. The table below is the LEGACY\n"
            "fit, still computed every frame but not what the picture came from \xE2\x80\x94 it is\n"
            "the A/B baseline. The partitions actually used are in the SDSM section.");

    // The local lights' atlases are Legacy's and SDSM's (VSM has its own distant lights), so their wind
    // range lives on this tab.
    ImGui::SeparatorText("Local lights (atlas)");
    ImGui::BeginDisabled(render::VsmActive());
    GRAPHICS_CONTROL(LocalShadowWindRange, "localShadowWindRange",
        ImGui::SliderFloat("Lamp shadow sway range (m)", &render::g_localShadowWindRange_game,
                           -1.0f, 200.0f, "%.0f"));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A spot / point light whose reach comes within this many metres of the camera\n"
                          "redraws its swaying casters (palms, ferns) every frame. Farther away its shadow is\n"
                          "rigid: the swaying casters are drawn without sway and cached with the static ones,\n"
                          "nothing is redrawn (UE does the same for VSM's distant lights). -1 = every light\n"
                          "sways. wind_test, 100 lamps at night: the palms were 0.63 of the lamps' 0.78 ms.\n"
                          "Atlas modes (Legacy / SDSM) only.");
    ImGui::EndDisabled();

    CascadeShadowConfig& csmCfg = scene.CascadeConfig();

    ImGui::SeparatorText("Coverage");
    // The atlas edge. A COMBO of powers of two, not a slider: the 2x2 split and the
    // gutter arithmetic want an even edge, and the interesting values are four.
    // Changing it reallocates the atlas at GPU idle (Scene::ReconcileShadowMode), so
    // it stutters once on change -- like the Legacy/VSM switch above.
    {
        static const char* const kResNames[] = { "512", "1024", "2048", "4096", "8192" };
        static const unsigned kResValues[] = { 512u, 1024u, 2048u, 4096u, 8192u };
        int resIdx = 3;
        for (int i = 0; i < 5; ++i) { if (kResValues[i] == render::g_csmAtlasRes_game) { resIdx = i; } }
        if (GRAPHICS_CONTROL(CsmAtlasRes, "csmAtlasRes",
            ImGui::Combo("Atlas resolution", &resIdx, kResNames, 5)))
        {
            render::g_csmAtlasRes_game = kResValues[std::clamp(resIdx, 0, 4)];
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Edge of the shared CSM atlas, in texels. A tile is half of it,\n"
                              "its content is the tile minus twice the %u-texel gutter.\n"
                              "R16 depth: 4096 = 33.5 MB, 2048 = 8.4 MB, 1024 = 2.1 MB.\n"
                              "Legacy needs the texels (it fits a cascade to a slice SPHERE and\n"
                              "spends most of a tile on empty air); SDSM fits the tile to the\n"
                              "geometry and reaches Legacy's density at half the edge -- that is\n"
                              "the whole S16 bet, and this is what tests it.",
                              render::kCascadeAtlasBorder);
        const double mb = (static_cast<double>(render::g_csmAtlasRes_game) *
                           render::g_csmAtlasRes_game * 2.0) / (1024.0 * 1024.0);
        ImGui::TextDisabled("tile %u, content %u, %.1f MB",
                            render::g_csmAtlasRes_game / 2u,
                            (render::g_csmAtlasRes_game / 2u) > 2u * render::kCascadeAtlasBorder
                                ? (render::g_csmAtlasRes_game / 2u) - 2u * render::kCascadeAtlasBorder
                                : (render::g_csmAtlasRes_game / 2u),
                            mb);
    }
    GRAPHICS_CONTROL(CsmMaxDistance, "csmMaxDistance",
        ImGui::SliderFloat("Max distance (m)", &csmCfg.maxDistance,
                           20.0f, 1000.0f, "%.0f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Far edge of cascade 3 \xE2\x80\x94 the hard shadow terminator.\n"
                          "Shrinking it is the cheapest way to buy texel density everywhere\n"
                          "(no extra memory, no extra rasterization).");
    GRAPHICS_CONTROL(CsmAutoSplits, "csmAutoSplits",
        ImGui::Checkbox("Auto splits (UE distribution)",
                        &csmCfg.useUeSplitDistribution));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: author ONLY the max distance above; the three intermediate splits come from\n"
                          "UE's exponential distribution (ComputeAccumulatedScale, exponent below).\n"
                          "OFF: the hand-authored row is used.\n"
                          "The two schemes are stored SEPARATELY -- turning this on never overwrites the\n"
                          "custom distances, so the toggle is reversible and both are shown here.\n");

    ImGui::BeginDisabled(!csmCfg.useUeSplitDistribution);
    GRAPHICS_CONTROL(CsmDistributionExponent, "csmDistributionExponent",
        ImGui::SliderFloat("Distribution exponent",
            &csmCfg.cascadeDistributionExponent, 0.1f, 10.0f, "%.2f"));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE's CascadeDistributionExponent (default 3, clamp 0.1..10). Cascade weights are\n"
                          "exponent^i and a split sits at the running sum over the total: for exponent 3 and\n"
                          "4 cascades that is 1,3,9,27 of 40 = 2.5% / 10% / 32.5% / 100% of the range.\n"
                          "Higher = more resolution pulled toward the camera. UE substitutes 4 when a scene\n"
                          "has no valid precomputed lighting, i.e. the fully dynamic case.\n");

    // Both schemes side by side. The row that is NOT driving is greyed rather than hidden,
    // so the toggle never conceals the numbers it is not using -- nor destroys them.
    ImGui::BeginDisabled(csmCfg.useUeSplitDistribution);
    GRAPHICS_CONTROL(CsmSplitDistances, "csmSplitDistances",
        ImGui::DragFloat4("Split distances (m)",
            csmCfg.sliceDistances.data(), 0.25f, 0.5f, 1000.0f, "%.1f"));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hand-authored far plane of cascades 0..3 in view space. BuildSplitScheme clamps them\n"
                          "monotonic and caps them at max distance, so out-of-order values are safe.\n"
                          "Greyed while auto splits drive the cascades -- the values are kept, not cleared.\n");
    {
        const std::array<float, 4> ueSplits =
            csmCfg.ComputeUeSplitDistances(scene.CameraRef().GetZNear());
        ImGui::Text("UE auto splits (m):  %7.2f %7.2f %7.2f %7.2f",
            ueSplits[0], ueSplits[1], ueSplits[2], ueSplits[3]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What UE's distribution produces for the current max distance and exponent. Shown\n"
                              "whether or not the toggle is on, so the two schemes can be compared BEFORE\n"
                              "switching. A readout, not an editable control.\n");
    }

    ImGui::SeparatorText("Fit");
    GRAPHICS_CONTROL(CsmOverlap, "csmOverlap",
        ImGui::SliderFloat("Overlap (texels)", &csmCfg.overlapInTexels,
                           0.0f, 8.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Padding added to the fitted sphere radius to absorb the texel-snap shift,\n"
                          "in CASCADE TEXELS. The snap moves the centre by at most one texel per axis,\n"
                          "so 2 already leaves a full texel of slack, and the same number is correct\n"
                          "for every cascade. At 2 the measured worst-case slack is 1.2 texels; 1.0 is\n"
                          "still positive but thin, and by 0.5 the ortho no longer covers the snapped\n"
                          "slice \xE2\x80\x94 the UpdateCascades assert fires in Debug, shadows clip at the\n"
                          "cascade edge in Release. Watch 'R fit/pad': the two should nearly coincide.");
    GRAPHICS_CONTROL(CsmZPadding, "csmZPadding",
        ImGui::SliderFloat("Z padding (m)", &csmCfg.zPadding,
                           0.0f, 100.0f, "%.1f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Slack added past the far side of the light-space depth range.");
    GRAPHICS_CONTROL(CsmCasterReach, "csmCasterReach",
        ImGui::SliderFloat("Caster reach (m)", &csmCfg.casterReachWS,
                           0.0f, 400.0f, "%.0f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far TOWARD the light the ortho near plane is pulled back so casters\n"
                          "between the sun and the slice still render. This is what inflates zRange\n"
                          "(and the D16 step) in the readout. Drop it to 0 and watch tall casters'\n"
                          "shadows get clipped \xE2\x80\x94 that is the problem pancaking exists to solve.");

    // S11 [r.Shadow.CSMScissorOptim]. Off by default like UE's, and the readout column
    // shows the rect it WOULD apply, so the saving is visible before the risk is taken.
    GRAPHICS_CONTROL(CsmScissorOptim, "csmScissorOptim",
        ImGui::Checkbox("Scissor to view cone (UE CSMScissorOptim)", &csmCfg.scissorOptim));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The cascade tile is a square around the slice's bounding sphere; the camera\n"
                          "sees only a cone inside it. UE scissor the depth pass to that cone's projection\n"
                          "(camera + 4 far corners, rays extended to the tile border) -- pure rasterisation\n"
                          "saving, identical result for every receiver the CAMERA can see. Watch the\n"
                          "'scissor %%' column: 100 = nothing to cut (camera outside the tile, or looking\n"
                          "along the sun).\n\n"
                          "OFF by default, as in UE, and for a reason: GLASS shades reflected/refracted\n"
                          "receivers that can lie outside the camera cone, and those read the undrawn\n"
                          "part of the tile as LIT. Check water and glass before shipping it on.");
    GRAPHICS_CONTROL(CsmScissorPad, "csmScissorPad",
        ImGui::SliderFloat("Scissor pad (texels)", &csmCfg.scissorPadTexels,
                           0.0f, 16.0f, "%.0f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("OURS -- UE pad nothing. The filter's taps (6x6 tent = 3 texels) plus the\n"
                          "receiver normal offset can reach past the cone's edge at the screen border;\n"
                          "a tap on a scissored-out texel reads LIT and lightens the shadow there.\n"
                          "Set 0 to see the flaw UE ship, 4 covers the widest kernel.");

    // S14 [UE ShadowBoundsAccurate]. ON by default: it is what UE always do for a
    // directional cascade, and it is the cut the scissor cannot make -- before the VS.
    GRAPHICS_CONTROL(CsmAccurateCasterCull, "csmAccurateCasterCull",
        ImGui::Checkbox("Accurate caster cull (UE ShadowBoundsAccurate)", &csmCfg.accurateCasterCull));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cull a cascade's casters against the camera SLICE EXTRUDED TOWARD THE SUN\n"
                          "(UE ComputeShadowCullingVolume) instead of the cascade's full ortho box.\n"
                          "In the shadow map that is the convex hull of the slice's projection -- tighter\n"
                          "than the scissor's rectangle -- and it acts on both the CPU and the GPU cull\n"
                          "before any vertex is shaded. Tall casters outside the view are kept: what\n"
                          "shadows the slice projects INTO it along the light, by definition.\n\n"
                          "Readout: 'cull pl' = planes of the volume (6 = box, 7..11 = accurate),\n"
                          "'casters' = objects the CPU cull passed last frame. Same glass caveat as the\n"
                          "scissor -- UE ship with it anyway. OFF = the old box, for the A/B.");

    // Occlusion plan S5b [UE r.Shadow.Virtual.UseHZB, on a cascade tile]. OFF by default,
    // by measurement (see CascadeShadowConfig::hzbCull): conservative by construction
    // (two passes), the sampled shadow is identical, but the pyramids cost more than
    // today's casters save.
    GRAPHICS_CONTROL(CsmHzbCull, "csmHzbCull",
        ImGui::Checkbox("HZB occlusion of casters (UE VSM UseHZB, per cascade)", &csmCfg.hzbCull));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A caster hidden from the SUN by a nearer caster (a wall) writes nothing into the\n"
                          "shadow map. Test each cascade's casters against last frame's depth pyramid of\n"
                          "its tile, draw the rest (pass A), rebuild the pyramid, retest the deferred ones\n"
                          "against it with THIS frame's matrices and draw the bad guesses (pass B) -- no\n"
                          "latency, no popping, identical shadow. GPU (indirect) path only.\n"
                          "OFF by default: the four tile pyramids cost 0.11-0.13 ms GPU (one read of the\n"
                          "atlas), which today's palms behind a wall do not pay back. Turn on for content\n"
                          "whose HIDDEN casters are expensive; hzbDef below says how many there are.\n"
                          "--set=csm.hzbCull:0|1; readout columns hzbDef / hzbB.");
    {
        const ShadowGpuData& sg = scene.ShadowGpu();
        ImGui::Text("hzb cull: deferred %u / %u / %u / %u, pass B drew %u / %u / %u / %u",
                    sg.HzbDeferred(0), sg.HzbDeferred(1), sg.HzbDeferred(2), sg.HzbDeferred(3),
                    sg.HzbDrawnB(0), sg.HzbDrawnB(1), sg.HzbDrawnB(2), sg.HzbDrawnB(3));
    }

    // UE's three legacy-CSM bias cvars first, then the one knob that is OURS. Split in
    // two on purpose: the user could not tell which of the four numbers were a
    // transcription and which were invented here.
    // Exactly UE's three legacy-CSM bias cvars, nothing else. The receiver normal
    // offset that used to sit here is deleted: UE has no such term, and it only
    // existed to cover a depth-bias budget that was a quarter of theirs.
    // The three that decide ACNE vs PETER-PANNING. Everything here is measured on
    // wind_test at the user camera; see docs/csm_improvement_plan.md S6.
    GRAPHICS_CONTROL(CsmPancake, "csmPancake",
        ImGui::Checkbox("Pancake casters", &csmCfg.pancakeCasters));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WHAT IT DOES: a caster in front of the cascade's near plane is pressed ONTO it\n"
                          "instead of being clipped away. That is what lets the near plane be fitted TIGHT to\n"
                          "the slice instead of pulled back by Caster reach, and that is where the D16 precision\n"
                          "comes from - watch zRange and D16 step in the readout table drop on cascade 0.\n"
                          "\n"
                          "Casters are kept by a SEPARATE, wider culling near plane, so Caster reach still\n"
                          "matters; its meaning just narrows from \"how far to push the projection back\" to\n"
                          "\"how far toward the light to look for casters\".\n"
                          "\n"
                          "COST: a triangle with some vertices clamped and some not is deformed. Only on casters\n"
                          "straddling the near plane - UE document the same side effect.\n"
                          "\n"
                          "Turn OFF and tall casters should visibly lose their tops. If nothing changes, they are\n"
                          "being CULLED instead and pancaking is not doing anything.");

    GRAPHICS_CONTROL(CsmPancakeSlack, "csmPancakeSlack",
        ImGui::SliderFloat("Pancake slack (m)", &csmCfg.pancakeSlackWS,
                           0.0f, 160.0f, "%.1f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Metres of room between the slice and the near plane casters are clamped to.\n"
                          "0 = fitted tight: maximum D16 precision, and everything in front of the\n"
                          "slice gets pancaked.\n"
                          "Raise it if a caster STRADDLING the near plane shadows itself wrongly - a\n"
                          "triangle with some vertices clamped and some not is deformed, and slack\n"
                          "pushes the plane out of that geometry.\n"
                          "At slack == Caster reach the projection near equals the CULLING near and\n"
                          "the whole step degenerates to its pre-pancaking baseline - which also makes\n"
                          "this the A/B lever without rebuilding.\n"
                          "Watch zRange / D16 step in the readout table pay for it.");

    ImGui::SeparatorText("Bias \xE2\x80\x94 depth pass");
    GRAPHICS_CONTROL(CsmDepthBias, "csmDepthBias",
        ImGui::SliderFloat("Depth bias (texels)", &csmCfg.depthBiasInTexels,
                           0.0f, 12.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WHAT IT DOES: pushes every caster AWAY from the sun when its depth is written,\n"
                          "by this many cascade texels. Uniform - it does not care how the surface is angled.\n"
                          "\n"
                          "RAISE to kill acne. COST: peter-panning, i.e. shadows detach from what casts them.\n"
                          "\n"
                          "Measured here: acne collapses between 0.5 (79% of lit pixels) and 1.0 (0.03%), then\n"
                          "stays FLAT to 7.0 - so above ~1 it buys NOTHING but peter-panning, which keeps\n"
                          "growing (shadow lift -0.81 at 1.0, +0.60 at 1.5, +3.13 at 5.0).\n"
                          "\n"
                          "[r.Shadow.CSMDepthBias = 10] Their 10 is 5.00 here: their bias scales by\n"
                          "radius/resolution, our texel is 2*radius/resolution, exactly twice theirs. We ship\n"
                          "1.50 because we measured this scene; they ship 10 to cover every scene.\n"
                          "SIDE EFFECT: this number also sets the penumbra WIDTH (UE derive TransitionSize from\n"
                          "the same expression), so raising it softens shadow edges as well.");
    GRAPHICS_CONTROL(CsmSlopeScale, "csmSlopeScale",
        ImGui::SliderFloat("Slope scale", &csmCfg.slopeScale,
                           0.0f, 8.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WHAT IT DOES: adds EXTRA depth push, proportional to tan(angle between the surface\n"
                          "and the sun) - so a wall raking the light gets more, a surface facing it gets none.\n"
                          "Total push = depthBias * (1 + this * slope).\n"
                          "\n"
                          "THIS IS THE KNOB THAT SEPARATES THE TWO PROBLEMS. Acne only happens where the light\n"
                          "rakes the surface; peter-panning is only visible where it does not. Spending here\n"
                          "instead of on Depth bias buys acne protection that costs nothing where you can see it.\n"
                          "It needs the CASTER's normal, which only the depth pass has - the sampler cannot\n"
                          "do this at all.\n"
                          "\n"
                          "0 = flat bias everywhere. [r.Shadow.CSMSlopeScaleDepthBias = 3]");
    GRAPHICS_CONTROL(CsmMaxSlope, "csmMaxSlope",
        ImGui::SliderFloat("Max slope", &csmCfg.maxSlope,
                           0.0f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WHAT IT DOES: caps the tan() above. Mandatory: at exactly 90 degrees to the light the\n"
                          "required bias is infinite.\n"
                          "\n"
                          "WATCH OUT: at 1.0 the cap engages at 45 degrees from the sun, so with a LOW sun almost\n"
                          "all flat ground is already AT the cap. Raising this then moves the entire ground plane,\n"
                          "not just steep geometry - it stops behaving like a slope knob and behaves like a second\n"
                          "Depth bias.\n"
                          "\n"
                          "[r.Shadow.ShadowMaxSlopeScaleDepthBias = 1]");
    ImGui::Text("effective: %.2f texels facing the sun, %.2f grazing",
                csmCfg.depthBiasInTexels,
                csmCfg.depthBiasInTexels * (1.0f + csmCfg.slopeScale * csmCfg.maxSlope));

    ImGui::SeparatorText("Bias \xE2\x80\x94 sample time (not in UE)");
    GRAPHICS_CONTROL(CsmNormalBias, "csmNormalBias",
        ImGui::SliderFloat("Normal bias (texels)",
            &csmCfg.normalBiasInTexels, 0.0f, 4.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WHAT IT DOES: before looking the shadow up, slides the RECEIVER's sample point out\n"
                          "along its own normal by this many cascade texels. It moves WHERE we look, not what\n"
                          "was stored - so unlike the three knobs above it costs NO depth push.\n"
                          "\n"
                          "WHY IT IS THE CHEAPEST ACNE FIX: acne is a surface sampling its own stored depth.\n"
                          "Stepping off the surface stops that without moving the shadow toward the light, so\n"
                          "peter-panning barely responds to it.\n"
                          "\n"
                          "COST: it detaches shadows from CONVEX EDGES and thin geometry, because the offset\n"
                          "point can leave the object. Too high and contact shadows creep away from corners.\n"
                          "\n"
                          "UE has no equivalent - their legacy CSM relies on the depth pass alone, and their\n"
                          "stock defaults acne and peter-pan visibly. This is deliberately not a transcription.");
    ImGui::SeparatorText("Cascade transition");
    GRAPHICS_CONTROL(CsmBlendFraction, "csmBlendFraction",
        ImGui::SliderFloat("Blend fraction", &csmCfg.blendFraction,
                           0.0f, 0.3f, "%.3f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WHAT IT DOES: over the last this-much of a cascade's OWN SLICE LENGTH, cross-fade\n"
                          "into the next cascade instead of switching at the split. Hides the jump in texel\n"
                          "density, bias and kernel footprint that a hard switch makes visible as a seam.\n"
                          "\n"
                          "COST: pixels inside the band take a SECOND shadow sample. 0 = hard switch.\n"
                          "\n"
                          "Fraction of the SLICE, not of the absolute distance - that was the bug this fixes:\n"
                          "measured off the distance, c2 (35..100 m) faded over 10 m where UE fade over 6.5.\n"
                          "[CascadeTransitionFraction = 0.1, UE clamp it to 0.3 and so does this slider]");
    GRAPHICS_CONTROL(CsmDistanceFade, "csmDistanceFade",
        ImGui::SliderFloat("Distance fade", &csmCfg.distanceFadeFraction,
                           0.0f, 0.3f, "%.3f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("WHAT IT DOES: the LAST cascade has no coarser neighbour to hand over to, so instead\n"
                          "of blending it fades the shadow out to fully lit over the last this-much of its slice.\n"
                          "\n"
                          "WITHOUT IT shadows end in a hard terminator LINE at the shadow distance - everything\n"
                          "is shadowed at 299 m and nothing at 301 m. That line is the artifact this removes.\n"
                          "\n"
                          "COST: shadows go missing slightly earlier than the shadow distance says. Raise the\n"
                          "Split distances / max distance if that bites before this does.\n"
                          "0 = the hard line back (the rollback check).\n"
                          "\n"
                          "UE do the same by moving the last cascade's fade plane inward by the same extension.");

    if (ImGui::Button("Reset CSM config to defaults"))
        graphicsSettings.ResetCsm(renderer, scene, settings);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back to the build's defaults (tools/dist/graphics_defaults.json) \xE2\x80\x94 the\n"
                          "baseline every plan step is measured against. The reset is saved to\n"
                          "user_settings.json and applied to subsequently loaded levels.");

    ImGui::SeparatorText("Filtering");
    int csmFilter = static_cast<int>(csmCfg.filterMode);
    if (GRAPHICS_CONTROL(CsmFilter, "csmFilter",
        ImGui::SliderInt("Filter kernel", &csmFilter, 0, 2,
        csmFilter == 0 ? "3x3 box" : (csmFilter == 1 ? "4x4 tent (UE q3)" :
                                                      "6x6 tent (UE q5, default)"))))
        csmCfg.filterMode = static_cast<uint32_t>(csmFilter);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = the original 3x3 SampleCmp box with its per-cascade radius shrink (the A/B arm).\n"
                          "1 = soft-occlusion RAMP + 4x4 tent from 4 Gather quads -- UE's Manual3x3PCF.\n"
                          "2 = the same ramp + 6x6 tent from 9 gathers -- UE's Manual5x5PCF, and the DEFAULT,\n"
                          "    because r.ShadowQuality defaults to 5 in Unreal and ManualPCF selects 5x5 there.\n"
                          "The ramp's width is proportional to the cascade's world texel, so softness and\n"
                          "resolution drop by the SAME factor at a boundary; the kernel WIDTH is what sets the\n"
                          "absolute softness. A stock UE reads softer than a 4x4 tent for exactly that reason.\n");

    GRAPHICS_CONTROL(CsmSharpen, "csmSharpen",
        ImGui::SliderFloat("Shadow filter sharpen",
            &csmCfg.shadowFilterSharpen, 0.0f, 1.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE's per-light Shadow Filter Sharpen, same 0..1 artist range and same\n"
                          "default of 0 (= off). The shader receives x*7+1 and multiplies the raw\n"
                          "PCF ratio by it before saturating, so partial occlusion is pushed toward\n"
                          "0 or 1: the penumbra keeps its WIDTH but loses its gradient. 1.0 leaves a\n"
                          "kernel roughly as hard as no filtering at all.\n"
                          "Only affects the two tent kernels -- the 3x3 box arm never had it.");

    GRAPHICS_CONTROL(CsmReceiverBias, "csmReceiverBias",
        ImGui::SliderFloat("Receiver bias", &csmCfg.csmReceiverBias,
                           0.0f, 1.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE r.Shadow.CSMReceiverBias, default 0.9. Scales the soft-occlusion ramp\n"
                          "by how edge-on the receiver is to the light: at grazing angles a texel\n"
                          "spans much more depth, so the ramp has to widen or the surface\n"
                          "shadows itself. 0 removes the term entirely (acne on shallow ground),\n"
                          "1 is the widest ramp (peter-panning on those same slopes).\n"
                          "Only affects the two tent kernels.");

    GRAPHICS_CONTROL(CsmOverBlur, "csmOverBlur",
        ImGui::Checkbox("PCF over-blur correction",
                        &csmCfg.pcfOverBlurCorrection));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE's ApplyPCFOverBlurCorrection: square the resulting visibility.\n"
                          "A wide tent leaks light INTO the umbra, because every tap inside the\n"
                          "kernel votes equally regardless of how far the blocker is; squaring\n"
                          "darkens the mid-tones and pulls the shadow core back to solid while\n"
                          "leaving 0 and 1 untouched. UE apply it unconditionally to every\n"
                          "filtered shadow, so ON is the matching default.\n"
                          "Applied AFTER sharpen, in UE's order. Only affects the tent kernels.");

    ImGui::SeparatorText("Debug");
    bool csmTint = render::g_csmDebugMode_game == render::CsmDebugMode::CascadeTint;
    if (ImGui::Checkbox("Cascade tint", &csmTint))
        render::g_csmDebugMode_game = csmTint ? render::CsmDebugMode::CascadeTint
                                         : render::CsmDebugMode::Off;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tint each pixel by the cascade its shadow sample RESOLVED to:\n"
                          "c0 red, c1 green, c2 blue, c3 yellow, grey = past the last cascade.\n"
                          "Resolved, not selected: where a pixel falls into the tile-border margin\n"
                          "it silently drops to a coarser cascade, and the tint is what makes that\n"
                          "ring visible. Ignored in VSM mode.\n"
                          "It rides the SUN's contribution only — spot/point lights are added by\n"
                          "later passes and stay untinted, so read the zones outside their pools.");

    ImGui::SeparatorText("Readout");
    const SceneFrameData::CascadeData& csm = scene.GetCascadeData();
    const ImGuiTableFlags csmTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("CsmReadout", 11, csmTableFlags))
    {
        ImGui::TableSetupColumn("c");
        ImGui::TableSetupColumn("slice (m)");
        ImGui::TableSetupColumn("tile");
        ImGui::TableSetupColumn("texel (mm)");
        ImGui::TableSetupColumn("R fit/pad (m)");
        ImGui::TableSetupColumn("zRange (m)");
        ImGui::TableSetupColumn("D16 (mm)");
        ImGui::TableSetupColumn("bias (mm)");
        ImGui::TableSetupColumn("scissor %");
        ImGui::TableSetupColumn("cull pl");
        ImGui::TableSetupColumn("casters");
        ImGui::TableHeadersRow();

        for (int c = 0; c < SceneFrameData::kCascades; ++c)
        {
            // zRange is what D16 has to resolve; the depth bias is stored in NDC, so
            // multiplying it back by the range recovers the world-space peter-panning.
            const float range = csm.farLsDbg[c] - csm.nearLsDbg[c];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", c);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f-%.1f", csm.splitsVS[c], csm.splitsVS[c + 1]);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", csm.tileSizeDbg[c]);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f", csm.unitsPerTexelDbg[c] * 1000.0f);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.2f / %.2f", csm.sphereRadiusDbg[c], csm.radiusDbg[c]);
            ImGui::TableSetColumnIndex(5); ImGui::Text("%.1f", range);
            ImGui::TableSetColumnIndex(6); ImGui::Text("%.2f", (range / 65535.0f) * 1000.0f);
            ImGui::TableSetColumnIndex(7); ImGui::Text("%.1f", csm.depthBiasNDC[c] * range * 1000.0f);
            ImGui::TableSetColumnIndex(8); ImGui::Text("%.0f", csm.scissorAreaDbg[c] * 100.0f);
            ImGui::TableSetColumnIndex(9); ImGui::Text("%u", csm.cullPlanesDbg[c]);
            ImGui::TableSetColumnIndex(10); ImGui::Text("%u", csm.cullCastersDbg[c]);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("texel = world mm per shadow texel (lower is sharper).  R fit/pad = sphere\n"
                        "radius before/after overlap.  D16 = quantization step of the 16-bit depth\n"
                        "atlas over zRange.  bias = depth bias in world mm (= peter-panning).\n"
                        "scissor %% = share of the tile the view-cone scissor would rasterise (S11).");

    // ---- S15: SDSM -------------------------------------------------------------
    // Lives in the CSM tab on purpose: SDSM renders into the SAME atlas with the SAME
    // filter, and the table above is its A/B baseline. The comparison that judges the
    // step is one row of this table against one row of that one.
    ImGui::SeparatorText("SDSM (GPU-fitted partitions)");
    ImGui::TextWrapped("Sample Distribution Shadow Maps: the four partitions' depth interval "
        "AND their light-space box are reduced from THIS frame's depth buffer by compute "
        "(docs/csm_improvement_plan.md S15). Pick the mode above to turn it on.");
    {
        // How many partitions are analysed AND rendered. Fewer means bigger tiles for
        // the ones that remain (they still land on the same 2x2 grid), which is S17's
        // whole idea -- in a top-down view three of four tiles are wasted.
        int parts = static_cast<int>(render::sdsm::g_partitions_game);
        if (GRAPHICS_CONTROL(SdsmPartitions, "sdsmPartitions",
            ImGui::SliderInt("Partitions", &parts, 1,
                             static_cast<int>(render::sdsm::kMaxPartitions))))
        {
            render::sdsm::g_partitions_game = static_cast<std::uint32_t>(
                std::clamp(parts, 1, static_cast<int>(render::sdsm::kMaxPartitions)));
        }
        DevHelp("How many partitions are analysed and drawn. Fewer means BIGGER tiles for "
                "the ones that remain -- they still land on the same 2x2 atlas grid, so "
                "dropping to 2 doubles the edge of each. Unused tiles are not drawn at all "
                "(their cull rows carry the reject-all plane), and the sampler's interval "
                "selection collapses onto the last active partition, so coverage stays "
                "complete at any count. Worth lowering when the camera looks down, where "
                "the far partitions cover almost no screen.");
    }
    GRAPHICS_CONTROL(SdsmBorder, "sdsmBorder",
        ImGui::SliderFloat("Filter border (texels)", &render::sdsm::g_borderTexels_game, 0.0f, 16.0f, "%.0f"));
    DevHelp("Room reserved INSIDE the partition's box for the filter kernel, measured in "
            "texels of that partition's own tile. Too low: a wide filter reaches past the "
            "fitted box and its outer taps read cleared (lit) texels, so shadows thin out "
            "at the edge of a partition. Too high: every partition zooms OUT to make room, "
            "which costs resolution everywhere. This is NOT the atlas gutter -- that is a "
            "separate 4-texel ring outside the content rect and it still exists.");
    GRAPHICS_CONTROL(SdsmDilation, "sdsmDilation",
        ImGui::SliderFloat("Dilation (fraction)", &render::sdsm::g_dilation_game, 0.0f, 0.2f, "%.3f"));
    DevHelp("Grows the measured box by this fraction of its own extent, on every side. The "
            "reduction only ever sees VISIBLE depth samples, so a caster whose shadow lands "
            "just past the last of them would fall outside the projection and stop casting; "
            "this buys margin for it. It costs resolution in direct proportion -- 0.01 is a "
            "1% wider box and a 1% coarser texel -- so raise it only if shadows clip at the "
            "edges of what the camera can see.");
    GRAPHICS_CONTROL(SdsmMinScale, "sdsmMinScale",
        ImGui::SliderFloat("Min box / sphere", &render::sdsm::g_minScaleOverSphere_game, 0.005f, 1.0f, "%.3f"));
    DevHelp("A FLOOR on the box size, written as a fraction of the Legacy bounding sphere "
            "for the same depth interval -- which makes it a CEILING on how far SDSM may "
            "zoom in. Without it a partition holding a handful of pixels magnifies without "
            "bound and flickers as those pixels come and go. 1.0 = never tighter than "
            "Legacy, which is the A/B control; 0.02 = at most 50x tighter. It is also the "
            "fallback box for a partition that saw no samples at all.");
    GRAPHICS_CONTROL(SdsmZMargin, "sdsmZMargin",
        ImGui::SliderFloat("Z margin (m)", &render::sdsm::g_zMargin_game, 0.0f, 200.0f, "%.0f"));
    DevHelp("Metres added to the light-space DEPTH range beyond the measured bounds, so a "
            "caster standing between the sun and the visible geometry is still inside the "
            "partition's projection and still casts. Too low and tall casters clip out "
            "through the far cap -- their shadows blink off as the camera moves. It only "
            "has to cover that cap and the depth-precision margin: the near side is held "
            "by the same pancake clamp the Legacy path uses, not by this.");
    GRAPHICS_CONTROL(SdsmStability, "sdsmStability",
        ImGui::SliderFloat("Stability (half-life, frames)",
            &render::sdsm::g_stabilityFrames_game, 0.0f, 240.0f, "%.0f"));
    DevHelp("How many FRAMES it takes for half of a wobble in the measured depth range to "
            "decay. The interior boundaries are logarithmic over [minZ, maxZ], so anything "
            "that moves either end moves ALL of them and the cascade edge walks across the "
            "ground -- on an ocean view it is maxZ, swinging metres as waves open and close "
            "the furthest visible point. 0 = raw per-frame reduction (the A/B). Only the "
            "INTERIOR splits are smoothed; the ends of the range stay exactly as measured, "
            "so coverage and caster culling cannot regress however inert this gets. The "
            "cost is lag: the splits chase the scene, so very long half-lives can leave a "
            "boundary in the wrong place for a moment after a hard cut or a fast turn. "
            "Counted in FRAMES, so the real time constant follows the frame rate.");
    {
        int cacheFrames = static_cast<int>(render::sdsm::g_cacheFrames_game);
        if (GRAPHICS_CONTROL(SdsmCacheFrames, "sdsmCacheFrames",
            ImGui::SliderInt("Far partitions every N frames", &cacheFrames, 1, 8)))
        {
            render::sdsm::g_cacheFrames_game = static_cast<std::uint32_t>(std::clamp(cacheFrames, 1, 16));
        }
    }
    DevHelp("The partition cache. Partition 0 renders every frame; partition p >= 1 every "
            "min(N, 2^p) frames, staggered so a frame re-renders at most one of them besides "
            "0 (N = 4: partition 1 every 2nd frame, 2 and 3 every 4th). In between a partition "
            "keeps its tile and the matrix it was drawn with; only its depth interval is "
            "fresh. Far foliage shadows then sway at 1/period of the frame rate. Re-rendered "
            "out of turn when the sun moves, the camera cuts or turns/moves past the box's "
            "margin below, the atlas or the partition count changes. 1 = off.");
    GRAPHICS_CONTROL(SdsmCacheDilation, "sdsmCacheDilation",
        ImGui::SliderFloat("Cached box margin", &render::sdsm::g_cacheDilation_game, 0.0f, 0.3f, "%.3f"));
    DevHelp("How much wider (fraction of its own extent, per side) a partition that will be "
            "kept is fitted, so the camera can turn and move a little before a receiver walks "
            "off its tile (off it, the next partition shadows it, and past the last one "
            "nothing does). The camera guard re-renders at half of this margin. Wider = more "
            "headroom and slightly coarser far shadows.");
    GRAPHICS_CONTROL(SdsmTexelSnap, "sdsmTexelSnap",
        ImGui::Checkbox("Stable partitions (texel snap)", &render::sdsm::g_texelSnap_game));
    DevHelp("Every box is re-measured from the depth buffer each frame, and the DLSS jitter "
            "makes its edges wander even when nothing moves, so each re-render puts the "
            "shadow edges up to a texel elsewhere. With the cache above that shift is held "
            "for several frames and reads as trembling. On: the texel size and the depth "
            "range are kept while the measured box still fits (see the hysteresis below) and "
            "the box is snapped to a world-anchored texel grid -- a still camera re-renders "
            "the same tile bit for bit, a moving one slides it by whole texels. Off = the raw "
            "per-frame fit.");
    ImGui::BeginDisabled(!render::sdsm::g_texelSnap_game);
    GRAPHICS_CONTROL(SdsmSizeHysteresis, "sdsmSizeHysteresis",
        ImGui::SliderFloat("Size hysteresis", &render::sdsm::g_sizeHysteresis_game, 0.0f, 0.3f, "%.3f"));
    ImGui::EndDisabled();
    DevHelp("How much larger than the measured box a kept texel (and a kept depth range) "
            "may be before the partition is refitted -- a fraction of the measured size. "
            "A refit re-rasterises the whole tile once, so wider = fewer of those while the "
            "camera moves, at up to this much coarser shadows (half of it on average). 0 = "
            "refit whenever the size changes at all, which brings the trembling back.");

    // ---- S16: EVSM4 -------------------------------------------------------------
    ImGui::SeparatorText("SDSM filter (EVSM4)");
    GRAPHICS_CONTROL(SdsmEvsm, "sdsmEvsm",
        ImGui::Checkbox("EVSM4 moments instead of PCF", &render::sdsm::g_evsm_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The depth tile is converted to four exponentially-warped MOMENTS and\n"
                          "sampled through a Chebyshev bound. Moments are PREFILTERABLE: bilinear,\n"
                          "a box blur and (later) mips act on the shadow ESTIMATE, where PCF can\n"
                          "only average binary compares taken before the filter.\n"
                          "Costs a second atlas, RGBA32F -- four times the bytes per texel of the\n"
                          "depth it comes from, which is why the mode pays for it with a SMALLER\n"
                          "atlas rather than a bigger one.");
    ImGui::BeginDisabled(!render::sdsm::g_evsm_game);
    GRAPHICS_CONTROL(SdsmEvsmPos, "sdsmEvsmPos",
        ImGui::SliderFloat("Positive exponent", &render::sdsm::g_evsmPos_game,
                           1.0f, render::sdsm::kEvsmMaxExponent, "%.1f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The steep warp: sharp contact, and what crushes light leaking.\n"
                          "42 is the fp32 ceiling, not a taste -- exp(42) SQUARED is already\n"
                          "2.9e36 against the format's 3.4e38.");
    GRAPHICS_CONTROL(SdsmEvsmNeg, "sdsmEvsmNeg",
        ImGui::SliderFloat("Negative exponent", &render::sdsm::g_evsmNeg_game,
                           1.0f, render::sdsm::kEvsmMaxExponent, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The shallow warp, which catches what the positive one lets through.\n"
                          "The sample's ratio is 8:1 against the positive one.");
    GRAPHICS_CONTROL(SdsmBlur, "sdsmBlur",
        ImGui::SliderFloat("Edge softening (of partition)", &render::sdsm::g_evsmBlur_game,
                           0.0f, 0.05f, "%.4f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Separable box over the moments, its width a FRACTION OF THE PARTITION --\n"
                          "so it is a world size that grows with distance, like a real penumbra,\n"
                          "rather than a texel count that means something different per tile.\n"
                          "0 skips both blur passes. The sample's 0.02 is 3 m on our far partition\n"
                          "and washes the field flat: our scene is measured in metres, theirs is not.");
    GRAPHICS_CONTROL(SdsmBlurMax, "sdsmBlurMax",
        ImGui::SliderFloat("Softening cap (texels)", &render::sdsm::g_evsmBlurMaxTexels_game,
                           1.0f, 64.0f, "%.0f"));
    ImGui::EndDisabled();

    const SdsmShadows& sdsm = scene.Sdsm();
    if (!sdsm.ReadoutValid())
    {
        ImGui::TextDisabled("No partition readout yet (switch to SDSM and let a few frames pass).");
    }
    else
    {
        if (ImGui::BeginTable("SdsmReadout", 8, csmTableFlags))
        {
            ImGui::TableSetupColumn("p");
            ImGui::TableSetupColumn("interval (m)");
            ImGui::TableSetupColumn("box XY (m)");
            ImGui::TableSetupColumn("texel (mm)");
            ImGui::TableSetupColumn("zRange (m)");
            ImGui::TableSetupColumn("bias (mm)");
            ImGui::TableSetupColumn("samples");
            ImGui::TableSetupColumn("flags");
            ImGui::TableHeadersRow();
            const auto& parts = sdsm.Readout();
            for (int p = 0; p < static_cast<int>(render::sdsm::kMaxPartitions); ++p)
            {
                const render::sdsm::Partition& sp = parts[p];
                const float ex = sp.boundsMax.x - sp.boundsMin.x;
                const float ey = sp.boundsMax.y - sp.boundsMin.y;
                const float ez = sp.boundsMax.z - sp.boundsMin.z;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", p);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f-%.1f", sp.intervalBegin, sp.intervalEnd);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f x %.1f", ex, ey);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f / %.2f",
                    sp.texelWS[0] * 1000.0f, sp.texelWS[1] * 1000.0f);
                ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f", ez);
                ImGui::TableSetColumnIndex(5); ImGui::Text("%.1f", sp.depthBiasNDC * ez * 1000.0f);
                ImGui::TableSetColumnIndex(6); ImGui::Text("%u", sp.sampleCount);
                ImGui::TableSetColumnIndex(7); ImGui::Text("%s%s%s",
                    (sp.flags & render::sdsm::kPartitionFlagEmpty) ? "EMPTY " : "",
                    (sp.flags & render::sdsm::kPartitionFlagScaleClamped) ? "CLAMP " : "",
                    (sp.flags & render::sdsm::kPartitionFlagVolumeLeak) ? "VOLUME-LEAK" : "");
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("texel = world mm per texel along EACH light-space axis (the box is\n"
                            "anisotropic -- that is half the win). Compare the smaller of the two\n"
                            "against the same row of the cascade table above.\n"
                            "EMPTY = the partition saw no samples and fell back to the Legacy sphere;\n"
                            "CLAMP = the zoom ceiling fired.");
        if (sdsm.CpuCasterCount() > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                "%u CPU-tail casters do NOT cast in SDSM (their light matrix exists only\n"
                "on the GPU). Fold them into the indirect path or accept the missing shadow.",
                sdsm.CpuCasterCount());
    }
}

#endif // !TC_SHIPPING
