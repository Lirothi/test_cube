// R10.4: Developer Controls > Sky (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/shadows/ShadowSettings.h"

void DeveloperWindow::DrawSkyTab(Scene& scene, SceneRenderSettings& settings)
{
    auto& sky = scene.SkyAtmosphereRef();
    ImGui::SeparatorText("Atmosphere LUTs");
    // No "Sky mode" here any more: which sky a level uses is authored in the level
    // (Inspector > Skybox > Procedural atmosphere, JSON `skybox.procedural`). A copy of
    // it in this window would be a second answer to the same question, and the one that
    // vanished on reload.
    // The atmosphere's LOOK settings -- is it procedural, how bright, what it feeds,
    // the medium's parameters -- are the level's, and live on the Sky Atmosphere object
    // in the outliner. What is left here is the debug views and the LUT preview, which
    // are session state and have no business in a level file.
    ImGui::TextDisabled("Sky: %s (level object: outliner > Sky Atmosphere)",
                        sky.mode != 0u ? "procedural atmosphere" : "HDRI cubemap");
    const auto d = scene.GetSunToward(); // T3: the time of day's sun, if the level has one
    constexpr float degrees = 180.0f / 3.14159265358979323846f;
    float elevation = std::asin(std::clamp(d.y, -1.0f, 1.0f)) * degrees;
    float azimuth = std::atan2(d.x, d.z) * degrees;
    bool sunChanged = ImGui::SliderFloat("Sun elevation", &elevation, -90.0f, 90.0f, "%.1f deg");
    sunChanged |= ImGui::SliderFloat("Sun azimuth", &azimuth, -180.0f, 180.0f, "%.1f deg");
    if (sunChanged)
    {
        sunElevationPlaying_ = false;
        sunElevationProgress_ = 0.0;
        const float e = elevation / degrees, a = azimuth / degrees;
        scene.SetSunToward(Math::float3(std::cos(e)*std::sin(a), std::sin(e), std::cos(e)*std::cos(a)));
    }
    ImGui::SeparatorText("Sun elevation animation");
    ImGui::BeginDisabled(sunElevationPlaying_);
    bool animationChanged = ImGui::DragFloat("Min elevation", &sunElevationMin_, 0.1f,
        -90.0f, 90.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
    if (animationChanged) sunElevationMax_ = std::max(sunElevationMax_, sunElevationMin_);
    if (ImGui::DragFloat("Max elevation", &sunElevationMax_, 0.1f,
        sunElevationMin_, 90.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp))
    {
        sunElevationMax_ = std::clamp(sunElevationMax_, sunElevationMin_, 90.0f);
        animationChanged = true;
    }
    animationChanged |= ImGui::DragFloat("Lerp time (seconds)", &sunElevationSeconds_, 0.1f,
        0.01f, 86400.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
    if (animationChanged) sunElevationProgress_ = 0.0;
    ImGui::EndDisabled();
    if (ImGui::Button(sunElevationPlaying_ ? "Stop##SunElevation" : "Play##SunElevation"))
    {
        if (sunElevationPlaying_) sunElevationPlaying_ = false;
        else
        {
            if (sunElevationProgress_ >= 1.0) sunElevationProgress_ = 0.0;
            const auto toSun = scene.GetSunToward();
            sunElevationAzimuth_ = std::atan2(static_cast<double>(toSun.x), static_cast<double>(toSun.z));
            sunElevationPlaying_ = true;
            ApplySunElevation(scene);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##SunElevation"))
    {
        sunElevationPlaying_ = false;
        sunElevationProgress_ = 0.0;
        const auto toSun = scene.GetSunToward();
        sunElevationAzimuth_ = std::atan2(static_cast<double>(toSun.x), static_cast<double>(toSun.z));
        ApplySunElevation(scene);
    }
    char progressText[32];
    std::snprintf(progressText, sizeof(progressText), "%.3f / 1.000", sunElevationProgress_);
    ImGui::ProgressBar(static_cast<float>(sunElevationProgress_), ImVec2(-1.0f, 0.0f), progressText);
    ImGui::TextWrapped("Linear min to max. Stop pauses; Play resumes. Reset stops at min. "
        "At 1, playback stops; Play starts a new pass. Runs while this window is hidden.");
    ImGui::Separator();
    ImGui::TextWrapped("Sun angles apply for this session. The disc's size is the level's "
                       "(Directional Light > Sun Disc > Light Source Angle).");
    ImGui::BeginDisabled(sky.mode == 0);
    ImGui::BeginDisabled(!sky.aerialPerspective);
    int aerialView = static_cast<int>(sky.aerialDebugView);
    const char* aerialViews[] = { "Scene", "Transmittance", "In-scattered light", "Depth slices" };
    if (ImGui::Combo("Aerial view", &aerialView, aerialViews, IM_ARRAYSIZE(aerialViews)))
        sky.aerialDebugView = static_cast<unsigned>(aerialView);
    ImGui::EndDisabled();
    ImGui::TextWrapped("Distant opaque geometry beyond the volumetric fog range. 32 x 32 x 16, 96 km.");
    ImGui::EndDisabled();
    ImGui::Checkbox("Build atmosphere LUTs", &sky.lutEnabled);
    int view = static_cast<int>(sky.lutDebugView);
    const char* views[] = { "Scene", "Transmittance", "Multi-scattering (x10)" };
    if (ImGui::Combo("View", &view, views, IM_ARRAYSIZE(views)))
        sky.lutDebugView = static_cast<unsigned>(view);
    ImGui::TextWrapped("A LUT view fills the viewport and builds the tables automatically. "
        "Choose Scene to return to normal rendering.");
    ImGui::TextDisabled("Transmittance: 256 x 64; multi-scattering: 32 x 32.");
    ImGui::TextWrapped("Preview settings apply for this session.");
    // Plan part C: the clouds' debug views. Their look lives on the Volumetric Cloud
    // object in the outliner; only the session's view selector is here.
    ImGui::SeparatorText("Volumetric clouds");
    auto& clouds = scene.VolumetricCloudRef();
    ImGui::TextDisabled("Clouds: %s (level object: outliner > Volumetric Cloud)",
                        clouds.enabled ? (sky.mode != 0u ? "on" : "authored, but the sky is a cubemap") : "none");
    ImGui::BeginDisabled(!clouds.enabled || sky.mode == 0u);
    int cloudView = static_cast<int>(clouds.debugView);
    const char* cloudViews[] = { "Scene", "Coverage (formation field)", "Transmittance", "Samples taken" };
    if (ImGui::Combo("Cloud view", &cloudView, cloudViews, IM_ARRAYSIZE(cloudViews)))
        clouds.debugView = static_cast<unsigned>(cloudView);
    ImGui::EndDisabled();
    ImGui::TextWrapped("Gray views of the half-res trace, display-linear with the calibration "
                       "ramp on top; the temporal resolve is bypassed while one is shown. "
                       "--set=cloud.debugView:0..3");

    // Project-wide, UE's r.VolumetricRenderTarget.Mode by its own numbers; index 0 = mode 1.
    static const char* const kCloudTraceModes[] = {
        "Half-res trace, full-res resolve (ours)", "Quarter-res trace, half-res resolve (UE mode 0)" };
    int traceMode = render::g_cloudTraceMode_game == 0u ? 1 : 0;
    if (GRAPHICS_CONTROL(CloudTraceMode, "cloudTraceMode",
            ImGui::Combo("Cloud trace", &traceMode, kCloudTraceModes, IM_ARRAYSIZE(kCloudTraceModes))))
    {
        render::g_cloudTraceMode_game = traceMode == 1 ? 0u : 1u;
    }
    DevHelp("Where the clouds' view march runs. Ours: a ray per half-res texel through one render "
            "pixel of its 2x2 a frame, rebuilt at the render resolution over four frames. UE mode 0 "
            "(their default): the same one level down -- a ray per quarter-res texel, rebuilt at half "
            "res, then upsampled bilaterally by depth over the scene. A quarter of the rays and of "
            "the rebuild, half the detail per axis. Recreates the cloud targets. "
            "--set=cloudTrace.mode:1|0");
    {
        float captureTexels = render::g_cloudCaptureTexels_game;
        if (GRAPHICS_CONTROL_WIDTH(CloudCaptureTexels, "cloudCaptureTexels", 105.0f,
                ImGui::SliderFloat("Cloud capture step (texels)", &captureTexels, 0.0f, 4.0f, "%.2f")))
        {
            render::g_cloudCaptureTexels_game = render::SanitizeCloudCaptureTexels(captureTexels);
        }
    }
    DevHelp("How far the clouds may drift, in texels of the sky's 128-texel cube where they move fastest "
            "(straight overhead at the layer's bottom), before the cube re-captures them -- the sky, the "
            "clouds in every mip and the lighting probes, ~0.35-0.4 ms a rebuild on wind_test. At 15 km/h "
            "and a 600 m base 0.5 is ~0.9 s. Moving 10 m or changing a cloud setting re-captures at once. "
            "0 = every 4 frames while the wind's clock moves (as before). --set=cloud.captureTexels");

    // Project-wide (graphics_settings.json), unlike the level-authored look above: a
    // performance switch, UE's cvar role. The readout under it is what the last frame
    // actually traced -- the proof the switch reached the GPU.
    static const char* const kCloudShadowUpdateModes[] = {
        "Every frame (UE)", "1/4 per frame (2x2)", "1/16 per frame (4x4)" };
    int updateMode = render::g_cloudShadowUpdateFrames_game >= 16u ? 2 : (render::g_cloudShadowUpdateFrames_game >= 4u ? 1 : 0);
    if (GRAPHICS_CONTROL(CloudShadowUpdateFrames, "cloudShadowUpdateFrames",
            ImGui::Combo("Cloud shadow update", &updateMode, kCloudShadowUpdateModes,
                         IM_ARRAYSIZE(kCloudShadowUpdateModes))))
    {
        render::g_cloudShadowUpdateFrames_game = updateMode == 2 ? 16u : (updateMode == 1 ? 4u : 1u);
    }
    DevHelp("How much of the cloud shadow map (512x512, a march through the layer per "
            "texel) is re-traced each frame. Every frame is UE's behaviour and costs about "
            "0.3 ms of GPU frame on wind_test. 1/4 and 1/16 re-trace an interleaved subset "
            "and keep the rest: the clouds move rigidly with the wind, well under a texel "
            "(78 m) between refreshes. A moved sun or map anchor, a cloud setting, a noise "
            "rebuild or a wind jump re-traces the whole map that frame. "
            "--set=cloudShadow.updateFrames:1|4|16");
    // The map march's cheap density: each is a texture tap per sample the view keeps.
    bool bilinearWeather = render::g_cloudShadowBilinearWeather_game;
    if (GRAPHICS_CONTROL(CloudShadowBilinearWeather, "cloudShadowBilinearWeather",
            ImGui::Checkbox("Cloud shadow: bilinear macro variation", &bilinearWeather)))
    {
        render::g_cloudShadowBilinearWeather_game = bilinearWeather;
    }
    DevHelp("The shadow map reads the macro variation's field (the weather map) with one bilinear "
            "tap instead of the view's 4-tap B-spline; only matters while the level's Macro "
            "Variation is above 0. The B-spline is there for a perspective artefact (facets drawn "
            "into streaks across distant clouds); the map is a top-down grid blurred 3x3 "
            "afterwards. Off = the map sees exactly the view's coverage. "
            "--set=cloudShadow.bilinearWeather:0|1");
    bool detailMean = render::g_cloudShadowDetailMean_game;
    if (GRAPHICS_CONTROL(CloudShadowDetailMean, "cloudShadowDetailMean",
            ImGui::Checkbox("Cloud shadow: erosion by its mean", &detailMean)))
    {
        render::g_cloudShadowDetailMean_game = detailMean;
    }
    DevHelp("The shadow map erodes the cloud by the cells volume's MEAN erosion instead of "
            "fetching it, and leaves out the fine detail -- what the clouds' own far self-shadow "
            "samples do. The cells are ~150 m, about four map texels. Measured on wind_test: "
            "0.01 ms saved for a visibly different shadow, so off by default. Off = the map "
            "sees the view's full erosion. --set=cloudShadow.detailMean:0|1");
    const render::CloudShadowUpdateStats& cloudStats = render::g_cloudShadowStats;
    if (cloudStats.tracedTexels == 0u)
    {
        ImGui::TextDisabled("No cloud shadow map was built last frame.");
    }
    else
    {
        ImGui::TextDisabled("Last frame traced %u of %u texels (%.1f%%); forced full traces %llu (last: %s)",
            cloudStats.tracedTexels, cloudStats.totalTexels,
            100.0 * cloudStats.tracedTexels / std::max(1u, cloudStats.totalTexels),
            static_cast<unsigned long long>(cloudStats.forcedFullTraces), cloudStats.lastForcedReason);
    }
}

#endif // !TC_SHIPPING
