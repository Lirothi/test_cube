// R10.4: Developer Controls > Contact (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <algorithm>
#include <cstdint>
#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/shadows/ShadowSettings.h"

// Its own tab because it belongs to NEITHER shadow mode: the trace reads the camera
// depth buffer, so Legacy CSM and VSM get the identical term. Putting it under either
// one would say it is a property of that mode, which it is not.
void DeveloperWindow::DrawContactTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    if (ImGui::Button("Reset contact shadows to defaults"))
    {
        graphicsSettings.ResetContactShadows(renderer, scene, settings);
    }
    GRAPHICS_CONTROL(ContactEnabled, "contactEnabled",
        ImGui::Checkbox("Contact shadows ENABLED",
                        &render::contact::g_enabled_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Master switch, and OFF is the default -- which matches UE, whose per-light\n"
                          "ContactShadowLength defaults to 0. Off takes not a single depth sample.\n\n"
                          "A short march through the CAMERA depth buffer toward the sun, recovering the\n"
                          "scale a shadow-map texel cannot. Works in BOTH shadow modes and for"
                          " EVERY light -- sun, spot and point -- as UE run it from one"
                          " GetShadowTermsBase.");
    ImGui::BeginDisabled(!render::contact::g_enabled_game);

    {
        // Either/or for spot + point. The sun is not on this list: there the contact
        // term sits on top of CSM/VSM by design.
        static const char* kLocalModes[] = { "Shadow map (contacts off for locals)",
                                             "Contacts INSTEAD of the shadow map",
                                             "Auto: contacts only where no shadow slot",
                                             "Both (UE: contacts on top of the map)" };
        int mode = (int)std::min<std::uint32_t>(render::contact::g_localMode_game, 3u);
        if (GRAPHICS_CONTROL(ContactLocalMode, "contactLocalMode",
            ImGui::Combo("Local lights (spot/point)", &mode, kLocalModes, 4)))
            render::contact::g_localMode_game = (std::uint32_t)mode;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The first three give a local light ONE shadow source. Stacking a contact\n"
                              "trace on a small-range light's shadow map darkens the same contact twice.\n\n"
                              "Shadow map: what you had; this tab does nothing for locals.\n"
                              "Contacts instead: the map is not even sampled (9 atlas taps saved per\n"
                              "pixel), the 8-step depth trace is the light's only shadow. Cheap, but a\n"
                              "contact only reaches as far as the trace length -- nothing casts across\n"
                              "the room.\n"
                              "Auto: slotted lights keep their map, unslotted ones get contacts, so a\n"
                              "light that lost the atlas budget is not left with no shadow at all.\n"
                              "Both: UE's own arrangement (a light with Contact Shadow Length > 0) --\n"
                              "the trace multiplied into the map, for the A/B.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("As in UE (CastScreenSpaceShadowRay)");

    GRAPHICS_CONTROL(ContactTemporal, "contactTemporal",
        ImGui::Checkbox("Temporal dither (TAA/DLSS averages it)",
                        &render::contact::g_temporalDither_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE: InterleavedGradientNoise(PixelPos, StateFrameIndexMod8). The dither\n"
                          "pattern shifts every frame over an 8-frame cycle, so the temporal pass\n"
                          "averages the binary per-pixel hit/miss into a smooth value. This is the\n"
                          "cheapest denoiser there is -- no extra pass, no extra buffer.\n\n"
                          "It makes a SINGLE frame noisier and only pays off through DLSS/TAA; turn\n"
                          "it off to judge a still or when running --dlss=off.");

    GRAPHICS_CONTROL(ContactLengthWorldSpace, "contactLengthWorldSpace",
        ImGui::Checkbox("Length in METRES",
                        &render::contact::g_lengthInWorldSpace_game));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UE support both readings and pick between them by the SIGN of the value.\n\n"
                          "OFF (their default): length is a MULTIPLE OF VIEW DEPTH, so the trace covers\n"
                          "the same number of SCREEN pixels near and far and keeps working at distance.\n"
                          "ON: plain metres. Predictable, but shrinks below a pixel far away and stops\n"
                          "doing anything there.");

    const float lenMax = render::contact::g_lengthInWorldSpace_game ? 2.0f : 0.3f;
    GRAPHICS_CONTROL(ContactLength, "contactLength",
        ImGui::SliderFloat(
            render::contact::g_lengthInWorldSpace_game ? "Length (m)" : "Length (x view depth)",
            &render::contact::g_length_game, 0.0f, lenMax, "%.4f"));

    GRAPHICS_CONTROL(ContactIntensity, "contactIntensity",
        ImGui::SliderFloat("Intensity", &render::contact::g_intensity_game,
                           0.0f, 1.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How dark a hit makes the pixel (UE ContactShadowCastingIntensity).");

    int csSteps = static_cast<int>(render::contact::g_steps_game);
    if (GRAPHICS_CONTROL(ContactSteps, "contactSteps",
                         ImGui::SliderInt("Steps", &csSteps, 1, 16)))
        render::contact::g_steps_game = static_cast<std::uint32_t>(csSteps);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Depth samples along the ray. UE hardcode 8, and that is not laziness: their\n"
                          "compare tolerance is |rayDepthSpan| * (1/steps) * 2, so THE STEP COUNT IS\n"
                          "BAKED INTO THE ACCEPTANCE WINDOW.\n\n"
                          "Raising it narrows the window per sample while adding samples along a ray\n"
                          "that hugs the surface, so each pixel gets more sensitive to the dither\n"
                          "phase -- MORE speckle, not less. Measured added speckle: 4 -> +3.49pp,\n"
                          "8 -> +4.00, 16 -> +4.05, 32 -> +5.77. Capped at 16 for that reason.");

    ImGui::Separator();
    ImGui::TextDisabled("OURS -- Epic ship no denoiser and no distance fade");

    GRAPHICS_CONTROL(ContactThickness, "contactThickness",
        ImGui::SliderFloat("Max thickness (x ray length)",
            &render::contact::g_maxThicknessFrac_game, 0.0f, 3.0f, "%.2f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("THE knob for far-field speckle. A hit whose occluder sits further BEHIND\n"
                          "the ray point than this fraction of the ray length is rejected -- it is not\n"
                          "a contact, it is the far side of something.\n\n"
                          "A FRACTION, not metres, on purpose: the ray length is itself a multiple of\n"
                          "view depth, so it grows with distance. A fixed metre threshold cannot\n"
                          "track it -- near it is a no-op, far it kills the real contacts too. Tied\n"
                          "to the ray it stays meaningful at 10 m and at 3 km alike.\n\n"
                          "0 = no test, which is UE behaviour and is only safe at close range.");

    GRAPHICS_CONTROL(ContactNormalOffset, "contactNormalOffset",
        ImGui::SliderFloat("Normal offset (x ray length)",
            &render::contact::g_normalOffsetFrac_game, 0.0f, 0.5f, "%.3f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Starts the ray this far off the surface along the normal. A ray that begins\n"
                          "ON the surface is ambiguous at its very first step. UE do not do this --\n"
                          "they start at the shaded point.\n\n"
                          "A FRACTION of the ray length, not metres: what it fights is the world\n"
                          "footprint of a SCREEN PIXEL plus depth precision, and both grow with\n"
                          "distance. 0.02 m is meaningful at 10 m and far below a pixel at 350 m,\n"
                          "where it would quietly stop doing anything.");

    GRAPHICS_CONTROL(ContactGrazingFade, "contactGrazingFade",
        ImGui::SliderFloat("Grazing fade (NdotL)",
            &render::contact::g_grazingFadeNdotL_game, 0.0f, 0.5f, "%.3f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fades the term out below this NdotL. As the sun grazes a surface the ray\n"
                          "runs nearly PARALLEL to it and the march measures depth-buffer quantisation\n"
                          "rather than geometry -- that is the speckle field on flat distant ground.\n"
                          "0 = no guard (UE behaviour).");

    GRAPHICS_CONTROL(ContactMinDistance, "contactMinDistance",
        ImGui::SliderFloat("Min distance (m)",
            &render::contact::g_minDistanceM_game, 0.0f, 2000.0f, "%.0f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Nearer than this the term is off, fading IN over the band below. Push it out\n"
                          "to spend contacts only where the cascades are too coarse to resolve contact\n"
                          "detail -- the near field already has the shadow map for that.");
    GRAPHICS_CONTROL(ContactMaxDistance, "contactMaxDistance",
        ImGui::SliderFloat("Max distance (m)",
            &render::contact::g_maxDistanceM_game, 0.0f, 2000.0f, "%.0f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Metres from the camera. 0 max = no far limit. Outside the window the term\n"
                          "is off entirely -- which is the blunt way to kill artifacts in the far field\n"
                          "where contacts buy the least anyway.");
    GRAPHICS_CONTROL(ContactFadeBand, "contactFadeBand",
        ImGui::SliderFloat("Fade band (m)",
            &render::contact::g_fadeBandM_game, 0.1f, 200.0f, "%.1f"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Applies to BOTH edges of the window: N metres after Min to fade in, N metres\n"
                          "before Max to fade out. Without it either boundary reads as a line across\n"
                          "the ground. A window narrower than two bands simply never reaches full\n"
                          "strength -- the two ramps are combined with min(), not multiplied.");

    ImGui::EndDisabled();
}

#endif // !TC_SHIPPING
