#include "app/ui/DeveloperWindow.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>
#include <string_view>
#include <system_error>

#include "app/Systems.h"
#include "app/levels/JsonLevel.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "core/profiling/ProfilerScopes.h"
#if WITH_EDITOR
#include "editor/EditorController.h"
#endif
#include "imgui.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderStats.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/renderables/InstanceTypes.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "ocean/OceanSimulation.h"
#include "text/TextManager.h"

namespace
{
    // Hoverable "(?)" next to a control. The exposure knobs are the kind where the wrong mental
    // model costs an hour of tuning in the wrong direction, so they carry their explanation.
    void HelpMarker(const char* text)
    {
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    std::string NormalizeLevelPath(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }

    std::string LevelPathString(const std::filesystem::path& path)
    {
        return NormalizeLevelPath(path.string());
    }

    template <size_t N>
    void SetTextBuffer(char (&buffer)[N], const std::string& text)
    {
        const std::string normalized = NormalizeLevelPath(text);
        std::snprintf(buffer, N, "%s", normalized.c_str());
    }

    bool LevelFileExists(const std::string& path)
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (normalizedPath.empty())
        {
            return false;
        }

        std::error_code ec;
        const std::filesystem::path fsPath(normalizedPath);
        const bool exists = std::filesystem::exists(fsPath, ec);
        const bool regularFile = exists && std::filesystem::is_regular_file(fsPath, ec);
        return !ec && regularFile;
    }

    struct DlssModeOption
    {
        sl::DLSSMode mode;
        const char* label;
    };

    constexpr DlssModeOption kDlssModes[] = {
        { sl::DLSSMode::eOff, "Off" },
        { sl::DLSSMode::eMaxPerformance, "Max Performance" },
        { sl::DLSSMode::eBalanced, "Balanced" },
        { sl::DLSSMode::eMaxQuality, "Max Quality" },
        { sl::DLSSMode::eUltraPerformance, "Ultra Performance" },
        { sl::DLSSMode::eUltraQuality, "Ultra Quality" },
        { sl::DLSSMode::eDLAA, "DLAA" },
    };

    constexpr const char* kSsrTechniqueLabels[] = {
        "Log March",
        "UE SSR (HZB march)",
    };

    constexpr const char* kUeSsrQualityLabels[] = {
        "Custom",
        "UE Low (8x1)",
        "UE Medium (16x1)",
        "UE High (8x4)",
        "UE Epic (12x12)",
    };

    const char* DlssModeLabel(sl::DLSSMode mode)
    {
        for (const DlssModeOption& option : kDlssModes)
        {
            if (option.mode == mode)
            {
                return option.label;
            }
        }
        return "Unknown";
    }

    ID3D12Resource* GetOceanShoreDepthResource()
    {
        OceanSimulation* oceanSimulation = Systems::GetOceanSimulation();
        return oceanSimulation ? oceanSimulation->GetShoreDepthResource() : nullptr;
    }

}

// Hover text for a developer-window control. Kept deliberately blunt about GPU cost: several of
// these knobs are linear in frame time and nothing about the label says so.
static void DevHelp(const char* text)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void DeveloperWindow::RefreshLevelList()
{
    availableLevelPaths_.clear();
    levelChangeStatus_.clear();
    levelListScanned_ = true;

    std::error_code ec;
    std::filesystem::directory_iterator it("data/levels", ec);
    if (ec)
    {
        levelChangeStatus_ = "Cannot scan data/levels";
        return;
    }

    const std::filesystem::directory_iterator end;
    while (it != end)
    {
        const std::filesystem::directory_entry& entry = *it;
        std::error_code entryEc;
        if (entry.is_regular_file(entryEc) && !entryEc && entry.path().extension() == ".json")
        {
            availableLevelPaths_.push_back(LevelPathString(entry.path()));
        }

        it.increment(ec);
        if (ec)
        {
            levelChangeStatus_ = "Cannot scan all level files";
            break;
        }
    }

    std::sort(availableLevelPaths_.begin(), availableLevelPaths_.end());
    if (availableLevelPaths_.empty())
    {
        levelChangeStatus_ = "No JSON levels found in data/levels";
    }
}

void DeveloperWindow::ToggleTextureInspector()
{
    textureDebugViewer_.SetOpen(!textureDebugViewer_.IsOpen());
}

void DeveloperWindow::DrawTraceControls()
{
    if (!traceWindowOpen_)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 190.0f), ImGuiCond_FirstUseEver);
    bool open = traceWindowOpen_;
    if (ImGui::Begin("Trace capture###TraceCapture", &open))
    {
        const Profiler::TraceCaptureStatus status = Profiler::Get().GetTraceCaptureStatus();

        if (status.active)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "RECORDING");
            ImGui::SameLine();
            if (status.openEnded)
            {
                ImGui::Text("%u frames, %zu events", status.framesRecorded, status.events);
            }
            else
            {
                ImGui::Text("%u frames left", status.framesRemaining);
            }
        }
        else if (status.pending)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "starting next frame...");
        }
        else
        {
            ImGui::TextDisabled("idle");
        }

        ImGui::Separator();

        // Start/Stop, not a frame count: the things worth capturing (a drag, a hitch while walking)
        // have no length you can know in advance.
        ImGui::BeginDisabled(status.active || status.pending);
        if (ImGui::Button("Start", ImVec2(120.0f, 0.0f)))
        {
            Profiler::Get().BeginTraceCapture();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!status.active && !status.pending);
        if (ImGui::Button("Stop", ImVec2(120.0f, 0.0f)))
        {
            Profiler::Get().StopTraceCapture();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(status.active || status.pending);
        if (ImGui::Button("Quick 120", ImVec2(120.0f, 0.0f)))
        {
            Profiler::Get().RequestTraceCapture(120);
        }
        ImGui::EndDisabled();

        ImGui::TextDisabled("F11 does the same as Quick 120 (unless ImGui has the keyboard).");

        ImGui::Separator();
        if (status.lastPath.empty())
        {
            ImGui::TextDisabled("no capture written yet");
        }
        else
        {
            ImGui::TextWrapped("last: %s", status.lastPath.c_str());
            if (ImGui::Button("Copy path"))
            {
                ImGui::SetClipboardText(status.lastPath.c_str());
            }
        }
    }
    ImGui::End();
    traceWindowOpen_ = open;
}

void DeveloperWindow::Draw(Renderer& renderer, Scene& scene, const InputManager& input, LevelManager& levelManager, SceneRenderSettings& settings
#if WITH_EDITOR
    , EditorController& editorController
#endif
)
{
    CPU_SCOPE(ProfilerScopes::kBuildDeveloperWindow);

    if (!open_)
    {
        textureDebugViewer_.Draw(renderer, GetOceanShoreDepthResource());
    // The inspector cannot brighten its own preview (ImGui clamps the image tint), so it can hand
    // the request over to the fullscreen view, which draws through a shader we control.
    if (const int requested = textureDebugViewer_.TakeFullscreenRequest(); requested >= 0)
    {
        settings.debugTexTarget = requested;
        settings.debugTexMip = textureDebugViewer_.RequestedMip();
        settings.debugTexMode = true;
    }
        oceanControlsWindow_.Draw(renderer);
        DrawTraceControls();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(700.0f, 800.0f), ImGuiCond_FirstUseEver);

    bool open = open_;
    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoCollapse |
        (windowMaximize_.maximized ? ImGuiWindowFlags_NoMove : 0);
    if (ImGui::Begin("Developer Controls [F1]###DeveloperControls", &open, windowFlags))
    {
        ui::HandleWindowTitleDoubleClickMaximize(windowMaximize_);

        if (ImGui::BeginTabBar("DeveloperControlsTabs"))
        {
            if (ImGui::BeginTabItem("Render"))
            {
                const float fps = renderer.GetFPS();
                const float frameMs = fps > 0.0f ? 1000.0f / fps : 0.0f;
                ImGui::Text("FPS: %.1f (%.2f ms)", fps, frameMs);
                ImGui::Text("Display: %ux%u", renderer.GetWidth(), renderer.GetHeight());
                ImGui::Text("Render: %ux%u (scale %.2f)", renderer.GetRenderWidth(), renderer.GetRenderHeight(), renderer.GetRenderResolutionScale());
                ImGui::Text("Draw calls: %u   Primitives: %.3fM",
                    render::g_renderStats.lastDrawCalls,
                    static_cast<double>(render::g_renderStats.lastPrimitives) / 1.0e6);

                ImGui::Checkbox("Trace capture window", &traceWindowOpen_);

                // P6B. The chain produces a render-resolution AO target that NOTHING samples yet
                // (items 6-7 are the consumption step), so these controls change the debug view and
                // the GPU cost and nothing else. That is stated on screen rather than left for the
                // user to discover: a control whose effect is invisible reads as a broken control.
                // P6B: edits go to the SCENE copy, because these are LEVEL settings (saved with
                // the level like exposure and the colour pipeline). `settings.gtao` is only the
                // per-frame transport and is overwritten from the scene every push.
                GtaoSettings& gtao = scene.GtaoRef();
                ImGui::SeparatorText("Ambient occlusion (GTAO)");
                ImGui::Checkbox("GTAO enabled", &gtao.enabled);
                DevHelp("Ground-truth screen-space ambient occlusion. Whole chain ~0.12 ms at the defaults (2 directions x 6 steps, half render resolution). These are LEVEL settings - save them by adding the \"Ambient Occlusion (GTAO)\" environment object in the editor.");
                ImGui::BeginDisabled(!gtao.enabled);
                ImGui::Checkbox("Denoise", &gtao.denoise);
                DevHelp("Bilateral 5x5 across depth and normal. ~0.017 ms. Removes ~40% of the raw noise.");
                ImGui::SameLine();
                ImGui::Checkbox("Temporal", &gtao.temporal);
                DevHelp("Accumulates over frames - what makes a 2x6-tap estimate usable. ~0.012 ms plus one extra half-res target.");
                // The radius is in WORLD units, but it is clamped to at least `numSteps` pixels, so
                // below ~1 m it stops having any authority at distance. See the plan's P6B section.
                ImGui::SliderFloat("AO world radius", &gtao.worldRadius, 0.05f, 8.0f, "%.2f m");
                DevHelp("Occlusion reach in METRES, so contacts do not breathe as the camera moves. FREE in GPU cost. Loses authority below ~1 m: the radius is floored at Steps pixels, so at distance it stops being a world radius.");
                // P16.4: the second, much wider radius. At or below the contact radius the kernel
                // skips the whole second walk and copies the contact answer into both channels, so
                // dragging this to the bottom is the exact-no-op baseline for an A/B.
                ImGui::SliderFloat("AO sky radius", &gtao.skyRadius, 0.0f, 60.0f, "%.1f m");
                DevHelp("A SECOND horizon walk at a much wider radius, damping the SKY FILL only - "
                        "whether this ground is under a canopy or inside a doorway. The contact "
                        "radius above cannot answer that, and with one radius the sky reaches under "
                        "a palm crown as freely as it reaches open sand (measured: the crown was "
                        "worth 0.04 stops). At or below AO world radius this is OFF and bit-for-bit "
                        "the old pass. COSTS a second walk, so roughly doubles the raw GTAO pass.");
                ImGui::BeginDisabled(!(gtao.skyRadius > gtao.worldRadius) || !gtao.useHzb);
                {
                    int skyBias = static_cast<int>(gtao.skyMipBias);
                    if (ImGui::SliderInt("AO sky mip bias", &skyBias, 0, 5))
                    {
                        gtao.skyMipBias = static_cast<uint32_t>(std::max(0, skyBias));
                    }
                }
                DevHelp("Pyramid level the WIDE walk starts from. Its taps are tens of pixels apart, "
                        "so mip 0 both misses cache and aliases off whichever texel it lands on. "
                        "Higher = cheaper. Separate from the contact walk's bias on purpose.");
                ImGui::EndDisabled();
                ImGui::SliderFloat("AO thickness", &gtao.thickness, 0.0f, 1.0f, "%.2f");
                DevHelp("1 = occluders are fully solid behind their silhouette, so foliage casts a slab behind every leaf. UE equivalent works out to 0.75. FREE.");
                // OFF matches UE (r.GTAO.UseNormals = 0). ON feeds the integral the normal-mapped
                // normal while the horizon search still walks bare depth, which reads as occlusion
                // wherever the two disagree — kept only so the difference can be seen.
                ImGui::Checkbox("AO normal from G-buffer (not UE default)", &gtao.useGBufferNormal);
                ImGui::Checkbox("AO uses depth pyramid (HZB)", &gtao.useHzb);
                DevHelp("Walk the horizon search over the HZB instead of flat depth: far steps read a "
                        "coarser mip, which both aggregates (less aliasing) and stops the taps "
                        "scattering across memory. Measured -22% on Pass_Gtao, and it under-estimates "
                        "occlusion slightly by design - the pyramid keeps the FURTHEST depth per tile.");
                ImGui::BeginDisabled(!gtao.useHzb);
                {
                    int bias = static_cast<int>(gtao.hzbMipBias);
                    if (ImGui::SliderInt("AO HZB mip bias", &bias, 0, 4))
                    {
                        gtao.hzbMipBias = static_cast<uint32_t>(std::max(0, bias));
                    }
                }
                DevHelp("Added to every step's mip. Higher = cheaper and smoother, but occlusion "
                        "keeps thinning: measured pixels-below-0.9 18.4% (off) -> 15.8% (bias 0) -> "
                        "12.3% (bias 1). UE tie this to quality level: 0 from 8 taps up, 1 at 6.");
                ImGui::EndDisabled();
                DevHelp("OFF matches UE. The horizon search walks DEPTH, so the integral needs the geometric normal; ON feeds it the normal-mapped one and reads as occlusion wherever they disagree (measured AO 0.35 on a fully open dune). Comparison only.");
                ImGui::SliderFloat("AO intensity", &gtao.intensity, 0.1f, 4.0f, "%.2f");
                DevHelp("Exponent on the occlusion term - above 1 deepens, below 1 lifts. FREE.");
                // UE's AmbientOcclusionStaticFraction. 0 is an exact no-op, so this is the A/B
                // knob for "how much does the scene actually want this".
                ImGui::SliderFloat("AO strength (applied)", &gtao.strength, 0.0f, 1.0f, "%.2f");
                DevHelp("How much of the DYNAMIC occlusion reaches the image. 0 is an exact no-op - material AO still applies. FREE: the pass runs either way, so this is the A/B knob, not the off switch.");
                {
                    int angles = static_cast<int>(gtao.numAngles);
                    if (ImGui::SliderInt("AO directions", &angles, 1, 8))
                    {
                        gtao.numAngles = static_cast<uint32_t>(std::max(1, angles));
                    }
                    DevHelp("Screen directions per pixel. Cost is LINEAR - doubling this roughly doubles the raw pass. 2 is the UE default and leans on the temporal stage.");
                    int steps = static_cast<int>(gtao.numSteps);
                    if (ImGui::SliderInt("AO steps", &steps, 2, 16))
                    {
                        gtao.numSteps = static_cast<uint32_t>(std::max(1, steps));
                    }
                    DevHelp("Taps along each direction. Also LINEAR in cost, and it doubles as the floor on the pixel radius, so raising it widens the reach at distance too.");
                }
                ImGui::BeginDisabled(!gtao.temporal);
                ImGui::SliderFloat("AO temporal blend", &gtao.temporalBlendWeight, 0.02f, 1.0f, "%.3f");
                DevHelp("Weight of the CURRENT frame. 0.1 (UE default) is roughly a 10-frame history. Higher = more responsive, noisier. FREE.");
                // How far history may deviate from this frame before it is clamped. Below ~0.2 the
                // history stops accumulating on this engine (DLSS jitter widens the per-frame
                // spread past the window); above ~0.5 the clamp stops protecting against ghosting.
                ImGui::SliderFloat("AO temporal clamp", &gtao.temporalClampRange, 0.05f, 1.0f, "%.2f");
                DevHelp("How far history may deviate before being clamped back, with a still camera. Below ~0.2 the history stops accumulating here (DLSS jitter widens the spread past the window); above ~0.5 the clamp stops guarding against ghosting. FREE.");
                ImGui::EndDisabled();
                ImGui::TextDisabled("Lighting does not consume this yet (P6B items 6-7).");
                ImGui::TextDisabled("To see the AO itself: Debug tab -> Show -> \"GTAO ...\".");
                ImGui::EndDisabled();

                ImGui::Separator();

                // P7 aerial perspective. Edits go to the PER-FRAME settings, not to a scene copy,
                // because there is no environment object for this yet -- so these survive until the
                // level is reloaded and no further. That is on screen below rather than left to be
                // discovered when a tuned sky comes back empty.
                {
                    // Edits go to the SCENE copy: it is the source of truth and the app's
                    // SceneRenderSettings is only a transport that gets overwritten from here
                    // every push. Same reasoning as GtaoRef above.
                    AtmosphereSettings& atmo = scene.AtmosphereRef();
                    ImGui::SeparatorText("Aerial perspective (P7)");
                    ImGui::Checkbox("Fog enabled", &atmo.enabled);
                    DevHelp("Global analytic height fog on opaque geometry AND the ocean surface, "
                            "which share one packed parameter set so they cannot disagree. Off by "
                            "default: it is a real image change and earns its default with an A/B. "
                            "NOTHING SET HERE IS SAVED. These knobs write the live scene copy for "
                            "tuning; the values a level keeps live on the \"Aerial Perspective\" "
                            "object under Post-Process in the editor outliner, which every level "
                            "already has. Tune here, then type the numbers there - or tune there in "
                            "the first place and skip this panel.");
                    ImGui::BeginDisabled(!atmo.enabled);
                    ImGui::SliderFloat("Fog density", &atmo.density, 0.0f, 0.05f, "%.4f");
                    DevHelp("Extinction per world unit AT the reference height. This is the main "
                            "dial; everything else shapes it. 0 disables the whole block.");
                    ImGui::SliderFloat("Height falloff", &atmo.heightFalloff, 0.0f, 0.2f, "%.3f");
                    DevHelp("How fast density thins with altitude. 0 makes it a uniform distance "
                            "fog. This term is what stops a high camera getting a flat screen-space "
                            "wash: looking down from altitude most of the ray is in thin air. It "
                            "also sets how THIN the layer is - density halves every 1/falloff "
                            "metres - and a thin layer seen from above collapses into a bright line "
                            "on the horizon, because that is then the only ray with any depth in "
                            "it. Keep it near 0.02-0.03 for a camera that flies.");
                    ImGui::SliderFloat("Reference height", &atmo.referenceHeight, -50.0f, 200.0f, "%.1f m");
                    DevHelp("World Y at which 'density' is exactly the value above. Sea level here.");
                    ImGui::SliderFloat("Start distance", &atmo.startDistance, 0.0f, 300.0f, "%.0f m");
                    DevHelp("Fog-free air in front of the camera, so near contrast survives.");
                    ImGui::SliderFloat("Max opacity", &atmo.maxOpacity, 0.0f, 1.0f, "%.2f");
                    DevHelp("Ceiling on coverage, so distance never fully flattens shape. At 0 the "
                            "fog is a no-op even while enabled - useful as an A/B lever. The "
                            "ceiling is RELEASED deep into the fog, so it shapes the far field "
                            "without seaming at the horizon: the sky is never fogged and ignores "
                            "it, so a hard floor would leave the water holding colour the sky above "
                            "it has none of.");
                    ImGui::SliderFloat("Sun scatter", &atmo.sunScatterStrength, 0.0f, 3.0f, "%.2f");
                    DevHelp("Forward-scattered sun added on top of the sky colour, so looking into "
                            "the sun warms the haze and looking away from it does not.");
                    ImGui::SliderFloat("Sun scatter tightness", &atmo.sunScatterExponent, 1.0f, 64.0f, "%.0f");
                    DevHelp("UE's DirectionalInscatteringExponent; their default is 4. Higher = a tighter glow around the sun.");
                    ImGui::SliderFloat("Sun scatter start", &atmo.sunScatterStartDistance, 0.0f, 500.0f, "%.0f m");
                    DevHelp("UE keep the sun lobe out of the near field with a distance of its own (DirectionalInscatteringStartDistance). "
                            "Note the lobe FADES OUT as the fog saturates: our base colour is the sky, "
                            "which already contains the sun's glow, so a lobe surviving to the horizon "
                            "would add the sun twice - and only on geometry, never on the sky pixel "
                            "next to it, which is a hard seam along the horizon.");
                    ImGui::SliderFloat("Back scatter", &atmo.skyBackScatter, 0.0f, 1.0f, "%.2f");
                    DevHelp("The phase function: how bright the haze is with the sun BEHIND you, "
                            "relative to looking into it. 1 = flat, the same haze in every "
                            "direction. Real haze is forward-peaked, so lower values keep a "
                            "front-lit view crisp while a backlit one stays thick - at the SAME "
                            "density. This is the knob for that, not a second density: density is "
                            "extinction, and making it directional would fade distant shapes in and "
                            "out as you pan. Fades out with distance, where the sky sample already "
                            "carries its own anisotropy.");
                    ImGui::SliderFloat("Sky blur", &atmo.skyBlur, 0.0f, 1.0f, "%.2f");
                    DevHelp("How blurred the sky is where it is read as the fog's COLOUR, at the "
                            "lightly-fogged end. 0 samples it sharp, which prints cloud edges and the "
                            "sunset band onto whatever stands in front of them. Fully fogged pixels "
                            "ignore this and always converge on the sharp sky, or the horizon seams.");
                    static const char* kFogViews[] = { "Normal", "Transmittance", "In-scattering" };
                    int fogView = static_cast<int>(g_atmosphereDebugView);
                    if (ImGui::Combo("Fog debug view", &fogView, kFogViews, 3))
                    {
                        g_atmosphereDebugView = static_cast<uint32_t>(fogView);
                    }
                    DevHelp("Transmittance = what the SURFACE keeps (brighter: the air does less "
                            "here). In-scattering = what the AIR adds, already weighted by "
                            "coverage, so it is the term actually summed into the image rather "
                            "than the raw scattering colour. Opaque geometry AND the ocean both "
                            "report; only the SKY is black, and that means 'not measured here' "
                            "rather than 'no fog here'. READ THESE AS RELATIVE: the view is written into scene "
                            "colour, so exposure and the tone curve still run on it and 1.0 is "
                            "not white by construction. Not saved with the level.");
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("Model transcribed from UE HeightFogCommon.ush. Their density/");
                    ImGui::TextDisabled("falloff numbers do NOT transfer - UE author in centimetres.");
                    ImGui::TextDisabled("Judge on wind_test: overview / shore_grove / sun_glint.");
                    ImGui::TextDisabled("Fix exposure while comparing - auto-exposure reacts to fog");
                    ImGui::TextDisabled("and shifts the WHOLE frame, sky included.");
                }

                // P8 bloom. Edits go to the SCENE copy for the same reason the two blocks above do.
                {
                    BloomSettings& bloom = scene.BloomRef();
                    ImGui::SeparatorText("Bloom (P8)");
                    ImGui::Checkbox("Bloom enabled", &bloom.enabled);
                    DevHelp("Exposure-aware HDR bloom: threshold, a half-resolution pyramid, and a "
                            "tent reconstruction, composited before the tone curve. Off by default "
                            "and off costs nothing - no pass is scheduled and no pyramid is "
                            "touched. NOTHING SET HERE IS SAVED; the level's values live on the "
                            "\"Bloom\" object under Post-Process in the editor outliner.");
                    ImGui::BeginDisabled(!bloom.enabled);
                    ImGui::SliderFloat("Bloom intensity", &bloom.intensity, 0.0f, 2.0f, "%.3f");
                    DevHelp("Weight of the halo added back to the scene. 0 is an exact no-op and "
                            "skips the whole chain.");
                    ImGui::SliderFloat("Bloom threshold", &bloom.threshold, -1.0f, 8.0f, "%.2f");
                    DevHelp("Luminance AFTER exposure at which bloom starts. NEGATIVE disables the "
                            "threshold entirely, which is UE's default (-1) and which they call "
                            "physically correct - a lens scatters everything, not just what passes "
                            "a test. It also makes bloom exactly LINEAR in exposure; with a "
                            "threshold, raising exposure grows bloom faster than the image.");
                    ImGui::SliderFloat("Bloom soft knee", &bloom.softKnee, 0.05f, 4.0f, "%.2f");
                    DevHelp("Slope of the ramp above the threshold. UE hardwire 0.5. Higher is a "
                            "harder cut, lower a longer shoulder.");
                    ImGui::SliderFloat("Bloom radius", &bloom.radius, 0.0f, 4.0f, "%.2f");
                    DevHelp("Tap spacing of the tent upsample, in destination texels. Spreads the "
                            "same energy wider - it does not brighten.");
                    {
                        int method = static_cast<int>(bloom.method);
                        const char* kMethods[] = { "Standard (pyramid)", "Convolution (FFT)" };
                        if (ImGui::Combo("Bloom method", &method, kMethods, 2))
                        {
                            bloom.method = static_cast<std::uint32_t>(method < 0 ? 0 : method);
                        }
                    }
                    DevHelp("Standard = the mip pyramid. Convolution = two Fourier transforms and a "
                            "complex multiply against a generated aperture, which is where streaks "
                            "and the starburst come from. Convolution runs on a quarter-resolution "
                            "grid and costs more; the kernel's own transform is cached until its "
                            "parameters change.");
                    if (bloom.method == 1u)
                    {
                        ImGui::SliderFloat("Kernel radius", &bloom.convKernelRadius, 0.0005f, 0.02f, "%.4f");
                        DevHelp("Core radius as a fraction of the grid. Small is correct - the glare "
                                "is the skirt. At 0.12 only 4% of the kernel's energy sits within 32 "
                                "texels and the result is a flat wash.");
                        int blades = static_cast<int>(bloom.convBlades);
                        if (ImGui::SliderInt("Blades", &blades, 0, 12))
                        {
                            bloom.convBlades = static_cast<std::uint32_t>(blades < 0 ? 0 : blades);
                        }
                        DevHelp("N blades give 2N rays, falling BETWEEN the blades.");
                        ImGui::SliderFloat("Blade rotation", &bloom.convBladeRotation, -3.2f, 3.2f, "%.2f");
                    }
                    ImGui::Checkbox("Firefly clamp", &bloom.fireflyClamp);
                    DevHelp("Karis average on the FIRST downsample: each tap weighted by "
                            "1/(1+luma), so one blown-out texel cannot dominate its tile. This is "
                            "what keeps moving sun glints on water from pumping the whole bloom.");
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("Runs after DLSS, before the tone curve. Judge it on");
                    ImGui::TextDisabled("wind_test: sun_glint first - that view is the failure case.");
                }

                ImGui::Separator();

                const bool dlssAvailable = renderer.IsDlssAvailable();
                ImGui::Text("DLSS status: %s", renderer.IsDlssActive() ? "Active" : (dlssAvailable ? "Inactive" : "Unavailable"));

                ImGui::BeginDisabled(!dlssAvailable);
                bool dlssEnabled = renderer.IsDlssActive();
                if (ImGui::Checkbox("DLSS enabled", &dlssEnabled))
                {
                    if (dlssEnabled)
                    {
                        if (renderer.GetDlssMode() == sl::DLSSMode::eOff)
                        {
                            renderer.SetDlssMode(sl::DLSSMode::eBalanced);
                        }
                        renderer.SetDlssActive(true);
                    }
                    else
                    {
                        renderer.SetDlssActive(false);
                    }
                }

                const sl::DLSSMode currentDlssMode = renderer.GetDlssMode();
                if (ImGui::BeginCombo("DLSS quality", DlssModeLabel(currentDlssMode)))
                {
                    for (const DlssModeOption& option : kDlssModes)
                    {
                        const bool selected = option.mode == currentDlssMode;
                        if (ImGui::Selectable(option.label, selected))
                        {
                            renderer.SetDlssMode(option.mode);
                            renderer.SetDlssActive(option.mode != sl::DLSSMode::eOff);
                        }
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();

                if (!dlssAvailable)
                {
                    ImGui::TextDisabled("Streamline DLSS is not available for this run.");
                }

                ImGui::Separator();

                ImGui::Checkbox("FXAA", &settings.doFxaa);

                int ssrTechnique = static_cast<int>(settings.ssrTechnique);
                const int ssrTechniqueCount = static_cast<int>(SsrTechnique::Count);
                if (ssrTechnique < 0 || ssrTechnique >= ssrTechniqueCount)
                {
                    ssrTechnique = 0;
                }
                if (ImGui::Combo("SSR technique", &ssrTechnique, kSsrTechniqueLabels, ssrTechniqueCount))
                {
                    settings.ssrTechnique = static_cast<SsrTechnique>(ssrTechnique);
                }
                DevHelp("How the reflection ray searches for its hit. Log March takes 128 growing "
                        "view-space steps against full depth. UE SSR follows SSRTReflections.usf: "
                        "8/12/16 fixed screen-space steps issued in Batch4 against the FURTHEST HZB "
                        "at a mostly fixed mip, with roughness-aware High/Epic ray counts. It is "
                        "usually cheaper but deliberately approximate. Only affects SSR, not RT.");

                if (settings.ssrTechnique == SsrTechnique::UeHzb)
                {
                    UeSsrSettings& ue = settings.ssrUe;
                    int preset = static_cast<int>(ue.preset);
                    if (preset < 0 || preset >= static_cast<int>(UeSsrQualityPreset::Count))
                    {
                        preset = 0;
                    }
                    if (ImGui::Combo("UE SSR quality", &preset, kUeSsrQualityLabels,
                                     static_cast<int>(UeSsrQualityPreset::Count)))
                    {
                        ApplyUeSsrQualityPreset(ue, static_cast<UeSsrQualityPreset>(preset));
                    }
                    DevHelp("The actual SSRTReflections.usf presets. High/Epic use 4/12 GGX rays "
                            "only at roughness >= 0.1; smoother surfaces collapse the whole budget "
                            "to one 24-step mirror ray, exactly as UE do.");

                    if (ImGui::TreeNode("UE SSR advanced"))
                    {
                        int steps = static_cast<int>(ue.numSteps);
                        if (ImGui::SliderInt("UE steps / ray", &steps, 4, 64))
                        {
                            ue.numSteps = static_cast<uint32_t>(std::min(64, (steps + 3) & ~3));
                            ue.preset = UeSsrQualityPreset::Custom;
                        }
                        DevHelp("Rounded up to a multiple of four because UE issue depth requests "
                                "in Batch4. More steps fill thin/far silhouettes and narrow the "
                                "per-step depth interval; cost is linear per ray.");

                        int rays = static_cast<int>(ue.numRays);
                        if (ImGui::SliderInt("UE rays / pixel", &rays, 1, 12))
                        {
                            ue.numRays = static_cast<uint32_t>(rays);
                            ue.preset = UeSsrQualityPreset::Custom;
                        }
                        DevHelp("Used only with glossy rays. Cost is rays x steps; 12x12 is UE Epic "
                                "and is intentionally expensive on rough reflectors.");

                        if (ImGui::Checkbox("UE roughness GGX rays", &ue.glossyRays))
                        {
                            ue.preset = UeSsrQualityPreset::Custom;
                        }
                        DevHelp("OFF traces one geometric mirror ray. ON importance-samples the "
                                "reflector roughness when Rays > 1; roughness < 0.1 still collapses "
                                "to UE's one 24-step mirror ray.");

                        ImGui::Checkbox("UE use surface roughness", &ue.useSurfaceRoughness);
                        DevHelp("Normally reads packed roughness from GB0, matching UE. Disable to "
                                "force one value below -- useful for proving which quality branch "
                                "is active without editing a material.");
                        ImGui::BeginDisabled(ue.useSurfaceRoughness);
                        ImGui::SliderFloat("UE roughness override", &ue.roughnessOverride,
                                           0.0f, 1.0f, "%.2f");
                        ImGui::EndDisabled();

                        ImGui::SliderFloat("UE start mip", &ue.startMipLevel, 0.0f, 4.0f, "%.2f");
                        DevHelp("UE hard-code 1. Mip 0 is tighter and preserves thin shapes but "
                                "finds fewer conservative candidates; higher mips reach more but "
                                "broaden the candidate footprint.");

                        ImGui::SliderFloat("UE depth tolerance", &ue.slopeCompareToleranceScale,
                                           0.25f, 8.0f, "%.2f");
                        DevHelp("UE hard-code 4. Lower values reduce stretched masks and false "
                                "thickness, at the cost of more misses. Extra steps are the safer "
                                "way to lower this.");

                        int retries = static_cast<int>(ue.confirmRetries);
                        if (ImGui::SliderInt("UE confirm retries", &retries, 0, 8))
                        {
                            ue.confirmRetries = static_cast<uint32_t>(retries);
                        }
                        DevHelp("0 is stock UE: accept the first coarse HZB hit. Above zero, confirm "
                                "against full depth and continue after this many rejected coarse "
                                "candidates instead of turning the first rejection into a hole.");

                        int refine = static_cast<int>(ue.refineSteps);
                        if (ImGui::SliderInt("UE full-depth refine", &refine, 0, 8))
                        {
                            ue.refineSteps = static_cast<uint32_t>(refine);
                        }
                        DevHelp("Subdivides each coarse candidate interval against full-resolution "
                                "depth. Tightens balls/leaves and recovers a real crossing hidden "
                                "inside a mip1 tile. Paid only when a coarse candidate exists.");
                        ImGui::TreePop();
                    }
                }

                ImGui::Checkbox("SSR temporal resolve", &settings.ssrTemporal);
                DevHelp("Accumulates the screen-space reflection over time instead of showing each "
                        "frame raw. A reflected ray is violently sensitive to where it starts, so "
                        "under DLSS's per-frame jitter the raw buffer boils even with a still "
                        "camera -- measured 7.9x less frame-to-frame movement in the reflections "
                        "with this on. Unreal never display their SSR unfiltered either. "
                        "Perf: ~0.014 ms. Off = see the tracer's raw output, which is what you "
                        "want when judging a tracer rather than the picture.");
                ImGui::BeginDisabled(!settings.ssrTemporal);
                {
                    ImGui::SetNextItemWidth(140.0f);
                    ImGui::SliderFloat("SSR temporal blend", &settings.ssrTemporalBlendWeight,
                                       0.02f, 1.0f, "%.3f");
                    DevHelp("Weight of the CURRENT frame. UE use 1/8 = 0.125 for their SSR TAA "
                            "config. Lower = longer history, steadier but slower to react; 1 = no "
                            "accumulation at all.");
                    ImGui::SetNextItemWidth(140.0f);
                    ImGui::SliderFloat("SSR temporal clamp expand", &settings.ssrTemporalClampExpand,
                                       0.0f, 2.0f, "%.2f");
                    DevHelp("How far the neighbourhood clamp box may widen when the camera is "
                            "still. 0 = clamp hard to this frame's 3x3 box always (least ghosting, "
                            "least accumulation); higher lets a still camera keep a longer history.");
                }
                ImGui::EndDisabled();

                float ssrScale = renderer.GetReflectionTextureScale().x;
                if (ImGui::SliderFloat("Reflection resolution", &ssrScale, 0.25f, 1.0f, "%.2f"))
                {
                    renderer.SetReflectionTextureScale(ssrScale);
                }
                ImGui::Text("Reflection target: %ux%u", renderer.GetReflectionTextureWidth(), renderer.GetReflectionTextureHeight());
                // S15b: glass off-screen reflections render into a glass G-buffer + glassReflection at
                // this same reflection resolution and follow the reflection source below.
                // Inspect them via Texture inspector [F4] -> "Glass Refl Normal/Depth" + "Glass Reflection".
                ImGui::TextDisabled("Glass: traced in SSR/RT, cubemap in Sky only, disabled in None.");

                // S16: glossy reflections — blur radius scales with surface roughness (0 = sharp mirror).
                ImGui::SliderFloat("Glossy blur", &settings.reflectionGlossyScale, 0.0f, 24.0f, "%.1f");

                // Analytic sun specular boost on metals: spec lobe *= (1 + metal*coef). 0 = physical.
                ImGui::SliderFloat("Sun spec on metal", &settings.sunMetalSpecInfluence, 0.0f, 16.0f, "%.1f");

                // Sun angular size: floors the analytic specular lobe width so smooth surfaces show
                // a bright, sample-able sun glint instead of a sub-pixel spike. 0 = punctual.
                ImGui::SliderFloat("Sun angular size", &settings.sunAngularSize, 0.0f, 0.25f, "%.3f");

                float oceanReflectionScale = renderer.GetOceanReflectionTextureScale().x;
                if (ImGui::SliderFloat("Ocean reflection resolution", &oceanReflectionScale, 0.25f, 1.0f, "%.2f"))
                {
                    renderer.SetOceanReflectionTextureScale(oceanReflectionScale);
                }
                ImGui::Text("Ocean reflection target: %ux%u", renderer.GetOceanReflectionTextureWidth(), renderer.GetOceanReflectionTextureHeight());

                ImGui::Separator();

                // Reflection source (S8): None / Sky only / SSR / RT. RT is greyed out on
                // non-RT hardware (and the renderer falls back to SSR anyway).
                const bool rtSupported = renderer.IsRaytracingSupported();
                const char* srcLabels[] = { "None", "Sky only", "SSR", "RT" };
                constexpr int srcCount = static_cast<int>(ReflectionSource::Count);
                int curSrc = static_cast<int>(settings.reflectionSource);
                if (curSrc < 0 || curSrc >= srcCount) { curSrc = static_cast<int>(ReflectionSource::SSR); }
                if (ImGui::BeginCombo("Reflections [F5]", srcLabels[curSrc]))
                {
                    for (int i = 0; i < srcCount; ++i)
                    {
                        const bool isRT = (i == static_cast<int>(ReflectionSource::RT));
                        ImGui::BeginDisabled(isRT && !rtSupported);
                        if (ImGui::Selectable(srcLabels[i], curSrc == i))
                        {
                            settings.reflectionSource = static_cast<ReflectionSource>(i);
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                if (!rtSupported)
                {
                    ImGui::TextDisabled("RT requires hardware ray tracing (unavailable).");
                }

                ImGui::BeginDisabled(!rtSupported);
                ImGui::Checkbox("RT debug view -> Reflection target [F6]", &settings.rtDebugView);
                ImGui::EndDisabled();
                if (rtSupported && settings.rtDebugView)
                {
                    ImGui::TextDisabled("Open the texture inspector [F4] and select 'Reflection'.");
                }

                ImGui::Separator();

                const bool dlssControlsRenderScale = renderer.IsDlssActive();
                ImGui::BeginDisabled(dlssControlsRenderScale);
                float renderScale = renderer.GetRenderResolutionScale();
                if (ImGui::SliderFloat("Render scale", &renderScale, 0.1f, 1.0f, "%.2f"))
                {
                    renderer.SetRenderResolutionScale(renderScale);
                }
                if (ImGui::Button("Native render scale"))
                {
                    renderer.SetRenderResolutionScale(1.0f);
                }
                ImGui::EndDisabled();
                if (dlssControlsRenderScale)
                {
                    ImGui::TextDisabled("DLSS quality controls the render scale while active.");
                }

                ImGui::EndTabItem();
            }

            // P2: live tuning for the photographic camera. Its own tab because it is a whole
            // control surface, not a readout, and because these knobs are meant to be swept while
            // watching the frame. Everything here is RUNTIME state (Scene::CameraExposureRef) --
            // it does not write the level, so a value worth keeping has to be copied into the
            // level's cameraExposure section afterwards. The tab is deliberately available in
            // Release: this is the tuning surface, not an editor feature.
            if (ImGui::BeginTabItem("Exposure"))
            {
                render::CameraExposureSettings& exposure = scene.CameraExposureRef();
                const ExposureMetering& metering = renderer.Exposure();
                const ExposureMetering::Readback readback = metering.LatestReadback();

                ImGui::Checkbox("Enabled", &exposure.enabled);
                ImGui::SameLine();
                HelpMarker(
                    "Master switch for the photographic camera.\n\n"
                    "OFF: the exposure multiplier is exactly 1.0 and the metering pass does no GPU "
                    "work at all (one empty command list, 0.001 ms). The image is bit-identical to "
                    "what the renderer produced before this feature existed.\n\n"
                    "ON: the scene is metered every frame and scene colour is multiplied by the "
                    "adapted exposure just before the tone curve -- after the DLSS resolve, so the "
                    "upscaler never sees an exposed image. Costs about 0.028 ms.\n\n"
                    "NOTE: the directional light still carries its own legacy 'exposure' value "
                    "(2.0 on wind_test). While that is still there, auto-exposure is partly "
                    "cancelling it rather than replacing it, which is why turning this on barely "
                    "changes the image. Separating the two is a later step of the plan.");

                if (!exposure.enabled)
                {
                    ImGui::TextDisabled("Dormant: multiplier x%.3f", render::kIdentityExposureMultiplier);
                }

                ImGui::BeginDisabled(!exposure.enabled);

                ImGui::Checkbox("Auto exposure", &exposure.autoExposure);
                ImGui::SameLine();
                HelpMarker(
                    "ON: the scene is metered every frame from a 256x144 grid of samples, and the "
                    "camera adapts towards the result at the speeds below.\n\n"
                    "OFF: the camera is pinned to 'Manual EV100' with no adaptation at all -- use "
                    "this when comparing two builds or hunting a lighting bug, because a moving "
                    "camera makes every A/B unreadable.\n\n"
                    "The sample grid is FIXED and normalised, not one sample per pixel. That is "
                    "why the pass costs the same at 1080p and 4K, and why native and DLSS meter "
                    "the identical positions and settle on the same value.");

                ImGui::SeparatorText("Live");
                if (readback.valid)
                {
                    const float multiplier = render::ExposureMultiplierFromEv100(readback.adaptedEv100);
                    ImGui::Text("Adapted   %+.3f EV100   (x%.5f)", readback.adaptedEv100, multiplier);
                    ImGui::Text("Target    %+.3f EV100", readback.targetEv100);
                    ImGui::Text("Metered   low %.5f   high %.5f  (scene linear)",
                        readback.lowLuminance, readback.highLuminance);
                    const float gap = readback.targetEv100 - readback.adaptedEv100;
                    ImGui::TextDisabled(fabsf(gap) < 0.01f ? "settled" : "adapting (%+.2f EV to go)", gap);
                }
                else
                {
                    ImGui::TextDisabled("waiting for the first readback...");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset adaptation"))
                {
                    renderer.Exposure().RequestReset();
                }
                ImGui::SameLine();
                HelpMarker(
                    "Adapted = what the camera is using right now. Target = where it is heading. "
                    "Metered low/high = the scene-linear luminance at the two percentiles below, "
                    "which is the actual window the average was taken over -- watch these while "
                    "sweeping the percentile sliders to see what is being included.\n\n"
                    "'Reset adaptation' snaps Adapted to Target instantly, the same thing that "
                    "happens on level load, resize and camera cuts.");

                // Plan section 6.5 asks for a histogram visualisation, and the percentile sliders
                // below are unreadable without one -- they clip a distribution you otherwise cannot
                // see. Bins are normalised to the PEAK so the shape survives a big flat sky.
                ImGui::SeparatorText("Histogram");
                {
                    static float bins[ExposureMetering::kHistogramBins] = {};
                    UINT total = 0;
                    if (metering.LatestHistogram(bins, ExposureMetering::kHistogramBins, &total) && total > 0)
                    {
                        const float minLog = ExposureMeteringConstants::kMinLogLum;
                        const float maxLog = ExposureMeteringConstants::kMaxLogLum;

                        const ImVec2 size(ImGui::GetContentRegionAvail().x, 90.0f);
                        const ImVec2 origin = ImGui::GetCursorScreenPos();
                        ImGui::PlotHistogram("##exposureHistogram", bins,
                            ExposureMetering::kHistogramBins, 0, nullptr, 0.0f, 1.0f, size);

                        // Shade what the percentiles actually keep. The window is defined on the
                        // CUMULATIVE distribution, not on bin position, so the markers have to be
                        // found by walking the bins -- placing them at lowPercentile * binCount
                        // would simply lie. bins[] is peak-normalised, but the cumulative FRACTION
                        // is scale-invariant, so normalising by their sum recovers it exactly.
                        constexpr int kBinCount = static_cast<int>(ExposureMetering::kHistogramBins);
                        float sum = 0.0f;
                        for (int i = 0; i < kBinCount; ++i) { sum += bins[i]; }
                        const float invSum = sum > 0.0f ? 1.0f / sum : 0.0f;

                        int loBin = 0;
                        int hiBin = kBinCount - 1;
                        bool haveLo = false;
                        float cumulative = 0.0f;
                        for (int i = 0; i < kBinCount; ++i)
                        {
                            cumulative += bins[i] * invSum;
                            if (!haveLo && cumulative >= exposure.lowPercentile) { loBin = i; haveLo = true; }
                            if (cumulative >= exposure.highPercentile) { hiBin = i; break; }
                        }

                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const float binW = size.x / static_cast<float>(kBinCount);
                        const float x0 = origin.x + binW * static_cast<float>(loBin);
                        const float x1 = origin.x + binW * static_cast<float>(hiBin + 1);
                        dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + size.y),
                            IM_COL32(90, 170, 255, 40));
                        dl->AddLine(ImVec2(x0, origin.y), ImVec2(x0, origin.y + size.y), IM_COL32(90, 170, 255, 180));
                        dl->AddLine(ImVec2(x1, origin.y), ImVec2(x1, origin.y + size.y), IM_COL32(90, 170, 255, 180));

                        // Where the adapted exposure currently sits, in the same log-luminance axis.
                        if (readback.valid)
                        {
                            const float meteredLog = readback.adaptedEv100 - 3.0f; // EV100 = log2(L)+3
                            const float t01 = (meteredLog - minLog) / (maxLog - minLog);
                            if (t01 >= 0.0f && t01 <= 1.0f)
                            {
                                const float mx = origin.x + size.x * t01;
                                dl->AddLine(ImVec2(mx, origin.y), ImVec2(mx, origin.y + size.y),
                                    IM_COL32(255, 210, 90, 220), 2.0f);
                            }
                        }

                        ImGui::TextDisabled("%.0f .. %.0f log2 luminance   |   %u samples   |  "
                                            "blue = metered window, yellow = adapted",
                                            minLog, maxLog, total);
                    }
                    else
                    {
                        ImGui::TextDisabled("histogram available once metering has run "
                                            "(enable the camera above)");
                    }
                }

                ImGui::SeparatorText("Metering");
                // P16.13: two fields, and only the one the current mode uses is shown. They were a
                // single value, so a trim dialled for a metered shot silently followed you into a
                // fixed-exposure one and back again. Showing both at once would only move the
                // confusion from the value to the label.
                if (exposure.autoExposure)
                {
                    ImGui::SliderFloat("Compensation (EV)", &exposure.compensationEv, -8.0f, 8.0f, "%+.2f");
                }
                else
                {
                    ImGui::SliderFloat("Compensation (EV, manual)", &exposure.manualCompensationEv,
                                       -8.0f, 8.0f, "%+.2f");
                }
                ImGui::SameLine();
                HelpMarker(
                    "Artistic offset in stops, applied on top of whatever the meter decided.\n\n"
                    "POSITIVE = BRIGHTER image. (Internally it subtracts from the EV, because a "
                    "higher EV means a more closed camera. The slider is in the direction you "
                    "expect; the sign flip happens in the shader.)\n\n"
                    "+1.0 doubles scene brightness, -1.0 halves it. This is the knob to reach for "
                    "when the meter is technically right but the shot wants to be brighter or "
                    "moodier -- it moves the whole image without changing how the meter behaves.\n\n"
                    "RE-CHECK THIS AFTER CHANGING THE TONE CURVE. The curves do not sit at the same "
                    "brightness -- Filmic runs about 0.4 EV darker than Legacy on the same scene -- "
                    "so a compensation tuned against one will stack with the other and overshoot.");

                ImGui::SliderFloat("Low percentile", &exposure.lowPercentile, 0.0f, 0.95f, "%.3f");
                ImGui::SameLine();
                HelpMarker(
                    "Fraction of the DARKEST samples thrown away before averaging.\n\n"
                    "0.02 keeps almost everything, so deep shade counts fully and the camera opens "
                    "up to expose it. Raising it makes the meter ignore shadows and expose for the "
                    "brighter subject instead -- the image gets DARKER overall but shadows stop "
                    "dragging the exposure up.\n\n"
                    "Reference: Narkowicz recommends discarding 50-80% of the darkest samples, and "
                    "Unreal's histogram default has historically sat near 80%. Our 0.02 default is "
                    "far more shadow-weighted than either; that is a deliberate starting point, "
                    "not a tuned value.");

                ImGui::SliderFloat("High percentile", &exposure.highPercentile, 0.5f, 1.0f, "%.3f");
                ImGui::SameLine();
                HelpMarker(
                    "Fraction of samples kept from the bottom, i.e. everything ABOVE this is "
                    "thrown away as 'too bright to meter'.\n\n"
                    "THIS IS THE GLINT KNOB. A sun-glint field can be 10-20% of the frame, so with "
                    "0.95 (discarding only the top 5%) most of the glint still counts as scene "
                    "brightness and the camera closes down, crushing the shaded side of the "
                    "island. Lowering it to ~0.80 discards the whole specular field and meters the "
                    "water and sand instead.\n\n"
                    "Narkowicz recommends discarding 2-20% of the brightest; Unreal's histogram "
                    "default is around 98.3% for its own metering band. Sweep it while looking "
                    "into the sun over water -- that is the view it exists for.");

                ImGui::SeparatorText("Weight mask (centre-weighted metering)");
                ImGui::SliderFloat("Mask strength", &exposure.meterMaskStrength, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                HelpMarker(
                    "How much screen position affects a sample's weight in the meter. 0 = off, "
                    "every sample counts equally (bit-identical to no mask).\n\n"
                    "This is the PRINCIPLED fix for 'the sun or the sky drags the whole frame'. "
                    "The percentile sliders throw bright samples away no matter where they are; "
                    "this instead de-weights the parts of the frame that are not the subject, which "
                    "is what a camera's centre-weighted meter does and what Unreal stores in a mask "
                    "texture.\n\n"
                    "Weights are floored at 0.05, so the frame edges still count a little rather "
                    "than dropping out entirely.");
                ImGui::BeginDisabled(exposure.meterMaskStrength <= 0.0f);
                ImGui::SliderFloat("Mask inner radius", &exposure.meterMaskInnerRadius, 0.0f, 1.5f, "%.2f");
                ImGui::SameLine();
                HelpMarker("Everything inside this radius keeps full weight. Fraction of the "
                           "half-diagonal, so 1.0 is the frame corner.");
                ImGui::SliderFloat("Mask outer radius", &exposure.meterMaskOuterRadius, 0.0f, 1.5f, "%.2f");
                ImGui::SameLine();
                HelpMarker("Weight falls off to the floor by this radius. Must be larger than the "
                           "inner one -- an inverted pair would weight the EDGES instead.");
                if (exposure.meterMaskOuterRadius <= exposure.meterMaskInnerRadius)
                {
                    exposure.meterMaskOuterRadius = exposure.meterMaskInnerRadius + 0.01f;
                }
                ImGui::SliderFloat("Sky bias", &exposure.meterMaskSkyBias, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                HelpMarker(
                    "Extra de-weighting of the TOP half of the frame, fading in from the midline. "
                    "A purely radial mask still lets the sky dominate the moment the camera tilts "
                    "up -- and in an exterior the sky is almost always the brightest thing and "
                    "almost never the subject. 0 = none.");
                ImGui::EndDisabled();

                ImGui::SeparatorText("Adaptation");
                ImGui::SliderFloat("Speed up (stops/s)", &exposure.speedUp, 0.0f, 20.0f, "%.2f");
                ImGui::SameLine();
                HelpMarker(
                    "How fast the camera CLOSES when the scene gets brighter, in stops per second. "
                    "This is the direction that fires when you turn to face the sun.\n\n"
                    "The eye's light adaptation is much faster than its dark adaptation, which is "
                    "why the default is 3 against 1. Set to 0 to freeze adaptation in this "
                    "direction entirely.\n\n"
                    "The step is capped per frame by a 0.1 s delta clamp, so a breakpoint or a long "
                    "level load cannot resolve into one instant jump when rendering resumes.");

                ImGui::SliderFloat("Speed down (stops/s)", &exposure.speedDown, 0.0f, 20.0f, "%.2f");
                ImGui::SliderFloat("Ease-in distance (stops)", &exposure.adaptationStartDistance,
                    0.05f, 8.0f, "%.2f");
                ImGui::SameLine();
                HelpMarker(
                    "Where adaptation switches from linear to exponential. Further from the target "
                    "than this it runs at the constant rate above, so a big transition takes a "
                    "predictable, bounded time; inside it the last stretch eases in.\n\n"
                    "Without this, a purely linear adaptation arrives at full speed and stops dead, "
                    "which reads as a mechanical snap rather than as an eye settling. Matches "
                    "Unreal's r.EyeAdaptation.ExponentialTransitionDistance; 1.5 is their default.");
                ImGui::SliderFloat("Black bucket influence", &exposure.blackBucketInfluence,
                    0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                HelpMarker(
                    "Weight of the DARKEST histogram bucket. 1 = counts normally.\n\n"
                    "Lower it when a scene has large regions of pure black — an unlit interior, "
                    "letterboxing, geometry that never receives light — which would otherwise drag "
                    "the meter toward exposing for nothing.");
                ImGui::SameLine();
                HelpMarker(
                    "How fast the camera OPENS when the scene gets darker, in stops per second -- "
                    "walking into shade, or turning away from the sun.\n\n"
                    "Slower than 'up' by default (1 vs 3) because that is how the eye behaves and "
                    "because a fast open makes shaded areas visibly 'bloom' open, which reads as a "
                    "bug rather than as vision.");

                ImGui::SeparatorText("Range");
                ImGui::SliderFloat("Min EV100", &exposure.minEv100, -16.0f, 20.0f, "%.2f");
                ImGui::SameLine();
                HelpMarker(
                    "Hard floor on the adapted EV. A LOW value lets the camera open wide, which "
                    "brightens dark scenes; raise it to stop the camera lifting night or deep "
                    "shade into flat grey.\n\n"
                    "This is a safety net, not a look. It cannot make different lighting "
                    "conditions read differently from each other -- that needs a compensation "
                    "curve, which this build does not have.");

                ImGui::SliderFloat("Max EV100", &exposure.maxEv100, -16.0f, 20.0f, "%.2f");
                ImGui::SameLine();
                HelpMarker(
                    "Hard ceiling on the adapted EV, i.e. how far the camera may close down.\n\n"
                    "LOWERING THIS IS THE BLUNT FIX FOR THE GLINT PROBLEM: cap it just above the "
                    "value the readout shows on a normal view and the camera physically cannot "
                    "crush the frame when you turn into the sun. Blunt because it clips the "
                    "response rather than fixing what is being metered -- prefer the high "
                    "percentile above for that.\n\n"
                    "The -6..16 default spans 22 stops, which clamps nothing at all. Treat the "
                    "defaults as 'off', not as 'tuned'.");
                if (exposure.maxEv100 < exposure.minEv100)
                {
                    std::swap(exposure.minEv100, exposure.maxEv100);
                }

                ImGui::BeginDisabled(exposure.autoExposure);
                ImGui::SliderFloat("Aperture f/", &exposure.apertureFStop, 1.0f, 32.0f, "%.1f");
                ImGui::SliderFloat("Shutter (s)", &exposure.shutterSpeedSec, 1.0f / 4000.0f, 1.0f, "%.5f",
                                   ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("ISO", &exposure.isoSensitivity, 25.0f, 6400.0f, "%.0f",
                                   ImGuiSliderFlags_Logarithmic);
                ImGui::EndDisabled();
                {
                    const float ev = render::Ev100FromCamera(exposure.apertureFStop,
                        exposure.shutterSpeedSec, exposure.isoSensitivity);
                    ImGui::TextDisabled("= EV100 %.2f   x%.5f", ev,
                                        render::ExposureMultiplierFromEv100(ev));
                }
                ImGui::SameLine();
                HelpMarker(
                    "Fixed exposure used when 'Auto exposure' is off. Higher EV = DARKER image "
                    "(a higher EV is a more closed camera); each +1 halves the brightness.\n\n"
                    "WARNING: this renderer's HDR is NOT photometric. Scene-referred linear values "
                    "here sit around 0.1-3, not the thousands of cd/m2 real luminance would give, "
                    "so EV100 is relative to an arbitrary engine scale. EV 0 is roughly the "
                    "authored look and auto settles near -0.3; the textbook daylight value of 10 "
                    "renders a black screen. Any future 'sun intensity in lux' UI has to establish "
                    "a scene-to-luminance scale first.");

                ImGui::EndDisabled();

                // P3B local exposure. It belongs with the CAMERA, not the grade: it decides how
                // many stops a REGION of the frame receives. It sat under the colour pipeline until
                // the user pointed out the mismatch -- that placement was about where the shader
                // evaluates it, not about what it is.
                ImGui::SeparatorText("Local exposure");
                ImGui::SliderFloat("Local highlights", &exposure.localHighlightContrast, 0.1f, 2.0f, "%.3f");
                ImGui::SameLine();
                HelpMarker(
                    "Contrast scale for everything brighter than middle grey, judged by the "
                    "BLURRED neighbourhood rather than the pixel. 1 = off.\n\n"
                    "BELOW 1 compresses: bright regions come down while their detail stays -- "
                    "sky and sunlit sand keep texture instead of racing to white. Measured on "
                    "sun_glint, 0.55 cut clipping 65x for a 2% median move.\n\n"
                    "ABOVE 1 EXPANDS, and on wind_test that is the more useful direction -- "
                    "this scene's problem is too little range, not too much. Either way it is "
                    "the one thing a global exposure cannot do: it changes exposure differently "
                    "in different parts of the same frame.");
                ImGui::SliderFloat("Local shadows", &exposure.localShadowContrast, 0.1f, 2.0f, "%.3f");
                ImGui::SameLine();
                HelpMarker("Same for everything darker than middle grey. Below 1 lifts shaded "
                           "regions without touching the lit ones -- the palm grove interior "
                           "opening up while the beach stays put. Above 1 deepens them instead, "
                           "and because it is per-neighbourhood it deepens WITHOUT crushing the "
                           "lit side, which a global contrast cannot manage.");
                ImGui::SliderFloat("Local detail", &exposure.localDetailStrength, 0.0f, 2.0f, "%.3f");
                ImGui::SameLine();
                HelpMarker("How much of the per-pixel detail survives the compression. 1 = all "
                           "of it, which is the point: compressing the base while passing "
                           "detail through is what keeps micro-contrast. Above 1 exaggerates "
                           "it and starts to look artificial.");
                ImGui::SliderFloat("Local hl threshold", &exposure.localHighlightThreshold, 0.0f, 4.0f, "%.2f");
                ImGui::SameLine();
                HelpMarker("Stops above middle grey before highlight compression starts. Keeps "
                           "mid-tones -- usually the subject -- untouched.");
                ImGui::SliderFloat("Local sh threshold", &exposure.localShadowThreshold, 0.0f, 4.0f, "%.2f");
                ImGui::SameLine();
                HelpMarker("Same below middle grey.");
                ImGui::PushID("localPresets");
                if (ImGui::SmallButton("Off"))
                {
                    exposure.localHighlightContrast = 1.0f; exposure.localShadowContrast = 1.0f;
                    exposure.localDetailStrength = 1.0f;
                    exposure.localHighlightThreshold = 0.0f; exposure.localShadowThreshold = 0.0f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Gentle"))
                {
                    exposure.localHighlightContrast = 0.85f; exposure.localShadowContrast = 0.9f;
                    exposure.localDetailStrength = 1.0f;
                    exposure.localHighlightThreshold = 0.5f; exposure.localShadowThreshold = 0.5f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Strong"))
                {
                    exposure.localHighlightContrast = 0.65f; exposure.localShadowContrast = 0.75f;
                    exposure.localDetailStrength = 1.1f;
                    exposure.localHighlightThreshold = 0.25f; exposure.localShadowThreshold = 0.25f;
                }
                ImGui::SameLine();
                // The measured match to docs/ref/ref_wind_test.png. Expansion, not compression:
                // on this level the histogram was too NARROW (23.6x p99/p02 against the
                // reference's 53.6x), so the scales go above 1. At 1.35 the spread reaches
                // 41.4x and at 1.5 it reaches 52.9x, both still clipping 0.000% of the frame.
                if (ImGui::SmallButton("Expand (ref match)"))
                {
                    exposure.localHighlightContrast = 1.35f; exposure.localShadowContrast = 1.35f;
                    exposure.localDetailStrength = 1.0f;
                    exposure.localHighlightThreshold = 0.0f; exposure.localShadowThreshold = 0.0f;
                }
                ImGui::PopID();
                ImGui::SameLine();
                HelpMarker(
                    "Off / Gentle / Strong COMPRESS the range (scales below 1) -- reach for "
                    "those when a frame clips, e.g. looking into the sun over water.\n\n"
                    "Expand does the reverse and is the one measured against "
                    "docs/ref/ref_wind_test.png: on the overview view it takes the p99/p02 "
                    "spread from 23.6x to 41.4x (1.5 on both scales reaches 52.9x against the "
                    "reference's 53.6x), and clipping stays at 0.000% the whole way. The median "
                    "drops slightly -- about +0.1 EV of compensation puts it back.\n\n"
                    "Watch the horizon and the island silhouette for halos when pushing past "
                    "1.5; the base layer is a blur, not a bilateral grid.");

                
                // P3: the display transform. Lives in the same tab because curve and exposure are
                // judged together -- changing one without seeing the other is how you end up
                // "fixing" a tone curve problem with exposure.
                ImGui::SeparatorText("Tone curve");
                {
                    render::ColorPipelineSettings& color = scene.ColorPipelineRef();
                    int curve = static_cast<int>(color.toneCurve);
                    if (ImGui::RadioButton("Legacy (ACES fit)", &curve, 0)) { color.toneCurve = render::ToneCurve::LegacyAces; }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("AgX", &curve, 1)) { color.toneCurve = render::ToneCurve::AgX; }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Filmic (Unreal)", &curve, 2)) { color.toneCurve = render::ToneCurve::Filmic; }
                    ImGui::SameLine();
                    HelpMarker(
                        "LEGACY: the Narkowicz ACES fit plus pow(1/2.2), exactly what shipped "
                        "before. It skews hue as it clips -- saturated cyans slide toward white "
                        "with a magenta cast -- which is visible on our lagoon water. Kept so a "
                        "suspected regression can be A/B'd against the curve; selecting it is "
                        "bit-identical to the pre-P3 image.\n\n"
                        "AGX: log-encode, sigmoid, and a chroma-attenuating inset matrix, then the "
                        "real sRGB transfer function instead of pow(1/2.2). Highlights desaturate "
                        "the way film does rather than snapping to a flat colour, and the sRGB "
                        "curve's linear toe stops near-black being lifted -- our blacks measured "
                        "MILKIER than the reference's, which is what that fixes.\n\n"
                        "FILMIC: Unreal's own parameterised film curve, with their five controls "
                        "below. Solved so 0.18 in gives 0.18 out however the knobs are set, which "
                        "keeps it from fighting the exposure solve (that targets the same 0.18). "
                        "The tonal response is faithful to theirs; the ACES glow module, red "
                        "modifier and AP1 working space are not included, because those headers "
                        "were not in the reference drop.");

                    ImGui::BeginDisabled(color.toneCurve != render::ToneCurve::Filmic);
                    ImGui::SliderFloat("Film slope", &color.filmSlope, 0.1f, 1.5f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("Steepness of the straight middle section, i.e. overall contrast of "
                               "the curve. Unreal's default is 0.88.");
                    ImGui::SliderFloat("Film toe", &color.filmToe, 0.0f, 1.0f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("How much the shadows roll off. Higher keeps more shadow detail and "
                               "lifts the black end; lower crushes toward black sooner. Default 0.55.");
                    ImGui::SliderFloat("Film shoulder", &color.filmShoulder, 0.0f, 1.0f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("How much the highlights roll off. Higher holds highlight detail "
                               "longer before white; lower reaches white sooner. Default 0.26.");
                    ImGui::SliderFloat("Film black clip", &color.filmBlackClip, 0.0f, 0.5f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("How far below zero the toe may reach before clipping. Default 0.");
                    ImGui::SliderFloat("Film white clip", &color.filmWhiteClip, 0.0f, 0.5f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("How far above one the shoulder may reach. Default 0.04.");
                    if (ImGui::SmallButton("Unreal defaults"))
                    {
                        color.filmSlope = 0.88f; color.filmToe = 0.55f; color.filmShoulder = 0.26f;
                        color.filmBlackClip = 0.0f; color.filmWhiteClip = 0.04f;
                    }
                    ImGui::EndDisabled();

                    ImGui::BeginDisabled(color.toneCurve != render::ToneCurve::AgX);
                    ImGui::SliderFloat("Slope", &color.agxSlope, 0.0f, 2.0f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("Gain applied inside the AgX log domain before the power. 1.0 is "
                               "neutral. Think 'exposure of the grade', not of the camera.");
                    ImGui::SliderFloat("Power", &color.agxPower, 0.1f, 2.5f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("Contrast. Above 1 deepens shadows and firms up the midtones; the "
                               "reference 'punchy' look uses about 1.35. This is the knob that "
                               "makes the image read as vivid rather than flat.");
                    ImGui::SliderFloat("Saturation", &color.agxSaturation, 0.0f, 2.0f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("Chroma multiplier around luma, applied in the log domain so it "
                               "does not reintroduce the clipping AgX just removed. 1.0 neutral; "
                               "the 'punchy' look uses about 1.4.");
                    if (ImGui::SmallButton("Neutral"))
                    {
                        color.agxSlope = 1.0f; color.agxPower = 1.0f; color.agxSaturation = 1.0f;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Punchy"))
                    {
                        color.agxSlope = 1.0f; color.agxPower = 1.35f; color.agxSaturation = 1.4f;
                    }
                    ImGui::EndDisabled();

                    // P3C. This is the half of Unreal's film pipeline that actually produces the
                    // look; their curve on its own is not what makes their images punchy.
                    ImGui::SeparatorText("Colour grade (pre-curve, works on BOTH curves)");
                    ImGui::SliderFloat("Grade saturation", &color.gradeSaturation, 0.0f, 2.5f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker(
                        "Chroma around luma, applied in scene-referred LINEAR before the tone "
                        "curve — the same place Unreal bakes it into its LUT. 1 = unchanged, 0 = "
                        "greyscale.\n\n"
                        "Grading before the curve rather than after is the whole point: after it, "
                        "you are pushing display code values whose range has already been "
                        "compressed, and saturation stops behaving predictably near the top.");
                    ImGui::SliderFloat("Grade contrast", &color.gradeContrast, 0.25f, 2.5f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker(
                        "Contrast pivoted on middle grey (0.18), so raising it deepens shadows and "
                        "lifts highlights while the midtones stay put.\n\n"
                        "Pivoting on zero instead would just darken everything as contrast rises — "
                        "which is exactly the trap the AgX 'punchy' preset above falls into, and "
                        "why that preset crushes the image.");
                    ImGui::SliderFloat("Grade gamma", &color.gradeGamma, 0.25f, 2.5f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("Midtone weighting. Above 1 lifts midtones without moving black or "
                               "white as much as gain would.");
                    ImGui::SliderFloat("Grade gain", &color.gradeGain, 0.0f, 2.5f, "%.3f");
                    ImGui::SameLine();
                    HelpMarker("Plain multiplier. Overlaps with exposure compensation — prefer the "
                               "camera's compensation for overall brightness and keep this for the "
                               "grade, or the two will fight.");
                    ImGui::SliderFloat("Grade offset", &color.gradeOffset, -0.25f, 0.25f, "%.4f");
                    ImGui::SameLine();
                    HelpMarker("Plain lift. Small positive values give the faded, milky-black film "
                               "look; negative values crush the black point.");
                    // Presets as data rather than a wall of buttons. "Vivid" is the combination
                    // that was measured against the reference photograph (median 0.184 / p02
                    // 0.0250 / p99 0.776 / chroma 0.434 against its 0.196 / 0.0152 / 0.814 /
                    // 0.445), not a guess; the others are steps around it.
                    struct GradePreset
                    {
                        const char* name;
                        float saturation, contrast, gamma, gain, offset;
                        const char* tip;
                    };
                    static const GradePreset kGradePresets[] = {
                        { "Neutral", 1.00f, 1.00f, 1.00f, 1.00f, 0.000f,
                          "No grading at all. Bit-identical to the ungraded image -- the shader "
                          "skips the whole block when every value is neutral." },
                        { "Vivid",   1.40f, 1.25f, 1.00f, 1.00f, 0.000f,
                          "The measured match to the reference photograph.\n\n"
                          "Set exposure compensation to suit the CURVE, not this preset: about "
                          "-0.4 EV on Legacy, and 0.0 on Filmic (which is darker to begin with, so "
                          "the two would otherwise stack and land a full stop under)." },
                        { "Punchy",  1.20f, 1.45f, 1.00f, 1.00f, 0.000f,
                          "Contrast-forward rather than colour-forward: deeper blacks and harder "
                          "highlights, with saturation left closer to neutral." },
                        { "Filmic",  1.10f, 1.05f, 1.00f, 1.00f, 0.015f,
                          "The faded look -- a small positive offset lifts the black point off "
                          "zero, which is what makes film prints read as soft rather than digital." },
                        { "Warm sand", 1.30f, 1.15f, 1.10f, 1.00f, 0.000f,
                          "Vivid with the midtones opened up, so lit sand and foliage keep detail "
                          "instead of racing to the highlight rolloff.\n\n"
                          "The gamma lift makes this brighter than Vivid at the same setting: on "
                          "Filmic it lands on the reference at about -0.15 EV of compensation, and "
                          "roughly a third of a stop above it at 0.0." },
                        { "Flat",    0.85f, 0.90f, 1.00f, 1.00f, 0.000f,
                          "Deliberately washed out. Useful for judging LIGHTING rather than look: "
                          "with contrast and saturation pulled down, shading errors stop hiding "
                          "behind the grade." },
                    };
                    // Namespaced: "Neutral" and "Punchy" also exist as AgX-look buttons above, and
                    // ImGui derives a widget's ID from its label, so without this the two pairs
                    // collide and it (rightly) complains about conflicting IDs.
                    ImGui::PushID("gradePresets");
                    for (int i = 0; i < static_cast<int>(std::size(kGradePresets)); ++i)
                    {
                        const GradePreset& preset = kGradePresets[i];
                        if (i != 0) { ImGui::SameLine(); }
                        if (ImGui::SmallButton(preset.name))
                        {
                            color.gradeSaturation = preset.saturation;
                            color.gradeContrast = preset.contrast;
                            color.gradeGamma = preset.gamma;
                            color.gradeGain = preset.gain;
                            color.gradeOffset = preset.offset;
                        }
                        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", preset.tip); }
                    }
                    ImGui::PopID();
                }

                ImGui::SeparatorText("Level");
                ImGui::TextDisabled("These are runtime values. To keep them, copy into the level's");
                ImGui::TextDisabled("\"cameraExposure\" section (or edit it in the editor inspector).");
                if (ImGui::SmallButton("Copy JSON to clipboard"))
                {
                    const render::ColorPipelineSettings& color = scene.GetColorPipeline();
                    char json[1024];
                    std::snprintf(json, sizeof(json),
                        "\"colorPipeline\": {\n"
                        "  \"toneCurve\": \"%s\",\n"
                        "  \"agxSlope\": %.4f,\n"
                        "  \"agxPower\": %.4f,\n"
                        "  \"agxSaturation\": %.4f,\n"
                        "  \"gradeSaturation\": %.4f,\n"
                        "  \"gradeContrast\": %.4f,\n"
                        "  \"gradeGamma\": %.4f,\n"
                        "  \"gradeGain\": %.4f,\n"
                        "  \"gradeOffset\": %.4f\n"
                        "},\n"
                        "\"cameraExposure\": {\n",
                        color.toneCurve == render::ToneCurve::AgX ? "agx" : "legacy",
                        color.agxSlope, color.agxPower, color.agxSaturation,
                        color.gradeSaturation, color.gradeContrast, color.gradeGamma,
                        color.gradeGain, color.gradeOffset);
                    const size_t used = std::strlen(json);
                    std::snprintf(json + used, sizeof(json) - used,
                        "  \"enabled\": %s,\n"
                        "  \"autoExposure\": %s,\n"
                        "  \"compensationEv\": %.4f,\n"
                        "  \"manualCompensationEv\": %.4f,\n"
                        "  \"minEv100\": %.4f,\n"
                        "  \"maxEv100\": %.4f,\n"
                        "  \"lowPercentile\": %.4f,\n"
                        "  \"highPercentile\": %.4f,\n"
                        "  \"speedUp\": %.4f,\n"
                        "  \"speedDown\": %.4f,\n"
                        "  \"apertureFStop\": %.4f,\n"
                        "  \"shutterSpeedSec\": %.5f,\n"
                        "  \"isoSensitivity\": %.1f\n"
                        "}",
                        exposure.enabled ? "true" : "false",
                        exposure.autoExposure ? "true" : "false",
                        exposure.compensationEv, exposure.manualCompensationEv,
                        exposure.minEv100, exposure.maxEv100,
                        exposure.lowPercentile, exposure.highPercentile,
                        exposure.speedUp, exposure.speedDown, exposure.apertureFStop,
                        exposure.shutterSpeedSec, exposure.isoSensitivity);
                    ImGui::SetClipboardText(json);
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Debug"))
            {
                const std::string_view activeLevelName = levelManager.GetActiveLevelName();
                ImGui::Text("Active level: %.*s", static_cast<int>(activeLevelName.size()), activeLevelName.data());
                const std::string_view activeLevelPath = levelManager.GetActiveLevelSourcePath();
                if (!activeLevelPath.empty())
                {
                    ImGui::Text("Active path: %.*s", static_cast<int>(activeLevelPath.size()), activeLevelPath.data());
                }

                if (!levelListScanned_)
                {
                    RefreshLevelList();
                }

                const auto requestLevelPathChange = [&](const std::string& path)
                {
                    const std::string normalizedPath = NormalizeLevelPath(path);
                    if (normalizedPath.empty())
                    {
                        levelChangeStatus_ = "Level path is empty";
                        return;
                    }
                    if (!LevelFileExists(normalizedPath))
                    {
                        levelChangeStatus_ = "Level file not found: " + normalizedPath;
                        return;
                    }

                    SetTextBuffer(levelPathBuffer_, normalizedPath);
#if WITH_EDITOR
                    if (editorController.RequestOpenLevelPath(levelManager, normalizedPath, preserveCameraOnLevelChange_))
                    {
                        levelChangeStatus_ = "Queued " + normalizedPath;
                    }
                    else
                    {
                        levelChangeStatus_ = "Could not queue " + normalizedPath;
                    }
#else
                    LevelLoadOptions loadOptions;
                    loadOptions.preserveCameraTransform = preserveCameraOnLevelChange_;
                    levelManager.RequestLevelPathChange(normalizedPath, loadOptions);
                    levelChangeStatus_ = "Queued " + normalizedPath;
#endif
                };

                const char* preview = levelPathBuffer_[0] ? levelPathBuffer_ : "Select level";
                if (ImGui::BeginCombo("JSON level", preview))
                {
                    for (const std::string& levelPath : availableLevelPaths_)
                    {
                        const bool selected = NormalizeLevelPath(levelPathBuffer_) == levelPath;
                        if (ImGui::Selectable(levelPath.c_str(), selected))
                        {
                            SetTextBuffer(levelPathBuffer_, levelPath);
                        }
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh levels"))
                {
                    RefreshLevelList();
                }
                if (ImGui::Button("Load JSON Level"))
                {
                    requestLevelPathChange(levelPathBuffer_);
                }
                ImGui::SameLine();
                if (ImGui::Button("Reload JSON Level"))
                {
                    requestLevelPathChange(levelPathBuffer_);
                }
                ImGui::Checkbox("Preserve camera transform", &preserveCameraOnLevelChange_);
                if (!levelChangeStatus_.empty())
                {
                    ImGui::TextDisabled("%s", levelChangeStatus_.c_str());
                }

                ImGui::Separator();

                bool wireframe = renderer.GetWireframeMode();
                if (ImGui::Checkbox("Wireframe", &wireframe))
                {
                    renderer.SetWireframeMode(wireframe);
                }
                bool textureViewerOpen = textureDebugViewer_.IsOpen();
                // Render-resolution targets are re-sampled at a different sub-pixel offset every
                // frame, so a half-res intermediate (GTAO, SSR) shimmers while you stare at it.
                // This holds the grid still. DLSS needs that jitter, so its own output loses its
                // anti-aliasing while this is on — hence the warning rather than a silent toggle.
                {
                    bool jitterPaused = renderer.IsJitterPaused();
                    ImGui::BeginDisabled(!renderer.IsDlssActive());
                    if (ImGui::Checkbox("Pause DLSS jitter (steady preview)", &jitterPaused))
                    {
                        renderer.SetJitterPaused(jitterPaused);
                    }
                    ImGui::EndDisabled();
                    if (jitterPaused && renderer.IsDlssActive())
                    {
                        ImGui::TextDisabled("  DLSS is reconstructing without jitter: expect aliasing.");
                    }
                }
                if (ImGui::Checkbox("Texture inspector [F4]", &textureViewerOpen))
                {
                    textureDebugViewer_.SetOpen(textureViewerOpen);
                }
                bool oceanControlsOpen = oceanControlsWindow_.IsOpen();
                if (ImGui::Checkbox("Ocean controls [F7]", &oceanControlsOpen))
                {
                    oceanControlsWindow_.SetOpen(oceanControlsOpen);
                }
#if WITH_EDITOR
                bool levelEditorOpen = editorController.IsOpen();
                if (ImGui::Checkbox("Level Editor [F2]", &levelEditorOpen))
                {
                    editorController.SetOpen(levelEditorOpen);
                }
#endif
                // Mirrors SceneRenderer::PickDebugTexTarget — keep the order identical.
                static const char* kDebugTexLabels[] = {
                    "Cascade shadow atlas", "GTAO raw", "GTAO denoised", "GTAO temporal",
                    "GTAO upsampled", "HZB furthest (AO + UE SSR)", "Scene depth", "HZB closest (debug/P9)",
                    "SSR hit mask (alpha)" };

                ImGui::SeparatorText("Fullscreen debug view");
                // Picking a target TURNS THE VIEW ON. Two separate controls where one is useless
                // without the other is not two controls, it is one control and a trap: the combo
                // silently did nothing until the checkbox above it was found.
                if (ImGui::Combo("Show", &settings.debugTexTarget, kDebugTexLabels,
                                 IM_ARRAYSIZE(kDebugTexLabels)))
                {
                    settings.debugTexMode = true;
                }
                DevHelp("Replaces the whole frame with the chosen render target. Picking one here "
                        "switches the view on; use Off below to get the scene back.");

                ImGui::BeginDisabled(settings.debugTexTarget != 5 && settings.debugTexTarget != 7);
                ImGui::SliderInt("HZB mip", &settings.debugTexMip, 0, 12);
                ImGui::EndDisabled();
                DevHelp("Which level of the depth pyramid to show. Mip 0 is half the render "
                        "resolution, and each level halves again. Depth-like targets go through a "
                        "monotone pow() stretch, or reversed-Z would blit as a black rectangle. "
                        "Furthest is what the AO horizon search reads; Closest is what the HiZ "
                        "reflection march reads, and it is only built while that technique is the "
                        "active reflection source.");

                if (settings.debugTexMode)
                {
                    if (ImGui::Button("Off (show the scene)"))
                    {
                        settings.debugTexMode = false;
                    }
                    ImGui::SameLine();
                    // Say what is actually on screen, including the grid size -- "GTAO raw" alone
                    // does not tell you that you are looking at a half-resolution target.
                    const unsigned halfW = (renderer.GetRenderWidth() + 1u) / 2u;
                    const unsigned halfH = (renderer.GetRenderHeight() + 1u) / 2u;
                    switch (settings.debugTexTarget)
                    {
                    case 1: case 2: case 3:
                        ImGui::Text("showing %s  (%ux%u, half res)",
                                    kDebugTexLabels[settings.debugTexTarget], halfW, halfH);
                        break;
                    case 4: case 6:
                        ImGui::Text("showing %s  (%ux%u, render res)",
                                    kDebugTexLabels[settings.debugTexTarget],
                                    renderer.GetRenderWidth(), renderer.GetRenderHeight());
                        break;
                    case 8:
                        ImGui::Text("showing %s  (%ux%u)  white = the ray hit something",
                                    kDebugTexLabels[settings.debugTexTarget],
                                    renderer.GetReflectionTextureWidth(),
                                    renderer.GetReflectionTextureHeight());
                        break;
                    case 5: case 7:
                        ImGui::Text("showing %s mip %d  (%ux%u)",
                                    settings.debugTexTarget == 5 ? "furthest" : "closest",
                                    settings.debugTexMip,
                                    std::max(1u, halfW >> settings.debugTexMip),
                                    std::max(1u, halfH >> settings.debugTexMip));
                        break;
                    default:
                        ImGui::Text("showing %s", kDebugTexLabels[settings.debugTexTarget]);
                        break;
                    }

                    // The AO targets are only written while the pass runs. Without this the view is
                    // whatever was last left in the texture, which reads as "the feature is broken".
                    if (settings.debugTexTarget >= 1 && settings.debugTexTarget <= 4 &&
                        !scene.GetGtao().enabled)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                            "GTAO is OFF (Render tab) - this target is stale, not empty.");
                    }
                }
                ImGui::Checkbox("Profiler overlay", &settings.showProfiler);
#if WITH_EDITOR
                ImGui::Checkbox("GPU instancing [F12]", &render::g_instancingEnabled);
#else
                ImGui::Checkbox("GPU instancing", &render::g_instancingEnabled);
#endif

                // (Mesh LOD enable/force + the selection boundaries moved to the "LOD" tab.)
                const Camera& camera = scene.CameraRef();
                const auto& camPos = camera.GetPosition();
                const auto& camDir = camera.GetDirection();
                ImGui::Text("Camera position: %.2f, %.2f, %.2f", camPos.x, camPos.y, camPos.z);
                ImGui::Text("Camera direction: %.2f, %.2f, %.2f", camDir.x, camDir.y, camDir.z);
                ImGui::Text("Move speed multiplier: %.2f", camera.GetMoveSpeedMult());
                ImGui::Text("Frame: %llu", static_cast<unsigned long long>(renderer.GetTotalFrameNumber()));

                ImGui::Separator();
                if (ImGui::TreeNodeEx("TextManager stats"))
                {
                    TextManager* textManager = renderer.GetTextManager();
                    bool textStatsEnabled = textManager->GetPerfStatsEnabled();
                    if (ImGui::Checkbox("Enable TextManager instrumentation", &textStatsEnabled))
                    {
                        textManager->SetPerfStatsEnabled(textStatsEnabled);
                    }
                    const TextManager::PerfStats& textStats = renderer.GetTextManager()->GetPerfStats();
                    ImGui::Text("Instrumentation: %s", textStatsEnabled ? "enabled" : "disabled");
                    ImGui::Text("Regions:%u backgrounds:%u AddText:%u AddTextf:%u cached:%u positional:%u",
                        textStats.regions, textStats.backgrounds, textStats.addTextCalls,
                        textStats.addTextfCalls, textStats.addCachedTextCalls, textStats.positionalTextCalls);
                    ImGui::Text("Lines direct:%u deferred:%u chars:%u direct glyphs:%u run glyphs:%u retarget verts:%u",
                        textStats.directLines, textStats.deferredLines, textStats.inputChars,
                        textStats.directGlyphs, textStats.runGlyphs, textStats.retargetedVertices);
                    ImGui::Text("AddText %.2fus  cached %.2fus  format %.2fus",
                        textStats.addTextUs, textStats.addCachedTextUs, textStats.formatUs);
                    ImGui::Text("GlyphRun %.2fus (%u)  direct emit %.2fus (%u)  run emit %.2fus (%u)",
                        textStats.buildGlyphRunUs, textStats.glyphRunBuilds,
                        textStats.directEmitUs, textStats.directEmitCalls,
                        textStats.runEmitUs, textStats.runEmitCalls);
                    ImGui::Text("Direct split reserve %.2fus  setup %.2fus  loop %.2fus",
                        textStats.directEmitReserveUs, textStats.directEmitSetupUs, textStats.directEmitLoopUs);
                    ImGui::Text("Retarget %.2fus  Build %.2fus  reserve %.2fus  regions %.2fus",
                        textStats.lineRetargetUs, textStats.buildUs,
                        textStats.buildReserveUs, textStats.buildRegionsUs);
                    ImGui::Text("Upload rect %.2fus  upload text %.2fus  Draw %.2fus  Begin %.2fus",
                        textStats.uploadRectUs, textStats.uploadTextUs, textStats.drawUs, textStats.beginUs);
                    ImGui::TreePop();
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("LOD"))
            {
                // Every mesh-LOD SELECTION control in one place (shadow-specific LOD lives with the
                // shadows: the per-view curve bias stays on the VSM tab). All of these apply live.
                ImGui::TextWrapped("Mesh LOD selection. Regular meshes pick a tier from "
                    "distance / instance radius (object-size-relative: a palm switches later than a "
                    "pebble); chunked terrain picks a tier PER CHUNK in metres, and its shadow "
                    "casters always match the drawn LOD by construction.");
                ImGui::Separator();

#if WITH_EDITOR
                ImGui::Checkbox("Mesh LOD [F10]", &render::g_lodEnabled);
#else
                ImGui::Checkbox("Mesh LOD", &render::g_lodEnabled);
#endif
                ImGui::BeginDisabled(!render::g_lodEnabled);
                // -1 = automatic (screen-size / cascade); 0..3 force that level on every mesh.
                static const char* kForcedLodLabels[] = { "Auto", "0 (full)", "1", "2", "3" };
                int forcedLodCombo = render::g_forcedLod + 1; // map -1..3 -> 0..4
                if (ImGui::Combo("Force LOD level", &forcedLodCombo, kForcedLodLabels, IM_ARRAYSIZE(kForcedLodLabels)))
                {
                    render::g_forcedLod = forcedLodCombo - 1;
                }

                ImGui::SeparatorText("Regular meshes (distance / radius)");
                ImGui::SliderFloat("LOD1 at ratio", &render::g_lodBound0, 2.0f, 60.0f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("distance / instance radius where LOD0 steps to LOD1. A 2 m-radius palm\n"
                                      "at ratio 15 switches at 30 m. +/-15%% hysteresis on every boundary; each\n"
                                      "boundary is forced at least 5%% past the previous one.");
                ImGui::SliderFloat("LOD2 at ratio", &render::g_lodBound1, 4.0f, 120.0f, "%.0f");
                ImGui::SliderFloat("LOD3 at ratio", &render::g_lodBound2, 8.0f, 240.0f, "%.0f");

                ImGui::SeparatorText("Chunked terrain (metres, per chunk)");
                ImGui::SliderFloat("Chunk LOD distance (m)", &render::g_chunkLodDist0, 24.0f, 400.0f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A terrain chunk closer than this (closest point of its box) draws AND\n"
                                      "casts LOD0; each 'factor' further steps one LOD coarser. The caster always\n"
                                      "matches the drawn LOD per chunk, so no setting here can cause the terrain\n"
                                      "self-shadow banding/phantom family -- this knob trades triangles for pop-in\n"
                                      "distance only.");
                ImGui::SliderFloat("Chunk LOD factor", &render::g_chunkLodDistFactor, 1.2f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Distance multiplier between LOD steps: LOD2 starts at distance*factor,\n"
                                      "LOD3 at distance*factor^2. +/-15%% hysteresis keeps a boundary chunk from\n"
                                      "flipping while the camera breathes.");
                ImGui::EndDisabled();

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("CSM"))
            {
                // S0: the enabler for the whole CSM improvement plan (docs/csm_improvement_plan.md).
                // Without a live readout there is nothing to judge the fit/density steps on, and
                // without live config there is nothing to A/B them against.
                ImGui::TextWrapped("Legacy cascaded shadow maps \xE2\x80\x94 the fast alternative to VSM. "
                    "Every number in the readout is what the cascade was actually built with this "
                    "frame (Scene::UpdateCascades), not a value the UI re-derived.");
                ImGui::Separator();

                bool legacyMode = !render::VsmActive();
                if (ImGui::Checkbox("Legacy CSM active [Ctrl+V]", &legacyMode))
                    render::g_shadowMode = legacyMode ? render::ShadowMode::Legacy : render::ShadowMode::VSM;
                if (!legacyMode)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                        "VSM drives directional shadows right now. The cascades below are still\n"
                        "computed every frame, but nothing samples them \xE2\x80\x94 the readout is live,\n"
                        "the picture is not. Uncheck the box above before judging any change.");

                CascadeShadowConfig& csmCfg = scene.CascadeConfig();

                ImGui::SeparatorText("Coverage");
                ImGui::SliderFloat("Max distance (m)", &csmCfg.maxDistance, 20.0f, 1000.0f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Far edge of cascade 3 \xE2\x80\x94 the hard shadow terminator.\n"
                                      "Shrinking it is the cheapest way to buy texel density everywhere\n"
                                      "(no extra memory, no extra rasterization).");
                ImGui::DragFloat4("Split distances (m)", csmCfg.sliceDistances.data(), 0.25f, 0.5f, 1000.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Far plane of cascades 0..3 in view space. BuildSplitScheme clamps them\n"
                                      "monotonic and caps them at max distance, so out-of-order values are safe.");

                ImGui::SeparatorText("Fit");
                ImGui::SliderFloat("Overlap (texels)", &csmCfg.overlapInTexels, 0.0f, 8.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Padding added to the fitted sphere radius to absorb the texel-snap shift,\n"
                                      "in CASCADE TEXELS. The snap moves the centre by at most one texel per axis,\n"
                                      "so 2 already leaves a full texel of slack, and the same number is correct\n"
                                      "for every cascade. At 2 the measured worst-case slack is 1.2 texels; 1.0 is\n"
                                      "still positive but thin, and by 0.5 the ortho no longer covers the snapped\n"
                                      "slice \xE2\x80\x94 the UpdateCascades assert fires in Debug, shadows clip at the\n"
                                      "cascade edge in Release. Watch 'R fit/pad': the two should nearly coincide.");
                ImGui::SliderFloat("Z padding (m)", &csmCfg.zPadding, 0.0f, 100.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Slack added past the far side of the light-space depth range.");
                ImGui::SliderFloat("Caster reach (m)", &csmCfg.casterReachWS, 0.0f, 400.0f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How far TOWARD the light the ortho near plane is pulled back so casters\n"
                                      "between the sun and the slice still render. This is what inflates zRange\n"
                                      "(and the D16 step) in the readout. Drop it to 0 and watch tall casters'\n"
                                      "shadows get clipped \xE2\x80\x94 that is the problem pancaking exists to solve.");

                ImGui::SeparatorText("Bias");
                ImGui::SliderFloat("Normal bias (texels)", &csmCfg.normalBiasInTexels, 0.0f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Receiver offset along its normal, in cascade texels (so it scales with\n"
                                      "each cascade's world texel size). Raise to kill acne, at the cost of\n"
                                      "detaching contact shadows.");
                ImGui::SliderFloat("Depth bias (texels)", &csmCfg.depthBiasInTexels, 0.0f, 8.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Sample-time depth offset, in cascade texels. The readout converts it to\n"
                                      "millimetres of peter-panning \xE2\x80\x94 that number is the one to minimise.");

                if (ImGui::Button("Reset CSM config to defaults"))
                    csmCfg = CascadeShadowConfig{};
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Back to the compile-time defaults in SceneRenderConfig.h \xE2\x80\x94 the\n"
                                      "baseline every plan step is measured against.");

                ImGui::SeparatorText("Debug");
                bool csmTint = render::g_csmDebugMode == render::CsmDebugMode::CascadeTint;
                if (ImGui::Checkbox("Cascade tint", &csmTint))
                    render::g_csmDebugMode = csmTint ? render::CsmDebugMode::CascadeTint
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
                if (ImGui::BeginTable("CsmReadout", 8, csmTableFlags))
                {
                    ImGui::TableSetupColumn("c");
                    ImGui::TableSetupColumn("slice (m)");
                    ImGui::TableSetupColumn("tile");
                    ImGui::TableSetupColumn("texel (mm)");
                    ImGui::TableSetupColumn("R fit/pad (m)");
                    ImGui::TableSetupColumn("zRange (m)");
                    ImGui::TableSetupColumn("D16 (mm)");
                    ImGui::TableSetupColumn("bias (mm)");
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
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("texel = world mm per shadow texel (lower is sharper).  R fit/pad = sphere\n"
                                    "radius before/after overlap.  D16 = quantization step of the 16-bit depth\n"
                                    "atlas over zRange.  bias = depth bias in world mm (= peter-panning).");

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("VSM"))
            {
                ImGui::TextWrapped("Virtual Shadow Maps (Rung 2, experimental) for local lights "
                    "(spot + point + glass). Legacy = CSM directional + spot/point atlas. Tunes "
                    "quality vs. cost live (no reallocation). Directional still uses CSM until Step 24.");
                ImGui::Separator();
                bool vsmMode = render::VsmActive();
                if (ImGui::Checkbox("VSM shadows enabled [Ctrl+V]", &vsmMode))
                    render::g_shadowMode = vsmMode ? render::ShadowMode::VSM : render::ShadowMode::Legacy;

                // Applies to BOTH modes (folds GPU-instanced casters into the indirect cull): ON =
                // GI casts in VSM + via indirect in Legacy; OFF = GI reverts to the Legacy CPU tail.
                ImGui::Checkbox("GPU-instanced casters -> indirect/VSM [Ctrl+G]", &render::g_giIndirectShadowsEnabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: GPU-instanced objects cast shadows in VSM (and via the indirect path in\n"
                                      "Legacy), dropping their CPU RenderShadow tail. OFF: Legacy CPU tail only (no VSM).");

                // Shadow LOD bias applies to BOTH Legacy cascades and VSM (it shifts the per-view caster
                // LOD the shadow passes rasterize), so it lives OUTSIDE the VSM-only disabled block below.
                // A change triggers a GPU-idle caster rebuild (Scene::ReconcileShadowLodBias) next frame.
                ImGui::SliderInt("Shadow LOD bias", &render::g_shadowLodBias, -2, 3);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ADDITIVE offset on the per-view shadow LOD. Each shadow view (CSM cascade /\n"
                                      "VSM clipmap level / local light) already picks a base LOD by its tier (near =\n"
                                      "fine, far = coarse); this shifts the whole curve. 0 = the tier curve alone;\n"
                                      "+ = coarser everywhere (cheaper), - = sharper. Shadows don't resolve fine\n"
                                      "geometry, so coarser is usually invisible. Changing it rebuilds casters (a hitch).");

                // (Chunked-terrain LOD selection moved to the "LOD" tab — it is a camera-LOD
                // control, not a shadow one; the caster follows the drawn LOD by construction.)

                ImGui::BeginDisabled(!render::VsmActive());

                ImGui::SliderFloat("LOD ref distance", &vsm::g_refDist, 1.0f, 40.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Smaller = coarser pages: fewer/faster + more stable, softer near.\n"
                                      "Larger = sharper near but more resident pages + higher render cost.");

                int ds = static_cast<int>(vsm::g_requestDownscale);
                if (ImGui::SliderInt("Request downscale", &ds, 1, 8))
                    vsm::g_requestDownscale = static_cast<std::uint32_t>(ds < 1 ? 1 : ds);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Screen sub-sampling for page discovery.\n"
                                      "1 = full res (best coverage, costliest); higher = cheaper, may miss pages.");

                int lru = static_cast<int>(vsm::g_lruThreshold);
                if (ImGui::SliderInt("LRU eviction frames", &lru, 1, 120))
                    vsm::g_lruThreshold = static_cast<std::uint32_t>(lru < 1 ? 1 : lru);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Frames a resident page survives unrequested before it is freed.");

                ImGui::SliderFloat("Clipmap base extent", &vsm::g_clipmapBaseExtent, 4.0f, 200.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Step 24f directional clipmap: finest level's world extent (level i = base*2^i).\n"
                                      "Smaller = sharper near shadows but less far coverage; larger = the reverse.");
                ImGui::SliderFloat("Clipmap depth bias", &vsm::g_clipmapDepthBias, 0.0f, 0.01f, "%.4f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Directional clipmap NDC depth bias at LEVEL 0 (0.0001 = 1.23 shadow texels).\n"
                                      "Constant in texels across levels when decay = 1, so its world size doubles per\n"
                                      "level -- raise it with decay < 1 or far thin shadows detach.");
                ImGui::SliderFloat("Depth bias decay /level", &vsm::g_clipmapDepthBiasDecay, 0.25f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("bias(L) = max(depthBias * decay^L, floor). 1.0 = legacy constant-in-texels;\n"
                                      "0.5 = constant WORLD-size bias (the near value everywhere). Lets the near bias\n"
                                      "rise against acne without peter-panning the far levels.");
                ImGui::SliderFloat("Depth bias floor (texels)", &vsm::g_clipmapDepthBiasFloorTexels, 0.0f, 1.5f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Lower bound of the decayed bias, in texels of the level actually sampled.\n"
                                      "The D32 pool has effectively no quantization floor, so 0 is legal -- the\n"
                                      "receiver-plane bias carries the slope; raise this only if residual acne shows.");
                ImGui::SliderFloat("Clipmap normal bias (UE units)", &vsm::g_clipmapNormalBias, 0.0f, 4.0f, "%.3f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Receiver offset along the normal, UE's r.Shadow.Virtual.NormalBias units\n"
                                      "(their default 0.5): scaled by distance-to-camera and the FOV, /1000 on the CPU.\n"
                                      "Values well above 0.5 are masking a caster/receiver geometry mismatch\n"
                                      "(terrain shadow LOD) -- see docs/terrain_shadow_chunking_plan.md.");

                ImGui::SliderFloat("Local lateral bias (texels)", &vsm::g_localLateralTexels, 0.0f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Spot + point VSM: surface-normal offset in shadow texels. ~1 texel.\n"
                                      "Higher = less acne but the shadow Peter-pans (lifts off the base).");
                ImGui::SliderFloat("Local depth push (texels)", &vsm::g_localDepthPushTexels, 0.0f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Spot + point VSM: along-the-light-ray depth push in shadow texels,\n"
                                      "slope-scaled by 1/N.L. The main acne knob; barely Peter-pans (depth-only).");

                ImGui::Checkbox("Resident-only render (faster, may flicker)", &vsm::g_residentIterOnly);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: render only pages a 3-frame-old snapshot says are resident (fewer CPU\n"
                                      "draws), but shadows blink for ~3 frames when the set changes (motion/churn).\n"
                                      "OFF: render the whole pool every frame (correct, ~4x the render CPU).\n"
                                      "Ignored while 'Single-draw page render' is on (that path skips nothing).");

                ImGui::Checkbox("Single-draw page render (dormant)", &vsm::g_pageDrawSingle);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: one ExecuteIndirect over every (page, group) arg instead of the 1024-page\n"
                                      "CPU loop; the per-page viewport becomes a VS clip remap + SV_ClipDistance page\n"
                                      "borders. Removes the resident-snapshot blink at no CPU cost. Needs the mega\n"
                                      "buffer. OFF: the per-page loop (A/B, and per-page inspection in PIX).\n"
                                      "Measured: CPU 0.182 -> 0.058 ms, GPU neutral, frame time unchanged\n"
                                      "(the pass records on a worker, so the CPU saving is off the hot path).");

                // Barrier plan step 7 prereq: this one is BOOT-ONLY (`--vsm-page-compact`).
                // It is the only config flag whose two paths rest a buffer in states that cannot
                // be combined — PageArgCount is UAV when off and INDIRECT_ARGUMENT when on, and
                // UAV is exclusive. Toggling it live would invalidate the canonical table the
                // barrier compile seeds from. The other page-draw flag needs no such treatment:
                // its two states are both reads and are declared as a union.
                ImGui::BeginDisabled();
                ImGui::Checkbox("Compact draw args (boot-only: --vsm-page-compact)", &vsm::g_pageDrawCompact);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: the setup CS appends only NON-EMPTY (page, group) records and the draw\n"
                                      "uses that counter as its count buffer, instead of walking all 1024 x groups\n"
                                      "fixed-layout records. Measured ~+0.017 ms GPU here: the walk it removes was\n"
                                      "already free, the atomic it adds lands in the setup CS. Kept for group-heavy\n"
                                      "scenes (records = pages x groups). No effect while single-draw is off.");

                ImGui::Checkbox("Page cache (experimental, off = net loss here)", &vsm::g_pageCaching);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: re-render only new / dynamic-overlapping pages; cached pages keep their\n"
                                      "depth. Measured no avg gain + worse spikes on this scene (cull-bound + dynamic\n"
                                      "teapots + gated clear slower than the hardware clear). Kept for a future coarse-skip.");

                ImGui::Checkbox("Log page stats to debug output", &vsm::g_logPageStats);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Periodically print \"[VSM] request ... | resident=...\" to DBWIN. Off by default\n"
                                      "(spammy); the live numbers above/below always update regardless.");

                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::TextDisabled("Pool: %u pages (%ux%u). refDist=%.1f downscale=%u lru=%u",
                    vsm::kPoolPageCount, vsm::kPoolTexels, vsm::kPoolTexels,
                    vsm::g_refDist, vsm::g_requestDownscale, vsm::g_lruThreshold);

                // --- Live page stats + physical-pool residency grid (kFrameCount-old snapshot). ---
                ImGui::Separator();
                ImGui::TextUnformatted("Pages");
                const VirtualShadowMap::DebugStats& vstats = scene.Vsm().Stats();
                if (!render::VsmActive())
                {
                    ImGui::TextDisabled("(enable VSM to sample live page stats)");
                }
                else if (!vstats.valid)
                {
                    ImGui::TextDisabled("(sampling\xE2\x80\xA6 move the camera to populate)");
                }
                else
                {
                    const float frac = static_cast<float>(vstats.resident) /
                                       static_cast<float>(vsm::kPoolPageCount);
                    char resLabel[64];
                    std::snprintf(resLabel, sizeof(resLabel), "%u / %u resident (%.0f%%)",
                        vstats.resident, vsm::kPoolPageCount, frac * 100.0f);
                    ImGui::ProgressBar(frac, ImVec2(240.0f, 0.0f), resLabel);
                    ImGui::Text("Requested %u  |  new this frame %u", vstats.requested, vstats.newAlloc);
                    if (vstats.fail > 0)
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.30f, 1.0f),
                            "Alloc fails: %u (pool oversubscribed \xE2\x80\x94 raise refDist/downscale or lower LOD)",
                            vstats.fail);
                    else
                        ImGui::TextDisabled("Alloc fails: 0");
                    ImGui::Text("Request LOD  L0=%u  L1=%u  L2=%u  L3=%u  L4=%u",
                        vstats.perLevel[0], vstats.perLevel[1], vstats.perLevel[2],
                        vstats.perLevel[3], vstats.perLevel[4]);

                    const std::vector<std::uint32_t>& owners = scene.Vsm().PhysOwnerSnapshot();
                    if (owners.size() == vsm::kPoolPageCount)
                    {
                        ImGui::Spacing();
                        ImGui::TextDisabled("Physical pool %ux%u pages \xE2\x80\x94 colour = owning shadow view:",
                            vsm::kPoolPagesPerAxis, vsm::kPoolPagesPerAxis);
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImVec2 origin = ImGui::GetCursorScreenPos();
                        const int   axis = static_cast<int>(vsm::kPoolPagesPerAxis);
                        const float cell = 9.0f, gap = 1.0f, pitch = cell + gap;
                        for (int py = 0; py < axis; ++py)
                        {
                            for (int px = 0; px < axis; ++px)
                            {
                                const std::uint32_t owner = owners[static_cast<size_t>(py) * axis + px];
                                ImU32 col;
                                if (owner == 0xFFFFFFFFu)
                                {
                                    col = IM_COL32(38, 38, 44, 255); // free
                                }
                                else
                                {
                                    const std::uint32_t view = owner / vsm::kPagesPerView;
                                    const bool clip = view >= vsm::kNumLocalVirtualViews; // directional clipmap
                                    const float hue = static_cast<float>(view % vsm::kMaxVirtualViews) /
                                                      static_cast<float>(vsm::kMaxVirtualViews);
                                    col = ImColor::HSV(hue, clip ? 0.85f : 0.55f, clip ? 1.0f : 0.85f);
                                }
                                const ImVec2 a(origin.x + px * pitch, origin.y + py * pitch);
                                dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), col, 1.5f);
                            }
                        }
                        const float gridPx = axis * pitch;
                        ImGui::Dummy(ImVec2(gridPx, gridPx));
                        if (ImGui::IsItemHovered())
                        {
                            const ImVec2 m = ImGui::GetIO().MousePos;
                            const int cx = static_cast<int>((m.x - origin.x) / pitch);
                            const int cy = static_cast<int>((m.y - origin.y) / pitch);
                            if (cx >= 0 && cx < axis && cy >= 0 && cy < axis)
                            {
                                const std::uint32_t owner = owners[static_cast<size_t>(cy) * axis + cx];
                                if (owner == 0xFFFFFFFFu)
                                    ImGui::SetTooltip("phys #%d (%d,%d): free", cy * axis + cx, cx, cy);
                                else
                                {
                                    const std::uint32_t view = owner / vsm::kPagesPerView;
                                    const char* kind = view < 8 ? "spot"
                                        : (view < vsm::kNumLocalVirtualViews ? "point face" : "directional clipmap");
                                    ImGui::SetTooltip("phys #%d (%d,%d)\nview %u (%s)",
                                        cy * axis + cx, cx, cy, view, kind);
                                }
                            }
                        }
                    }
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Bindings"))
            {
                const auto& descs = input.GetBindingDescs();
                if (descs.empty())
                {
                    ImGui::TextDisabled("No bindings loaded.");
                }
                else
                {
                    const ImGuiTableFlags tableFlags =
                        ImGuiTableFlags_Borders |
                        ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable |
                        ImGuiTableFlags_ScrollY;
                    const float tableHeight = ImGui::GetContentRegionAvail().y;
                    if (ImGui::BeginTable("BindingsTable", 2, tableFlags, ImVec2(0.0f, tableHeight)))
                    {
                        ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        for (const auto& desc : descs)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(desc.keys.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(desc.action.c_str());
                        }

                        ImGui::EndTable();
                    }
                }

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    open_ = open;

    textureDebugViewer_.Draw(renderer, GetOceanShoreDepthResource());
    oceanControlsWindow_.Draw(renderer);
    DrawTraceControls();
}
