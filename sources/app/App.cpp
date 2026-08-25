#include "app/App.h"
#include "rendering/core/RenderConstants.h" // P16.1 g_preExposureEnabled
#include "core/math/Math.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <mimalloc.h>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>
#include "vfx/WindState.h" // --shot-count phase series steps the frozen wind clock (g_windStep)

// Boot-level override; see App.h. Set by main.cpp from "--level=<path>".
std::string g_bootLevelPath;
bool  g_camOverride = false;
float g_camPos[3] = { 0.0f, 0.0f, 0.0f };
float g_camFly[2] = { 0.0f, 0.0f }; // "--cam-fly=x,z": constant camera drift in m/s (world XZ)
// "--cam-fly-delay=<sec>": hold the camera STILL for this long, then start the drift. Captures
// the motion-ONSET transient (converged temporal histories -> first frames of motion), which a
// from-boot drift can never show.
float g_camFlyDelay = 0.0f;
float g_camRot[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
// One-shot screenshot; see App.h. Set by main.cpp from "--shot=<path>" / "--shot-delay=<sec>".
std::string g_shotPath;
double g_shotDelaySec = 7.0;
// Phase series (see App.h): "--shot-count=<n> --shot-step=<sec> [--shot-interval=<sec>]".
int    g_shotCount = 1;
double g_shotStepSec = 0.0;
double g_shotIntervalSec = 1.0;
// Temporary VSM perf harness; see App.h.
std::string g_profDumpPath;
uint32_t g_traceFrames = 0;
// Boot upscaler mode; see App.h. Set by main.cpp from "--dlss=<mode>". -1 = compiled default.
int g_bootDlssMode = -1;
// Empty-HUD capture mode; see App.h. Set by main.cpp from "--no-hud".
bool g_hudHidden = false;
// Single-process settings sweep; see App.h. Set by main.cpp from "--sweep=<setting>:<v0>,...".
std::string g_sweepSetting;
std::vector<float> g_sweepValues;
// Fixed settings for the whole run; see App.h. Set by main.cpp from "--set=<name>:<value>;...".
std::vector<std::pair<std::string, float>> g_fixedSettings;

#include "app/levels/JsonLevel.h"
#include "rendering/lighting/Skybox.h" // --sweep=light.skyIntensity
#include "ocean/OceanRenderable.h"  // --set=ocean.contactFoam
#include "ocean/OceanSimulation.h"
#include "rendering/shadows/VirtualShadowMap.h" // --set=vsm.clipmap* (dev-window globals, headless)
#include "rendering/meshes/LodSelect.h" // --sweep=vsm.shadowLod* (docs/bug_shadow_lod_bias_perf.md)
#include "rendering/debug/LodDebugView.h" // --set=lod.debug (LOD selection debug view)
#include "rendering/core/Screenshot.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/core/RenderStats.h"
#if WITH_EDITOR
#include "editor/EditorController.h"
#endif

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

static void MiOut(const char* msg, void* /*arg*/) { OutputDebugStringA(msg); }

namespace
{
    constexpr const wchar_t* kLoadingScreenPath = L"data/loading_screen.jpg";

    class ScopedComInitialization
    {
    public:
        ScopedComInitialization()
            : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
            , uninitialize_(SUCCEEDED(hr_))
        {
        }

        ~ScopedComInitialization()
        {
            if (uninitialize_)
            {
                CoUninitialize();
            }
        }

        bool IsValid() const
        {
            return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
        }

    private:
        HRESULT hr_;
        bool uninitialize_;
    };

    HBITMAP LoadBitmapFromWicFile(const wchar_t* path, BITMAP& outInfo)
    {
        outInfo = {};

        ScopedComInitialization com;
        if (!com.IsValid())
        {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr))
        {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename(
            path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
        if (FAILED(hr))
        {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.GetAddressOf());
        if (FAILED(hr))
        {
            return nullptr;
        }

        UINT width = 0;
        UINT height = 0;
        hr = frame->GetSize(&width, &height);
        if (FAILED(hr) || width == 0 || height == 0 || width > static_cast<UINT>(INT_MAX) || height > static_cast<UINT>(INT_MAX) ||
            width > UINT_MAX / 4u || height > UINT_MAX / (width * 4u))
        {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(hr))
        {
            return nullptr;
        }

        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
        {
            return nullptr;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
        bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* pixels = nullptr;
        HDC screenDc = GetDC(nullptr);
        if (!screenDc)
        {
            return nullptr;
        }

        HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
        ReleaseDC(nullptr, screenDc);
        if (!bitmap || !pixels)
        {
            if (bitmap)
            {
                DeleteObject(bitmap);
            }
            return nullptr;
        }

        const UINT stride = width * 4u;
        const UINT byteCount = stride * height;
        hr = converter->CopyPixels(nullptr, stride, byteCount, static_cast<BYTE*>(pixels));
        if (FAILED(hr))
        {
            DeleteObject(bitmap);
            return nullptr;
        }

        GetObjectW(bitmap, sizeof(outInfo), &outInfo);
        return bitmap;
    }
}

App::~App()
{
    ReleaseLoadingScreen();
}

namespace
{
    // "--sweep": apply one value of the swept setting. Deliberately a flat if-chain over string
    // names rather than a reflection system -- this is a capture harness, and a name that does not
    // match should be loud and cheap to diagnose, not silently ignored.
    // Returns false for an unknown name so the caller can say so once.
    bool ApplySweepValue(Scene& scene, SceneRenderSettings& renderSettings,
                         const std::string& setting, float value)
    {
        render::CameraExposureSettings& e = scene.CameraExposureRef();
        render::ColorPipelineSettings& c = scene.ColorPipelineRef();

        // P4: the SCENE-side brightness knobs, so a sweep can measure them against the camera-side
        // ones. `light.exposure` is the field P4 exists to retire -- it multiplies sun AND ambient
        // in lighting_cs, which is a camera control wearing a light's name.
        if (setting == "light.exposure") { scene.DirectionalLightRef().SetExposure(value); return true; }
        if (setting == "light.ambient")  { scene.DirectionalLightRef().SetAmbient(value);  return true; }
        // P6B: the AO pass and its knobs, so a sweep can measure it before it is wired into
        // lighting. `gtao.enabled` is the A/B switch the plan requires before it becomes default.
        if (setting == "gtao.enabled")     { scene.GtaoRef().enabled = value != 0.0f; return true; }
        if (setting == "gtao.worldRadius") { scene.GtaoRef().worldRadius = value;     return true; }
        if (setting == "gtao.thickness")   { scene.GtaoRef().thickness = value;       return true; }
        if (setting == "gtao.intensity")   { scene.GtaoRef().intensity = value;       return true; }
        if (setting == "gtao.numAngles")   { scene.GtaoRef().numAngles = (uint32_t)std::max(1.0f, value); return true; }
        if (setting == "gtao.numSteps")    { scene.GtaoRef().numSteps = (uint32_t)std::max(1.0f, value);  return true; }
        // Items 3-5: one key per filter stage, so a sweep can isolate the denoise from the
        // temporal from the upsample instead of measuring the whole chain at once.
        if (setting == "gtao.denoise")     { scene.GtaoRef().denoise = value != 0.0f; return true; }
        if (setting == "gtao.temporal")    { scene.GtaoRef().temporal = value != 0.0f; return true; }
        if (setting == "gtao.filterRadius") { scene.GtaoRef().filterRadius = (uint32_t)std::max(0.0f, value); return true; }
        if (setting == "gtao.filterPlaneTolerance") { scene.GtaoRef().filterPlaneTolerance = value; return true; }
        if (setting == "gtao.temporalBlendWeight") { scene.GtaoRef().temporalBlendWeight = value; return true; }
        if (setting == "gtao.temporalClampRange") { scene.GtaoRef().temporalClampRange = value; return true; }
        if (setting == "gtao.useGBufferNormal") { scene.GtaoRef().useGBufferNormal = value != 0.0f; return true; }
        if (setting == "gtao.useHzb") { scene.GtaoRef().useHzb = value != 0.0f; return true; }
        if (setting == "gtao.hzbMipBias") { scene.GtaoRef().hzbMipBias = (uint32_t)std::max(0.0f, value); return true; }
        // P16.4. `skyRadius` at or below `worldRadius` switches the second horizon walk off, which
        // is the exact-no-op baseline for the A/B.
        if (setting == "gtao.skyRadius") { scene.GtaoRef().skyRadius = value; return true; }
        if (setting == "gtao.skyMipBias") { scene.GtaoRef().skyMipBias = (uint32_t)std::max(0.0f, value); return true; }
        // Mid-range intensity; 0 = the sky walk's compute path is off entirely (exact no-op).
        if (setting == "gtao.skyIntensity") { scene.GtaoRef().skyIntensity = std::max(0.0f, value); return true; }
        // The directional-clipmap knobs the Developer window already carries, exposed to the
        // harness. Shadow bias is judged by A/B and the GUI cannot be driven headlessly, so
        // without these a clipmap artifact can only be argued about, not measured. They are
        // process globals, not scene state -- deliberately, so they survive a level switch the
        // way the dev-window sliders do.
        if (setting == "vsm.clipmapDepthBias")  { vsm::g_clipmapDepthBias = value;  return true; }
        // Per-level depth-bias shaping (bias(L) = max(base * decay^L, floorTexels), see
        // VsmClipmapShadow) -- headless mirrors of the two dev-window sliders beside the base bias.
        if (setting == "vsm.clipmapDepthBiasDecay") { vsm::g_clipmapDepthBiasDecay = std::clamp(value, 0.01f, 1.0f); return true; }
        if (setting == "vsm.clipmapDepthBiasFloor") { vsm::g_clipmapDepthBiasFloorTexels = std::max(0.0f, value); return true; }
        if (setting == "vsm.clipmapNormalBias") { vsm::g_clipmapNormalBias = value; return true; }
        if (setting == "vsm.clipmapBaseExtent") { vsm::g_clipmapBaseExtent = std::max(0.1f, value); return true; }
        if (setting == "vsm.clipmapBlend") { vsm::g_clipmapBlendEnabled = value != 0.0f; return true; }
        if (setting == "vsm.clipmapBlendWidth") { vsm::g_clipmapBlendWidth = std::clamp(value, 0.0f, 0.5f); return true; }
        // The dev-window "Shadow LOD bias" slider, headless. A change triggers the same GPU-idle
        // caster rebuild the slider does (Scene::ReconcileShadowLodBias polls it) — which is the
        // point: the round-trip perf leak is only reproducible in ONE process
        // (docs/bug_shadow_lod_bias_perf.md §6).
        if (setting == "vsm.shadowLodBias") { render::g_shadowLodBias = (int)value; return true; }
        // Caster-vs-receiver LOD floor (LodSelect.h). Off = the old per-view-only caster LOD.
        if (setting == "vsm.pageCaching")  { vsm::g_pageCaching = value != 0.0f; return true; }
        if (setting == "vsm.windMaxLevel") { vsm::g_windAnimateMaxLevel = (std::uint32_t)std::max(0.0f, value); return true; }
        if (setting == "vsm.perInstanceCasterLod") { vsm::g_perInstanceCasterLod = value != 0.0f; return true; }
        if (setting == "vsm.shadowLodBiasNearTier") { render::g_shadowLodBiasNearTier = value != 0.0f; return true; }
        if (setting == "vsm.shadowLodTierStride")
        {
            render::g_shadowLodTierStride = std::clamp((int)value, 1, 8);
            return true;
        }
        // Chunked-terrain LOD selection curve (per-chunk camera tiers; the caster matches the drawn
        // LOD by construction, so these trade triangles for pop-in distance only — no rebuild).
        if (setting == "lod.chunkDist0")  { render::g_chunkLodDist0 = std::max(1.0f, value); return true; }
        if (setting == "lod.chunkFactor") { render::g_chunkLodDistFactor = std::max(1.01f, value); return true; }
        // Regular-mesh LOD selection (distance/radius tier boundaries; the dev "LOD" tab's sliders)
        // + the master enable and the per-mesh force, so a LOD capture needs no GUI.
        if (setting == "lod.bound0")  { render::g_lodBound0 = std::max(0.5f, value); return true; }
        if (setting == "lod.bound1")  { render::g_lodBound1 = std::max(0.5f, value); return true; }
        if (setting == "lod.bound2")  { render::g_lodBound2 = std::max(0.5f, value); return true; }
        if (setting == "lod.fadeBand") { render::g_lodFadeBand = std::clamp(value, 0.0f, 0.35f); return true; }
        if (setting == "lod.enabled") { render::g_lodEnabled = value != 0.0f; return true; }
        if (setting == "lod.forced")  { render::g_forcedLod = std::clamp((int)value, -1, 3); return true; }
        // LOD selection debug view (dev "LOD" tab). 0 off, 1 tier colours, 2 apparent-triangle-size
        // colours. Exposed here so a debug capture is a command line, not a click path.
        if (setting == "lod.debug")         { render::g_lodDebugMode = static_cast<render::LodDebugMode>(std::clamp((int)value, 0, 2)); return true; }
        if (setting == "lod.debugBoxes")    { render::g_lodDebugBoxes = value != 0.0f; return true; }
        if (setting == "lod.debugLabels")   { render::g_lodDebugLabels = value != 0.0f; return true; }
        if (setting == "lod.debugCriteria") { render::g_lodDebugCriteria = value != 0.0f; return true; }
        if (setting == "lod.debugRegular")  { render::g_lodDebugRegularMeshes = value != 0.0f; return true; }
        if (setting == "lod.debugRange")    { render::g_lodDebugRange = std::max(10.0f, value); return true; }
        if (setting == "lod.debugMaxBoxes") { render::g_lodDebugMaxBoxes = std::max(0, (int)value); return true; }
        // 0 = whole level, 1 = editor selection only. A headless run has no selection, so 1 there
        // reports an empty view on purpose rather than silently meaning 0.
        if (setting == "lod.debugFilter")   { render::g_lodDebugFilter = static_cast<render::LodDebugFilter>(std::clamp((int)value, 0, 1)); return true; }
        // Mirror of the VSM page-stats log toggle, so a headless run can capture the resident/request
        // counts (logs/vsm_pages.log) that the dev-window "VSM" tab shows live.
        if (setting == "vsm.logPageStats")  { vsm::g_logPageStats = value != 0.0f; return true; }
        if (setting == "gtao.strength") { scene.GtaoRef().strength = value; return true; }
        if (setting == "gtao.upsampleTolerance") { scene.GtaoRef().upsampleTolerance = value; return true; }
        // Where surface reflections come from: 0 None, 1 SkyOnly, 2 SSR, 3 RT. The DEFAULT IS RT,
        // and on RT-capable hardware that means the screen-space path never runs -- so without this
        // key there is no headless way to exercise, measure or gate SSR at all. (Found the hard way
        // in P6C step 6: a "HiZ" capture that had quietly been tracing the TLAS.)
        if (setting == "render.reflectionSource")
        {
            const uint32_t r = (uint32_t)std::max(0.0f, value);
            renderSettings.reflectionSource = r < (uint32_t)ReflectionSource::Count
                ? (ReflectionSource)r : ReflectionSource::RT;
            return true;
        }
        // P6C/P13: which screen-space search the reflection ray uses. 0 = log march (fixed growing
        // steps against full depth), 1 = UE SSRT fixed-step Batch4 march against the furthest HZB.
        // The A/B switch for the SSR retrofit, and the only way to compare both inside one binary.
        // Only has an effect when render.reflectionSource is 2 (SSR).
        if (setting == "ssr.temporal")   { renderSettings.ssrTemporal = value != 0.0f; return true; }
        if (setting == "ssr.temporalBlend") { renderSettings.ssrTemporalBlendWeight = value; return true; }
        if (setting == "ssr.temporalClampExpand") { renderSettings.ssrTemporalClampExpand = value; return true; }
        if (setting == "ssr.ueQuality")
        {
            const uint32_t q = static_cast<uint32_t>(std::max(0.0f, value));
            ApplyUeSsrQualityPreset(renderSettings.ssrUe,
                q < static_cast<uint32_t>(UeSsrQualityPreset::Count)
                    ? static_cast<UeSsrQualityPreset>(q) : UeSsrQualityPreset::Epic);
            return true;
        }
        if (setting == "ssr.ueSteps")
        {
            renderSettings.ssrUe.numSteps = static_cast<uint32_t>(std::max(4.0f, value));
            renderSettings.ssrUe.preset = UeSsrQualityPreset::Custom;
            return true;
        }
        if (setting == "ssr.ueRays")
        {
            renderSettings.ssrUe.numRays = static_cast<uint32_t>(std::max(1.0f, value));
            renderSettings.ssrUe.preset = UeSsrQualityPreset::Custom;
            return true;
        }
        if (setting == "ssr.ueGlossy")
        {
            renderSettings.ssrUe.glossyRays = value != 0.0f;
            renderSettings.ssrUe.preset = UeSsrQualityPreset::Custom;
            return true;
        }
        if (setting == "ssr.ueUseSurfaceRoughness")
        {
            renderSettings.ssrUe.useSurfaceRoughness = value != 0.0f;
            return true;
        }
        if (setting == "ssr.ueRoughnessOverride")
        {
            renderSettings.ssrUe.roughnessOverride = value;
            return true;
        }
        if (setting == "ssr.ueStartMip") { renderSettings.ssrUe.startMipLevel = value; return true; }
        // P7 aerial perspective. `atmosphere.enabled` is the gate; the rest are the model's own
        // parameters, so a sweep can find defaults without a rebuild.
        if (setting == "atmosphere.enabled")
        {
            scene.AtmosphereRef().enabled = value != 0.0f;
            return true;
        }
        if (setting == "atmosphere.density") { scene.AtmosphereRef().density = value; return true; }
        if (setting == "atmosphere.debugView")
        {
            g_atmosphereDebugView = static_cast<uint32_t>(std::max(0.0f, value));
            return true;
        }
        if (setting == "atmosphere.heightFalloff")
        {
            scene.AtmosphereRef().heightFalloff = value;
            return true;
        }
        if (setting == "atmosphere.referenceHeight")
        {
            scene.AtmosphereRef().referenceHeight = value;
            return true;
        }
        if (setting == "atmosphere.startDistance")
        {
            scene.AtmosphereRef().startDistance = value;
            return true;
        }
        if (setting == "atmosphere.maxOpacity") { scene.AtmosphereRef().maxOpacity = value; return true; }
        if (setting == "atmosphere.sunScatter")
        {
            scene.AtmosphereRef().sunScatterStrength = value;
            return true;
        }
        if (setting == "atmosphere.sunScatterExp")
        {
            scene.AtmosphereRef().sunScatterExponent = value;
            return true;
        }
        if (setting == "atmosphere.sunScatterStart")
        {
            scene.AtmosphereRef().sunScatterStartDistance = value;
            return true;
        }
        if (setting == "atmosphere.skyBlur") { scene.AtmosphereRef().skyBlur = value; return true; }
        // P8 bloom. `bloom.enabled` is the gate; the rest are the extraction and the pyramid, so
        // a sweep can find defaults without a rebuild.
        if (setting == "bloom.enabled")
        {
            scene.BloomRef().enabled = value != 0.0f;
            return true;
        }
        if (setting == "bloom.intensity") { scene.BloomRef().intensity = value; return true; }
        if (setting == "bloom.threshold") { scene.BloomRef().threshold = value; return true; }
        if (setting == "bloom.softKnee") { scene.BloomRef().softKnee = value; return true; }
        if (setting == "bloom.radius") { scene.BloomRef().radius = value; return true; }
        // P8C: 0 = the pyramid, 1 = FFT convolution.
        if (setting == "bloom.method")
        {
            scene.BloomRef().method = static_cast<uint32_t>(std::max(0.0f, value));
            return true;
        }
        if (setting == "bloom.blades")
        {
            scene.BloomRef().convBlades = static_cast<uint32_t>(std::max(0.0f, value));
            return true;
        }
        // P8C-2: the aperture-kernel keys (bloom.kernelRadius, spokes*, chroma, the anamorphic
        // squeeze, ghostSpacing) are GONE with the mechanism -- deleted, not aliased.
        if (setting == "bloom.convSize") { scene.BloomRef().convSize = value; return true; }
        if (setting == "bloom.convPercent") { scene.BloomRef().convPercent = value; return true; }
        if (setting == "bloom.anamorphicIntensity")
        {
            scene.BloomRef().convAnamorphicIntensity = value;
            return true;
        }
        if (setting == "bloom.anamorphicLength")
        {
            scene.BloomRef().convAnamorphicLength = value;
            return true;
        }
        if (setting == "bloom.anamorphicWidth")
        {
            scene.BloomRef().convAnamorphicWidth = value;
            return true;
        }
        if (setting == "bloom.anamorphicThreshold")
        {
            scene.BloomRef().convAnamorphicThreshold = value;
            return true;
        }
        if (setting == "bloom.anamorphicNarrow")
        {
            scene.BloomRef().convAnamorphicNarrow = value;
            return true;
        }
        if (setting == "bloom.anamorphicChroma")
        {
            scene.BloomRef().convAnamorphicChroma = value;
            return true;
        }
        if (setting == "bloom.ghosts")
        {
            scene.BloomRef().convGhosts = static_cast<uint32_t>(std::max(0.0f, value));
            return true;
        }
        if (setting == "bloom.ghostBokeh") { scene.BloomRef().convGhostBokeh = value; return true; }
        if (setting == "bloom.ghostThreshold") { scene.BloomRef().convGhostThreshold = value; return true; }
        if (setting == "bloom.ghostIntensity")
        {
            scene.BloomRef().convGhostIntensity = value;
            return true;
        }
        if (setting == "bloom.firefly")
        {
            scene.BloomRef().fireflyClamp = value != 0.0f;
            return true;
        }
        if (setting == "atmosphere.backScatter")
        {
            scene.AtmosphereRef().skyBackScatter = value;
            return true;
        }
        if (setting == "ssr.ueTolerance") { renderSettings.ssrUe.slopeCompareToleranceScale = value; return true; }
        if (setting == "ssr.ueConfirmRetries")
        {
            renderSettings.ssrUe.confirmRetries = static_cast<uint32_t>(std::max(0.0f, value));
            return true;
        }
        if (setting == "ssr.ueRefineSteps")
        {
            renderSettings.ssrUe.refineSteps = static_cast<uint32_t>(std::max(0.0f, value));
            return true;
        }
        if (setting == "ssr.technique")
        {
            const uint32_t t = (uint32_t)std::max(0.0f, value);
            renderSettings.ssrTechnique = t < (uint32_t)SsrTechnique::Count
                ? (SsrTechnique)t : SsrTechnique::LogMarch;
            return true;
        }
        // The fullscreen debug blit, so a half-res intermediate can be captured headlessly instead
        // of only judged by eye in the GUI. `debug.tex` = 0 shadow atlas, 1 GTAO raw, 2 denoised,
        // 3 temporal, 4 upsampled, 5 HZB furthest, 6 scene depth, 7 HZB closest;
        // `debug.texMode` turns the blit on.
        if (setting == "debug.texMode")    { renderSettings.debugTexMode = value != 0.0f; return true; }
        if (setting == "debug.tex")        { renderSettings.debugTexTarget = (int)value; return true; }
        if (setting == "debug.texMip")     { renderSettings.debugTexMip = (int)value; return true; }
        if (setting == "light.sunTemperatureK")
        {
            scene.DirectionalLightRef().SetUseSunTemperature(value > 0.0f);
            if (value > 0.0f) { scene.DirectionalLightRef().SetSunTemperatureK(value); }
            return true;
        }
        // P16.12: one scalar drives all three channels, which is what a sweep needs; the level
        // and the inspector carry the full colour. 0 switches the bounce off.
        if (setting == "light.groundAlbedo")
        {
            scene.DirectionalLightRef().SetGroundAlbedo(Math::float3(value, value, value));
            return true;
        }
        if (setting == "light.skyFillIntensity")
        {
            scene.DirectionalLightRef().SetSkyFillIntensity(value);
            return true;
        }
        if (setting == "light.skyIntensity")
        {
            if (Skybox* sky = scene.GetSkybox()) { sky->SetExposure(value); }
            return true;
        }
        // P16.3b: the sky's horizontal illuminance in lux. 0 = un-authored, the legacy behaviour.
        // Shore contact foam, as a CAPTURE switch rather than a level edit: it is the thing that
        // litters a lighting comparison with moving white speckle, and turning it off in the level
        // would mean editing content that was tuned on purpose. 0 = off.
        // The whole ocean surface, as a CAPTURE switch (same rationale as contactFoam below):
        // shadow/terrain artifacts live on dune slopes the water covers, and the user's repro
        // screenshots are taken with the ocean hidden. 0 = hidden.
        if (setting == "ocean.visible") { scene.SetOceanVisible(value != 0.0f); return true; }
        // Sky-reflection horizon pull (OceanSkyReflectDir; 1 = off). Here so the streak A/B is a
        // command line rather than a level edit — same one-binary discipline as the VSM levers.
        if (setting == "ocean.skyHorizonPull")
        {
            if (OceanRenderable* ocean = scene.FindOceanRenderable())
            {
                if (OceanSimulation* sim = ocean->GetSimulation())
                {
                    OceanRenderConfig cfg = sim->GetRenderConfig();
                    cfg.reflectionSkyHorizonPull = Math::Clamp(value, 0.05f, 1.0f);
                    sim->SetRenderConfig(cfg);
                }
            }
            return true;
        }
        if (setting == "ocean.contactFoam")
        {
            if (OceanRenderable* ocean = scene.FindOceanRenderable())
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
        }
        if (setting == "light.skyIlluminanceLux")
        {
            if (Skybox* sky = scene.GetSkybox()) { sky->SetIlluminanceLux(value); }
            return true;
        }
        // P16.2: the sun's illuminance in lux. `light.sunIntensity` is kept as an ALIAS, not a
        // second control -- it is the same number under its pre-P16.2 name, and every measurement
        // recipe written down in the plan spells it that way.
        if (setting == "light.sunIlluminanceLux" || setting == "light.sunIntensity")
        {
            scene.DirectionalLightRef().SetSunIlluminanceLux(value);
            return true;
        }
        // REMOVED (P16.2): `light.legacySplit`, the P4 equivalence probe. It claimed that putting
        // the factor back on the retired whole-scene multiplier reproduces the migrated image
        // exactly, and MEASURED on wind_test it does not -- 27.6 meanabs against a 0.81 noise floor,
        // the frame 27/255 brighter. The knob was not broken by P16.2; it was invalidated by F8 and
        // F9 and nobody re-ran it. `lighting_cs` still ends in `color * exposure + skySpecular`, so
        // that multiplier now scales the SKY IRRADIANCE FILL (which the sun's intensity does not
        // touch) and skips the sky specular (added after it) -- two terms that did not exist when
        // the equivalence was true. A probe asserting an invariant that no longer holds is worse
        // than no probe, so it is gone rather than caveated.
        //
        // The P16.2 migration has its own equivalence check and it needs no code: run the level
        // once as authored and once with `--set=light.sunIlluminanceLux:<the level's own value>`.
        // Identical frames mean the new key and the legacy fold arrive at the same light.
        //
        // With this gone, `DirectionalLight::exposure_` can no longer be set to anything but 1.0,
        // so the trailing multiply in the shaders is provably an identity. Retiring that plumbing
        // is a separate change; it touches the ocean, glass and RT reflection constant buffers.
        if (setting == "exposure.lowPercentile")   { e.lowPercentile = value;  return true; }
        if (setting == "exposure.highPercentile")  { e.highPercentile = value; return true; }
        if (setting == "exposure.compensationEv")  { e.compensationEv = value; return true; }
        // P16.13: manual mode has its own trim; the auto key above no longer reaches it.
        if (setting == "exposure.manualCompensationEv") { e.manualCompensationEv = value; return true; }
        // P16.6: the camera. `exposure.manualEv100` is kept because every measurement recipe in
        // the plan spells it that way -- it back-solves the aperture, so it still means exactly what
        // it always meant.
        if (setting == "exposure.aperture")   { e.apertureFStop = value;   return true; }
        if (setting == "exposure.shutter")    { e.shutterSpeedSec = value; return true; }
        if (setting == "exposure.iso")        { e.isoSensitivity = value;  return true; }
        if (setting == "exposure.manualEv100")
        {
            e.apertureFStop = render::ApertureFromEv100(value, e.shutterSpeedSec, e.isoSensitivity);
            return true;
        }
        if (setting == "exposure.minEv100")        { e.minEv100 = value;       return true; }
        if (setting == "exposure.maxEv100")        { e.maxEv100 = value;       return true; }
        if (setting == "exposure.speedUp")         { e.speedUp = value;        return true; }
        if (setting == "exposure.meterMaskStrength")    { e.meterMaskStrength = value;    return true; }
        if (setting == "exposure.meterMaskInnerRadius") { e.meterMaskInnerRadius = value; return true; }
        if (setting == "exposure.meterMaskOuterRadius") { e.meterMaskOuterRadius = value; return true; }
        if (setting == "exposure.meterMaskSkyBias")     { e.meterMaskSkyBias = value;     return true; }
        if (setting == "exposure.speedDown")       { e.speedDown = value;      return true; }
        if (setting == "exposure.adaptationStartDistance") { e.adaptationStartDistance = value; return true; }
        if (setting == "exposure.blackBucketInfluence")    { e.blackBucketInfluence = value;    return true; }
        if (setting == "render.preExposure") { render::g_preExposureEnabled = value != 0.0f; return true; }
        if (setting == "exposure.enabled")         { e.enabled = value != 0.0f;      return true; }
        if (setting == "exposure.autoExposure")    { e.autoExposure = value != 0.0f; return true; }
        if (setting == "color.toneCurve")
        {
            c.toneCurve = (value != 0.0f) ? render::ToneCurve::AgX : render::ToneCurve::LegacyAces;
            return true;
        }
        if (setting == "color.agxSlope")      { c.agxSlope = value;      return true; }
        if (setting == "color.agxPower")      { c.agxPower = value;      return true; }
        if (setting == "color.agxSaturation") { c.agxSaturation = value; return true; }
        if (setting == "color.gradeSaturation") { c.gradeSaturation = value; return true; }
        if (setting == "color.gradeContrast")   { c.gradeContrast = value;   return true; }
        if (setting == "color.gradeGamma")      { c.gradeGamma = value;      return true; }
        if (setting == "color.gradeGain")       { c.gradeGain = value;       return true; }
        if (setting == "color.gradeOffset")     { c.gradeOffset = value;     return true; }
        // P3B moved onto the camera. The `color.*` spellings are kept as aliases so older notes,
        // scripts and the plan's recorded sweeps keep resolving.
        if (setting == "exposure.localHighlightContrast" || setting == "color.localHighlightContrast")
        { e.localHighlightContrast = value; return true; }
        if (setting == "exposure.localShadowContrast" || setting == "color.localShadowContrast")
        { e.localShadowContrast = value; return true; }
        if (setting == "exposure.localDetailStrength" || setting == "color.localDetailStrength")
        { e.localDetailStrength = value; return true; }
        // Composite: both local contrast scales at once. The two are independent branches of the
        // same function (base above / below middle grey), so sweeping them together is the only way
        // to measure the base EXPANSION the plan's P3B target needs -- one knob alone can move only
        // one end of the histogram, and the reference wants both ends moved in the same shot.
        if (setting == "exposure.localContrast" || setting == "color.localContrast")
        {
            e.localHighlightContrast = value;
            e.localShadowContrast = value;
            return true;
        }
        return false;
    }
}

LRESULT CALLBACK App::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    // When the window is first created the app pointer is not set yet, but that's fine
    if (message == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    if (app && app->systems_) {
        auto& renderer = app->systems_->renderer;
        renderer.HandleImGuiWndProc(hWnd, message, wParam, lParam);

        const bool mouseMessage = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
        const bool keyMessage = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        const bool consumedByImGui =
            (mouseMessage && renderer.ImGuiWantsMouse()) ||
            (keyMessage && renderer.ImGuiWantsKeyboard());
        const bool inputStateRelease =
            message == WM_KEYUP ||
            message == WM_SYSKEYUP ||
            message == WM_LBUTTONUP ||
            message == WM_MBUTTONUP ||
            message == WM_RBUTTONUP;

        // Release messages must always reach InputManager. A modifier or mouse
        // button may have gone down before ImGui acquired capture; swallowing
        // its release would leave the engine-side state stuck indefinitely.
        if (!consumedByImGui || inputStateRelease) {
            app->systems_->input.OnWndProc(hWnd, message, wParam, lParam);
        }
    }

    switch (message) {
    case WM_SIZE:
    {
        UINT width = LOWORD(lParam);
        UINT height = HIWORD(lParam);
        if (app && app->systems_ && app->systems_->renderer.GetDevice() && wParam != SIZE_MINIMIZED) {
            app->systems_->renderer.OnResize(width, height);
        }
        break;
    }
    case WM_ERASEBKGND:
        if (app && app->loadingScreenVisible_)
        {
            app->PaintLoadingScreen(reinterpret_cast<HDC>(wParam));
            return 1;
        }
        break;
    case WM_PAINT:
        if (app && app->loadingScreenVisible_)
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hWnd, &ps);
            app->PaintLoadingScreen(dc);
            EndPaint(hWnd, &ps);
            return 0;
        }
        break;
    case WM_DESTROY:
        if (app)
        {
            app->SetRunnig(false);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

void App::InitWindow(HINSTANCE hInstance, int nCmdShow) {
    assert(systems_);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = App::WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"D3D12WindowClass";

    RegisterClassEx(&wc);

    RECT screenRect;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &screenRect, 0);

    float scale = 1.0f, aspect = 9.0f/16.0f;
    LONG defWidth = (LONG)(2560 * scale);
	LONG defHeight = (LONG)(1440 * scale);

    defWidth = std::min(defWidth, (screenRect.right - screenRect.left) - 8);
    defHeight = LONG(defWidth * aspect);

    RECT rect = { 0, 0, defWidth, defHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    int windowWidth = rect.right - rect.left;
    int windowHeight = rect.bottom - rect.top;
    int posX = (screenRect.right - windowWidth) / 2;
    int posY = (screenRect.bottom - windowHeight) / 2;

    hWnd_ = CreateWindow(
        wc.lpszClassName,
        L"D3D12 Multi-Mesh Renderer",
        WS_OVERLAPPEDWINDOW,
        posX, posY,
        windowWidth,
        windowHeight,
        nullptr, nullptr,
        hInstance, this
    );
    SetWindowLongPtr(hWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    LoadLoadingScreen();
    loadingScreenVisible_ = true;
    ShowWindow(hWnd_, nCmdShow);
    UpdateWindow(hWnd_);

    auto& renderer = systems_->renderer;
    auto& input = systems_->input;
    renderer.InitD3D12(hWnd_, defWidth, defHeight);
    input.Initialize(hWnd_);
#if PROF_GPU_ENABLED
    Profiler::Get().InitGpu(renderer.GetDevice(), renderer.GetCommandQueue());
#endif
}

void App::InitScene()
{
    assert(systems_);

    auto& renderer = systems_->renderer;
    auto& scene = systems_->scene;
    auto& input = systems_->input;
    auto& levelManager = systems_->levelManager;

    if (!input.LoadActions(L"input/bindings.json"))
    {
        assert(false && "No bindings.json found!");
    }

    // I0: materials are per-file assets in data/materials/ (one json per material, name = stem).
    // The legacy monolith loads first so per-file materials win on a name clash during migration.
    MaterialDataManager* materials = renderer.GetMaterialDataManager();
    (void)materials->LoadPresetsFromJsonFile(L"data/materials.json"); // legacy, absent post-migration
    (void)materials->LoadPresetsFromDirectory(L"data/materials");
    assert(materials->PresetCount() > 0 && "No material presets found (data/materials/)!");

    UploadBatch uploadBatch;
    const bool batchBegun = uploadBatch.Begin(&renderer);
    assert(batchBegun && "Failed to begin upload batch");
    (void)batchBegun;

    renderer.InitTextSystem(uploadBatch.CommandList(), uploadBatch.KeepAlive(), L"fonts");
    if (auto* debugDraw = renderer.GetDebugDrawSystem())
    {
        debugDraw->Initialize(&renderer, uploadBatch.CommandList(), uploadBatch.KeepAlive());
    }

    LevelLoadContext loadCtx{ uploadBatch, renderer, scene };

    if (!levelManager.HasLevel(JsonLevel::kName))
    {
        std::string bootLevel = g_bootLevelPath;
#if WITH_EDITOR
        if (bootLevel.empty())
        {
            bootLevel = EditorController::LoadLastOpenedLevelPath();
        }
#endif
        if (bootLevel.empty())
        {
            bootLevel = "data/levels/demo.json";
        }
        levelManager.RegisterLevel<JsonLevel>(bootLevel);
    }

    const bool levelLoaded = levelManager.LoadLevel(JsonLevel::kName, loadCtx);
    assert(levelLoaded && "Failed to load initial level");

    // Applied after the level so it beats the level's own freeCameraStart.
    if (g_camOverride)
    {
        Camera& cam = scene.CameraRef();
        cam.SetPosition(float3(g_camPos[0], g_camPos[1], g_camPos[2]));

        // Quaternion -> the camera's yaw/pitch/roll. Recovered GEOMETRICALLY rather than by an
        // Euler-extraction formula: yaw/pitch come from where the quaternion points the forward
        // axis, then roll is the leftover twist about that axis, measured as the signed angle
        // between the un-rolled up vector and the real one. This is independent of the rotation
        // ORDER the camera happens to use, so it cannot silently break if that order is ever
        // changed - and it degrades sanely at the poles instead of producing NaNs.
        const DirectX::XMVECTOR q = DirectX::XMQuaternionNormalize(
            DirectX::XMVectorSet(g_camRot[0], g_camRot[1], g_camRot[2], g_camRot[3]));
        const DirectX::XMVECTOR fwd = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 0, 1, 0), q);
        const DirectX::XMVECTOR upQ = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 1, 0, 0), q);
        DirectX::XMFLOAT3 f{};
        DirectX::XMStoreFloat3(&f, fwd);
        const float yaw = std::atan2(f.x, f.z);
        const float pitch = std::atan2(-f.y, std::sqrt(f.x * f.x + f.z * f.z));

        // The up vector this yaw/pitch alone would produce; the angle from it to the real up,
        // measured about forward, IS the roll.
        const mat4 noRoll = mat4::RotationRollPitchYaw(pitch, yaw, 0.0f);
        const float3 upNr3 = noRoll.TransformPoint(float3(0.0f, 1.0f, 0.0f));
        const DirectX::XMVECTOR upNr = DirectX::XMVectorSet(upNr3.x, upNr3.y, upNr3.z, 0.0f);
        const DirectX::XMVECTOR cross = DirectX::XMVector3Cross(upNr, upQ);
        const float sinR = DirectX::XMVectorGetX(DirectX::XMVector3Dot(cross, fwd));
        const float cosR = DirectX::XMVectorGetX(DirectX::XMVector3Dot(upNr, upQ));
        cam.SetYawPitchRoll(yaw, pitch, std::atan2(sinR, cosR));
    }

    uploadBatch.SubmitAndWait(&renderer);
    HideLoadingScreen();
}

void App::LoadLoadingScreen()
{
    ReleaseLoadingScreen();

    loadingBitmap_ = LoadBitmapFromWicFile(kLoadingScreenPath, loadingBitmapInfo_);
}

void App::ReleaseLoadingScreen()
{
    if (loadingBitmap_)
    {
        DeleteObject(loadingBitmap_);
        loadingBitmap_ = nullptr;
    }
    loadingBitmapInfo_ = {};
}

void App::HideLoadingScreen()
{
    loadingScreenVisible_ = false;
    ReleaseLoadingScreen();
}

void App::PaintLoadingScreen(HDC dc) const
{
    if (!dc || !hWnd_)
    {
        return;
    }

    RECT client{};
    GetClientRect(hWnd_, &client);
    const int clientW = std::max<LONG>(client.right - client.left, 1);
    const int clientH = std::max<LONG>(client.bottom - client.top, 1);

    HBRUSH blackBrush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(dc, &client, blackBrush);

    if (loadingBitmap_ && loadingBitmapInfo_.bmWidth > 0 && loadingBitmapInfo_.bmHeight > 0)
    {
        HDC imageDc = CreateCompatibleDC(dc);
        HGDIOBJ oldBitmap = SelectObject(imageDc, loadingBitmap_);

        const double scaleX = static_cast<double>(clientW) / static_cast<double>(loadingBitmapInfo_.bmWidth);
        const double scaleY = static_cast<double>(clientH) / static_cast<double>(loadingBitmapInfo_.bmHeight);
        const double scale = std::max(scaleX, scaleY);
        const int drawW = std::max(1, static_cast<int>(static_cast<double>(loadingBitmapInfo_.bmWidth) * scale + 0.5));
        const int drawH = std::max(1, static_cast<int>(static_cast<double>(loadingBitmapInfo_.bmHeight) * scale + 0.5));
        const int drawX = (clientW - drawW) / 2;
        const int drawY = (clientH - drawH) / 2;

        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, drawX, drawY, drawW, drawH,
            imageDc, 0, 0, loadingBitmapInfo_.bmWidth, loadingBitmapInfo_.bmHeight, SRCCOPY);

        SelectObject(imageDc, oldBitmap);
        DeleteDC(imageDc);
    }

    const int minDim = std::max(1, std::min(clientW, clientH));
    const int margin = std::max(24, minDim / 24);
    const int fontHeight = std::max(32, minDim / 18);
    HFONT font = CreateFontW(
        -fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Consolas");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    const int oldBkMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, RGB(0, 0, 0));

    RECT textRect{
        client.left + margin,
        client.top + margin,
        client.right - margin,
        client.bottom - margin
    };
    RECT shadowRect = textRect;
    OffsetRect(&shadowRect, 3, 3);
    DrawTextW(dc, L"Loading...", -1, &shadowRect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, RGB(245, 248, 252));
    DrawTextW(dc, L"Loading...", -1, &textRect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldBkMode);
    if (oldFont)
    {
        SelectObject(dc, oldFont);
    }
    if (font)
    {
        DeleteObject(font);
    }
}

void App::Run(HINSTANCE hInstance, int nCmdShow) {
    systems_ = std::make_unique<Systems::AppSystems>();
    Systems::Init(systems_.get());

    {
        auto& renderer = systems_->renderer;
        auto& scene = systems_->scene;
        auto& input = systems_->input;
        auto& levelManager = systems_->levelManager;

        InitWindow(hInstance, nCmdShow);
        TaskSystem::Get().Start(static_cast<unsigned int>(std::thread::hardware_concurrency() * 0.75f));
        //TaskSystem::Get().Start(8);
        Profiler::Get().SetThreadName("MainThread");

        InitScene();

        // "--dlss=<mode>": apply the boot override once the device, the DLSS handler and the
        // deferred targets exist, so this takes exactly the same path as the dev-window combo
        // (resolution update + target recreation) rather than a second, untested init order.
        if (g_bootDlssMode >= 0)
        {
            renderer.SetDlssMode(static_cast<sl::DLSSMode>(g_bootDlssMode));
        }

        MSG msg = {};
        double lastTime = GetTimeSeconds();
        while (isRunning_) {
            Profiler::Get().BeginFrame(renderer.GetTotalFrameNumber());
            render::g_renderStats.NextFrame(); // snapshot last frame's draw/primitive counts
            TaskSystem::Get().WaitForTrackedAsyncTasks();
            renderer.BeginFrame();
#if PROF_GPU_ENABLED
            Profiler::Get().CollectGpuResults();
#endif

            {
                CPU_SCOPE(ProfilerScopes::kWholeCycle);
                input.NewFrame();

                {
                    CPU_SCOPE(ProfilerScopes::kWinMessages);
                    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg);
                        DispatchMessage(&msg);
                        if (msg.message == WM_QUIT) {
                            break; // Break out of the loop and stop rendering
                        }
                    }
                    if (msg.message == WM_QUIT) {
                        break;
                    }
                }

                renderer.BeginImGuiFrame();

                Profiler::Get().Tick();

                double now = GetTimeSeconds();
                float deltaTime = static_cast<float>(now - lastTime);
                lastTime = now;

                deltaTime = Math::Clamp(deltaTime, 1e-6f, 0.1f);

                // "--cam-fly=x,z": drift the camera at a constant world velocity (m/s). The ONLY
                // headless way to exercise motion-dependent paths — window relocations (wetness,
                // surf sim), clipmap snapping, DLSS history — a fixed --cam-pos never triggers them.
                if (g_camFly[0] != 0.0f || g_camFly[1] != 0.0f)
                {
                    static const double flyStart = GetTimeSeconds();
                    if (now - flyStart >= (double)g_camFlyDelay)
                    {
                        Camera& cam = scene.CameraRef();
                        const float3 p = cam.GetPosition();
                        cam.SetPosition(float3(p.x + g_camFly[0] * deltaTime, p.y,
                                               p.z + g_camFly[1] * deltaTime));
                    }
                }
                renderer.Tick(deltaTime);
                appController_.Tick(input, renderer, scene, levelManager, deltaTime);
                scene.Tick(deltaTime);

                levelManager.Tick(deltaTime);

                if (auto pendingLevel = levelManager.ConsumePendingLevelRequest())
                {
                    renderer.WaitForPreviousFrame();

                    bool levelLoaded = false;
                    UploadBatch levelBatch;
                    if (levelBatch.Begin(&renderer))
                    {
                        LevelLoadContext levelCtx{ levelBatch, renderer, scene
#if WITH_EDITOR
                            , pendingLevel->options.editorDocument
#endif
                        };

                        levelLoaded = pendingLevel->loadFromPath
                            ? levelManager.LoadLevelFromPath(pendingLevel->sourcePath, levelCtx, pendingLevel->options)
                            : levelManager.LoadLevel(pendingLevel->levelName, levelCtx, pendingLevel->options);
                        if (levelLoaded)
                        {
                            levelBatch.SubmitAndWait(&renderer);
                        }
                    }
#if WITH_EDITOR
                    appController_.OnLevelChangeRequestCompleted(*pendingLevel, levelLoaded, renderer, scene, levelManager);
#endif
                }

                appController_.WaitForHudBuild();
                scene.Render(&renderer);
            }

            // "--set=<name>:<value>;...": pin settings for the run. Applied once, here, because the
            // scene has to exist first — several names in the table reach through it. Same
            // dispatcher as --sweep, so a name works in both or in neither.
            if (!g_fixedSettings.empty())
            {
                static bool fixedApplied = false;
                if (!fixedApplied)
                {
                    fixedApplied = true;
                    for (const auto& kv : g_fixedSettings)
                    {
                        const bool known = ApplySweepValue(scene, appController_.SettingsRef(),
                                                           kv.first, kv.second);
                        char msg[192];
                        std::snprintf(msg, sizeof(msg), "[set] %s = %g%s\n", kv.first.c_str(),
                                      kv.second, known ? "" : "  (UNKNOWN SETTING)");
                        OutputDebugStringA(msg);
                    }
                }
            }

            // "--shot=<path>": after the warmup delay (ocean/particle sim settling), grab the
            // just-presented backbuffer to a PNG and quit. Reliable on the flip-model swapchain.
            //
            // "--shot-count=<n> --shot-step=<sec>" turns it into a PHASE SERIES from ONE process:
            // requires --wind-freeze, whose frozen clock this advances by exactly --shot-step via
            // vfx::g_windStep between frames (path gets an _NN suffix). One boot instead of N —
            // the previous way to film the shore breathing was N full relaunches. The inter-shot
            // pause (--shot-interval) exists for the temporal stack: DLSS needs a few frames after
            // each time jump or every frame in the series carries the previous phase's ghost.
            //
            // "--sweep=<setting>:<v0>,..." reuses the same series machinery for SETTINGS instead of
            // time: value[i] is applied before shot i and the exposure adaptation is reset so each
            // shot settles on its own value. One boot for a whole slider sweep.
            if (!g_shotPath.empty())
            {
                static double shotStart = GetTimeSeconds();
                static int shotIndex = 0;
                static int appliedSweepIndex = -1;
                static bool sweepNameReported = false;

                // Apply before the settle delay is judged, so the value is in place for the whole
                // interval rather than only for the frame the shot is taken on.
                if (!g_sweepSetting.empty() && shotIndex < static_cast<int>(g_sweepValues.size()) &&
                    appliedSweepIndex != shotIndex)
                {
                    const float value = g_sweepValues[static_cast<size_t>(shotIndex)];
                    const bool known = ApplySweepValue(scene, appController_.SettingsRef(),
                                                      g_sweepSetting, value);
                    if (!known && !sweepNameReported)
                    {
                        OutputDebugStringA(("[sweep] unknown setting: " + g_sweepSetting + "\n").c_str());
                        sweepNameReported = true;
                    }
                    // The camera must not carry the previous value's adaptation into this shot.
                    renderer.Exposure().RequestReset();
                    appliedSweepIndex = shotIndex;
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "[sweep] shot %d: %s = %g\n",
                        shotIndex, g_sweepSetting.c_str(), value);
                    OutputDebugStringA(msg);
                }

                const double delay = shotIndex == 0 ? g_shotDelaySec : g_shotIntervalSec;
                if (GetTimeSeconds() - shotStart >= delay)
                {
                    std::string path = g_shotPath;
                    if (g_shotCount > 1)
                    {
                        char suffix[8];
                        std::snprintf(suffix, sizeof(suffix), "_%02d", shotIndex);
                        const size_t dot = path.rfind('.');
                        if (dot != std::string::npos) { path.insert(dot, suffix); }
                        else { path += suffix; }
                    }
                    const bool ok = Screenshot::SaveBackbufferPng(renderer, path);
                    OutputDebugStringA(ok ? "[shot] saved\n" : "[shot] FAILED\n");
                    ++shotIndex;
                    if (shotIndex >= g_shotCount)
                    {
                        g_shotPath.clear();
                        isRunning_ = false;
                    }
                    else
                    {
                        vfx::g_windStep = static_cast<float>(g_shotStepSec);
                        shotStart = GetTimeSeconds();
                    }
                }
            }

            // "--profdump=<path>": temporary VSM perf harness — after the same warmup delay, dump the
            // profiler overlay to a file and quit. Independent of --shot so timings can be swept headlessly.
            if (!g_profDumpPath.empty())
            {
                static double profStart = GetTimeSeconds();
                if (GetTimeSeconds() - profStart >= g_shotDelaySec)
                {
                    const bool ok = Profiler::Get().DumpOverlay(g_profDumpPath);
                    OutputDebugStringA(ok ? "[profdump] saved\n" : "[profdump] FAILED\n");
                    g_profDumpPath.clear();
                    isRunning_ = false;
                }
            }

            // "--trace=<frames>": the same capture the CaptureTrace key requests, but headless.
            // Without it a profiler change cannot be verified without driving the GUI — which is
            // how an 82 us hole in the GPU timeline went unexplained for so long. Requests after
            // the warmup delay, then keeps running until the capture has written itself out.
            if (g_traceFrames != 0)
            {
                static double traceStart = GetTimeSeconds();
                static uint32_t framesLeft = 0;
                static bool requested = false;
                if (!requested && GetTimeSeconds() - traceStart >= g_shotDelaySec)
                {
                    Profiler::Get().RequestTraceCapture(g_traceFrames);
                    framesLeft = g_traceFrames + 30; // margin: the write happens on the last frame
                    requested = true;
                }
                else if (requested && --framesLeft == 0)
                {
                    g_traceFrames = 0;
                    isRunning_ = false;
                }
            }


            Profiler::Get().EndFrame();
        }

        TaskSystem::Get().Stop();

        renderer.WaitForPreviousFrame();
        scene.Clear();
        Systems::DestroyOceanSimulation();
        renderer.Shutdown();
    }

    Systems::Shutdown();
    systems_.reset();

    mi_register_output(MiOut, nullptr);
    mi_collect(true);
    mi_option_set(mi_option_show_stats, 1);
    //mi_stats_print(nullptr);
}
