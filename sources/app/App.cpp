#include "app/FrameRequests.h" // what a --set asks of the frame (R2.2)
#include "app/App.h"
#include "app/AppResources.h"
#include "app/BuildVersion.h"
#include "rhi/MemoryReport.h"    // the memory line in the session log
#include "core/math/Math.h"
#include "core/vfs/Vfs.h"
#include "materials/Material.h"
#include "core/diagnostics/BootProfile.h"
#include "core/diagnostics/CrashHandler.h"   // --crash-selftest=terminate-frame (crash::SelfTestFrame)
#include "rendering/diagnostics/GraphSelfTest.h" // --crash-selftest=pass-throw (render::PassThrowSelfTestFrame)
#include "rendering/diagnostics/RetireSelfTest.h" // --retire-selftest (render::RetireSelfTestFrame)
#include "core/diagnostics/ThrowSelfTest.h"  // --throw-selftest, --crash-selftest=view-throw (diag::ThrowSelfTestFrame)
#include "materials/TextureDecodeCache.h"    // --throw-selftest=texdecode (texdecode::ThrowSelfTestFrame)
#include "core/memory/MemScope.h"           // memory profiling M3: MEM_SCOPE on the frame's owners
#include "core/memory/MemTrace.h"           // memory profiling M4: frame markers, the trace's stop
#include "rhi/MemGates.h"        // memory profiling M6: --mem-selftest, --mem-readout
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/logging/Log.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <intrin.h> // __cpuid: the CPU's brand string for the auto-detect's system identity
#include <filesystem>
#include <optional> // the main thread's "Whole Cycle" held across the loop's turn (render thread, plan G8)
#include <thread>
#include <mimalloc.h>
#include <vector>
#include <wincodec.h>
#include <timeapi.h> // timeBeginPeriod: the frame cap's sleep granularity (see App::Run)
#include <wrl/client.h>
#include <dxgi1_4.h> // the auto-detect's GPU identity (EnumAdapterByLuid)
#include "rendering/wind/WindState.h" // --shot-count phase series steps the frozen wind clock (g_windStep)

std::atomic<bool> g_modelBusy{ false };
std::atomic<bool> g_modelBusyBackground{ false };

BootOptions& Boot()
{
    static BootOptions options;
    return options;
}

#include "app/levels/JsonLevel.h"
#include "ocean/OceanBuoyancy.h"    // get_readouts "floaters"
#include "ocean/OceanReadback.h"
#include "ocean/OceanRenderable.h"  // get_readouts: the wake's fronts
#include "rendering/shadows/TessySettings.h" // get_readouts: the Tessy state
#include "rendering/core/Screenshot.h"
#include "app/harness/CaptureHarnesses.h" // R3.3: --shot, --sweep, --profdump, --trace
#include "app/harness/GateHarnesses.h" // R3.3: --mem-readout, --scalability-check, --rt-audit
#include "app/harness/IntentHarness.h" // R3.3: --intent's end
#include "app/harness/SettingsDumpHarness.h" // R2.0: --settings-dump
#include "app/diagnostics/SceneStress.h"      // R3.3e: --scene-stress, a harness of this loop
#include "rendering/core/UploadBatch.h"
#include "rendering/core/RenderStats.h"
#include "core/threading/ThreadRole.h" // game thread plan: the frame's phases and their roles
#include "core/threading/Knob.h"      // G5: the knobs' exchange at boot
#include "core/threading/RenderThread.h" // G8: Scene::Render on a thread of its own (frame.renderThread)
#include "app/diagnostics/RtAudit.h"     // G9: --rt-audit
#include "rendering/core/VisibilityStats.h" // occlusion plan S0
#include "app/scene/SceneObjectFactory.h"   // occlusion plan S0: scene.replicate spawns through the factory
#include "app/settings/Scalability.h"       // sg.* (docs/scalability_plan.md)
#include "app/settings/SettingContext.h"    // R2.1: what a settings registry row reaches
#include "core/settings/SettingsRegistry.h" // R2.1: the names --set takes, one row each
#include "core/settings/SettingsSeries.h" // MCP set_settings / reset_settings (R2.n)
#include "app/ui/GameSettingsMenu.h"        // the player's saved display settings at boot
#include <fstream>
#include "rendering/debug/DebugDraw.h"
#include "materials/MaterialDataManager.h"
#include "ui/ImGuiRenderer.h" // R4.3: renderer.ImGui()
#if WITH_EDITOR
#include "editor/EditorController.h"
#include "editor/intent/EditorMcpServer.h" // Claude Code's screenshots, taken at the --shot point
#endif

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winmm.lib") // timeBeginPeriod

// t.MaxFPS's wait, transcribed from UEngine::UpdateTimeAndHandleMaxTickRate (UnrealEngine.cpp:
// 3060-3125): WaitTime = budget - time already spent; above 5 ms, Sleep(WaitTime - 2 ms), then
// give the timeslice away (UE's SleepNoStats(0) is SwitchToThread) until the deadline. The 2 ms
// of slack is what lands the cap on its number: Sleep only ever overshoots, by up to a timer
// period, and that period is 1 ms only because App::Run asks for it the way UE's
// FWindowsPlatformMisc::PlatformInit does. `frameStart` is the end of the previous wait -- UE's
// LastRealTime -- so the budget covers the whole frame, not just the part after Present.
static void WaitOutFrameBudget(double frameStart, double frameSeconds)
{
    const double waitEnd = frameStart + frameSeconds;
    const double waitTime = waitEnd - GetTimeSeconds();
    if (waitTime <= 0.0) { return; }
    if (waitTime > 0.005)
    {
        Sleep(static_cast<DWORD>((waitTime - 0.002) * 1000.0));
    }
    while (GetTimeSeconds() < waitEnd)
    {
        SwitchToThread();
    }
}

// mimalloc's output callback (its exit statistics, plus any "mimalloc: warning/error:" it raises
// from ANY thread). Allocator callback, so the raw no-allocation frontend only. The statistics
// printer emits one table row as several fragments through separate calls, so fragments are
// accumulated per thread until a newline and then written as one record; a stray fragment
// longer than the buffer is written as-is rather than lost.
static void MiOut(const char* msg, void* /*arg*/)
{
    if (msg == nullptr) { return; }
    static thread_local char line[1024];
    static thread_local size_t length = 0;

    const auto emit = [](const char* text, size_t count)
    {
        while (count != 0 && (text[count - 1] == '\n' || text[count - 1] == '\r')) { --count; }
        if (count == 0) { return; }
        const std::string_view view(text, count);
        const logging::LogLevel level =
            view.starts_with("mimalloc: error")   ? logging::LogLevel::Error
          : view.starts_with("mimalloc: warning") ? logging::LogLevel::Warning
                                                   : logging::LogLevel::Info;
        logging::WriteRaw(level, logging::LogCategory::Core, view);
    };

    for (const char* cursor = msg; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '\n')
        {
            emit(line, length);
            length = 0;
            continue;
        }
        if (length + 1 >= sizeof(line))
        {
            emit(line, length);
            length = 0;
        }
        line[length++] = *cursor;
    }
}

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

        // Through the VFS, decoded from memory; `bytes` outlives every WIC object below.
        std::vector<std::uint8_t> bytes;
        if (!vfs::ReadAll(path, bytes) || bytes.empty())
        {
            return nullptr;
        }
        Microsoft::WRL::ComPtr<IWICStream> stream;
        if (FAILED(factory->CreateStream(stream.GetAddressOf())) ||
            FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))))
        {
            return nullptr;
        }
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
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
    // Occlusion plan S0: `--set=scene.replicate:K` -- clone every enabled staticMesh of the loaded
    // level K*K-1 more times on a grid whose step is the level's static extent, so a scene where
    // the camera geometry dominates the frame exists without authoring one (wind_test x16 ~ 10k
    // objects). Recorded by the --set row (g_sceneReplicateRequest, app/FrameRequests.h), executed once the level is in (below),
    // through the same factory + UploadBatch + AddInitializedObject path the editor's spawn uses.
    // Nothing is written to disk.

    void ReplicateLevelObjects(Scene& scene, Renderer& renderer, const std::string& levelPath, std::uint32_t k)
    {
        if (k < 2u || levelPath.empty()) { return; }
        nlohmann::json level;
        {
            vfs::InStream in(levelPath, /*binary*/ false);
            if (!in)
            {
                LOG_WARNING(logging::LogCategory::App, "scene.replicate: cannot open level {}", levelPath);
                return;
            }
            try { in >> level; }
            catch (...)
            {
                LOG_WARNING(logging::LogCategory::App, "scene.replicate: level {} is not valid JSON", levelPath);
                return;
            }
        }
        const AABB bounds = scene.ComputeStaticBounds();
        const float3 ext = bounds.IsValid() ? (bounds.GetMax() - bounds.GetMin()) : float3(400.0f, 0.0f, 400.0f);
        // 10 % of air between copies; a level with no static extent still gets a sane grid.
        const float stepX = std::max(ext.x, 50.0f) * 1.1f;
        const float stepZ = std::max(ext.z, 50.0f) * 1.1f;

        // The render half parks first (game thread plan section 3.7): the copies join the scene it walks.
        renderer.WaitForPreviousFrame();
        UploadBatch uploads;
        if (!uploads.Begin(&renderer))
        {
            LOG_WARNING(logging::LogCategory::App, "scene.replicate: UploadBatch::Begin failed");
            return;
        }
        std::uint32_t added = 0, skipped = 0;
        const nlohmann::json objects = level.value("objects", nlohmann::json::array());
        for (const nlohmann::json& o : objects)
        {
            if (!o.is_object() || o.value("type", std::string{}) != "staticMesh" || !o.value("enabled", true)) { continue; }
            const nlohmann::json* pos = o.contains("position") ? &o["position"] : nullptr;
            if (!pos || !pos->is_array() || pos->size() < 3) { ++skipped; continue; }
            const std::string baseName = o.value("name", std::string("staticMesh"));
            // Centred on the original (K=4: offsets -1..2), so a camera standing in the level sees
            // copies in every direction instead of a one-sided quadrant at +X/+Z.
            const int lo = -static_cast<int>((k - 1u) / 2u);
            for (int ix = lo; ix < lo + static_cast<int>(k); ++ix)
            {
                for (int iz = lo; iz < lo + static_cast<int>(k); ++iz)
                {
                    if (ix == 0 && iz == 0) { continue; } // the original is already in the scene
                    nlohmann::json c = o;
                    c.erase("id"); // a runtime object has no editor identity (AddInitializedObject assigns none)
                    c["name"] = baseName + " rep(" + std::to_string(ix) + "," + std::to_string(iz) + ")";
                    c["position"][0] = (*pos)[0].get<float>() + static_cast<float>(ix) * stepX;
                    c["position"][2] = (*pos)[2].get<float>() + static_cast<float>(iz) * stepZ;
                    std::unique_ptr<RenderableObjectBase> obj =
                        SceneObjectFactory::CreateStaticMeshFromJson(SceneObjectFactory::ResolveMeshAsset(c));
                    if (obj && scene.AddInitializedObject(renderer, uploads, std::move(obj))) { ++added; }
                    else { ++skipped; }
                }
            }
        }
        uploads.SubmitAndWait(&renderer);
        LOG_INFO(logging::LogCategory::App, "scene.replicate {}: +{} static meshes ({} skipped), grid step {:.1f} x {:.1f} m",
                 k, added, skipped, stepX, stepZ);
    }

    // Every --set / --sweep / MCP set_settings name lives in the settings registry (architecture plan R2.2):
    // app/settings/*SettingRows.cpp, a row a name, or a family whose keys are an owner's table (race.*, sg.*, ...).
    bool ApplySweepValue(Scene& scene, SceneRenderSettings& renderSettings,
                         const std::string& setting, float value)
    {
        settings::SettingContext ctx{ scene, renderSettings };
        return settings::Registry().Apply(ctx, setting, value);
    }

    // The reading half: what a ceiling starts from (the scalability table), what the gate reads back, what the
    // Developer Controls tab shows, what --settings-dump prints. A knob written at the frame's safe point (dlss.mode,
    // aa.renderScale) reads as asked until then. Its rows only read: the const is the chain's promise, kept by the rows.
    bool ReadSweepValue(Scene& scene, const SceneRenderSettings& rs, const std::string& name, float& out)
    {
        settings::SettingContext ctx{ scene, const_cast<SceneRenderSettings&>(rs) };
        return settings::Registry().Read(ctx, name, out);
    }

    // The adapter as the auto-detect remembers it: its name and the user-mode driver's version (only the name is compared:
    // a new GPU measures again, a driver update keeps the player's levels), and its own memory.
    std::string GpuIdentity(ID3D12Device* device, std::uint64_t& vramBytes)
    {
        DXGI_ADAPTER_DESC1 desc{};
        LARGE_INTEGER umd{};
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (device && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&desc)) &&
            FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        {
            umd.QuadPart = 0;
        }
        vramBytes = static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
        char driver[48];
        std::snprintf(driver, sizeof(driver), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                      HIWORD(umd.LowPart), LOWORD(umd.LowPart));
        return vfs::Narrow(desc.Description) + " / " + driver;
    }

    // The machine beside its GPU (owner, 2026-11-17: "auto-detect forced at the first start of a build on a new system"):
    // the CPU's brand string and the installed memory. A settings folder carried to another computer with the same graphics
    // card measures again there; a driver update still keeps the player's levels.
    std::string SystemIdentity()
    {
        int regs[4]{};
        char brand[49]{};
        __cpuid(regs, static_cast<int>(0x80000000u));
        if (static_cast<unsigned>(regs[0]) >= 0x80000004u)
        {
            for (int i = 0; i < 3; ++i)
            {
                __cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned>(i)));
                std::memcpy(brand + 16 * i, regs, sizeof(regs));
            }
        }
        std::string cpu(brand);
        const std::size_t first = cpu.find_first_not_of(' ');
        cpu = first == std::string::npos ? std::string("unknown CPU") : cpu.substr(first, cpu.find_last_not_of(' ') - first + 1);
        ULONGLONG installedKb = 0;
        const unsigned long long gb = GetPhysicallyInstalledSystemMemory(&installedKb) ? (installedKb + 512ull * 1024ull) / (1024ull * 1024ull) : 0ull;
        return cpu + " / " + std::to_string(gb) + " GB";
    }

    // Auto-detect (docs/scalability_plan.md §5): the real frame, not a synthetic benchmark. High at a Balanced render
    // resolution, 60 frames and 5 s to settle (1 s for the next levels), then the median of the GPU frame's own times over
    // 3 s (the profiler's GPU.Frame, last frame -- not its average); within 0.6 of the budget (60 fps less 30 %: 11.7 ms)
    // Ultra is tried, over it Medium, then Low, then the resolution goes to Performance. Under 6 GB of video memory the
    // textures stay at Medium at most, under 4 GB Low. A frame over 1 s stops it at Medium (UE's TDR guard). The result is
    // applied; saved (with the GPU it was measured on) only for a packaged build's own first start.
    struct ScalabilityAutodetect
    {
        bool running = false;
        int level = 2;          // the level being measured
        int resolution = 1;
        int frame = 0;
        int measurements = 0;   // the first one settles longer: the boot's streaming and the GI's warm-up
        double settleUntil = 0.0;
        double measureUntil = 0.0;
        std::vector<double> samples;
        std::array<double, settings::kScalabilityLevels> measured{ -1.0, -1.0, -1.0, -1.0 };
    };

    void StepScalabilityAutodetect(ScalabilityAutodetect& a, Renderer& renderer, double now)
    {
        settings::Scalability& s = settings::TheScalability();
        constexpr double kBudgetMs = 1000.0 / 60.0 * 0.7;
        const auto measureLevel = [&](int level, int resolution)
        {
            a.level = level;
            a.resolution = resolution;
            s.SetPreset(level);
            s.SetResolutionLevel(resolution);
            s.Apply("auto-detect: measuring " + s.Table().levelNames[static_cast<std::size_t>(level)]);
            a.frame = 0;
            a.settleUntil = 0.0;
            a.samples.clear();
            s.SetAutodetectStatus("measuring " + s.Table().levelNames[static_cast<std::size_t>(level)] + "...");
        };
        const auto finish = [&](int preset, int resolution, const std::string& why)
        {
            s.SetPreset(preset);
            s.SetResolutionLevel(resolution);
            std::uint64_t vram = 0;
            const std::string gpu = GpuIdentity(renderer.GetDevice(), vram);
            const double gb = static_cast<double>(vram) / (1024.0 * 1024.0 * 1024.0);
            const int textures = s.GroupIndex("textures");
            if (textures >= 0 && gb > 0.0 && gb < 6.0)
            {
                s.SetGroupLevel(textures, std::min(s.GroupLevel(textures), gb < 4.0 ? 0 : 1));
            }
            s.Apply("auto-detect: " + why);
            std::string error;
            if (s.AutodetectSaves() && !s.SaveUser(error)) { LOG_WARNING(logging::LogCategory::App, "auto-detect: not saved: {}", error); }
            nlohmann::json record = { { "gpu", gpu }, { "system", SystemIdentity() }, { "vramGB", std::round(gb * 10.0) / 10.0 }, { "preset", preset },
                                      { "resolution", resolution }, { "why", why }, { "gpuTimeScale", s.autodetectGpuScale } };
            nlohmann::json ms = nlohmann::json::object();
            for (int l = 0; l < settings::kScalabilityLevels; ++l)
            {
                if (a.measured[static_cast<std::size_t>(l)] >= 0.0)
                {
                    ms[s.Table().levelNames[static_cast<std::size_t>(l)]] = std::round(a.measured[static_cast<std::size_t>(l)] * 1000.0) / 1000.0;
                }
            }
            record["gpuFrameMs"] = ms;
            if (s.AutodetectSaves() && !settings::Scalability::WriteUserSection("autodetect", record.dump(), error))
            {
                LOG_WARNING(logging::LogCategory::App, "auto-detect: the record was not saved: {}", error);
            }
            const std::string name = s.Table().levelNames[static_cast<std::size_t>(preset)];
            s.SetAutodetectStatus("recommended: " + name + " (" + why + ")");
            LOG_INFO(logging::LogCategory::App, "auto-detect: {} at resolution {} -- {}; {} ({:.1f} GB)", name,
                     s.Table().levelNames[static_cast<std::size_t>(resolution)], why, gpu, gb);
            s.SetAutodetectRunning(false);
            a.running = false;
        };
        if (a.running && s.TakeAutodetectAbort())
        {
            // The menu's Cancel put the levels back itself; nothing more is applied or saved.
            a.running = false;
            s.SetAutodetectRunning(false);
            s.SetAutodetectStatus("auto-detect cancelled");
            LOG_INFO(logging::LogCategory::App, "auto-detect: cancelled");
            return;
        }
        if (!a.running)
        {
            if (!s.TakeAutodetectRequest() || !s.Loaded()) { return; }
            a = ScalabilityAutodetect{};
            a.running = true;
            s.SetAutodetectRunning(true);
            LOG_INFO(logging::LogCategory::App, "auto-detect: measuring the frame (budget {:.1f} ms, time scale x{:.2f})", kBudgetMs,
                     s.autodetectGpuScale);
            measureLevel(2, 1);
            return;
        }
        ++a.frame;
        if (a.frame < 60) { return; }
        // The first measurement after a boot waits for the streaming, the GI's probes and the shader caches to settle; a
        // switch of level costs a few frames.
        if (a.settleUntil == 0.0)
        {
            a.settleUntil = now + (a.measurements == 0 ? 5.0 : 1.0);
            a.measureUntil = a.settleUntil + 3.0;
            ++a.measurements;
            return;
        }
        if (now < a.settleUntil) { return; }
        // The last GPU frame's own time, not the overlay's average (an EMA of 0.997 a frame: ~333 frames to settle, so
        // a level measured right after another would read a blend of the two -- the reviews, 2026-11-14).
        double gpuMs = -1.0;
        for (const Profiler::OverlaySample& r : Profiler::Get().OverlaySnapshot())
        {
            if (r.gpu && r.name == "GPU.Frame") { gpuMs = r.lastMs; break; }
        }
        if (gpuMs > 1000.0)
        {
            finish(1, 1, "a frame over 1 s: Medium");
            return;
        }
        if (gpuMs > 0.0) { a.samples.push_back(gpuMs * static_cast<double>(s.autodetectGpuScale)); }
        if (now < a.measureUntil) { return; }
        if (a.samples.empty())
        {
            finish(2, 1, "no GPU timings: High");
            return;
        }
        std::sort(a.samples.begin(), a.samples.end());
        const double ms = a.samples[a.samples.size() / 2];
        a.measured[static_cast<std::size_t>(a.level)] = ms;
        LOG_INFO(logging::LogCategory::App, "auto-detect: {} -- GPU frame {:.2f} ms (median of {})",
                 s.Table().levelNames[static_cast<std::size_t>(a.level)], ms, a.samples.size());
        char why[96];
        if (a.level == 2)
        {
            if (ms <= 0.6 * kBudgetMs) { measureLevel(3, 1); return; }
            if (ms > kBudgetMs) { measureLevel(1, 1); return; }
            std::snprintf(why, sizeof(why), "High %.1f ms within the budget", ms);
            finish(2, 1, why);
        }
        else if (a.level == 3)
        {
            std::snprintf(why, sizeof(why), "Ultra %.1f ms %s the budget", ms, ms <= kBudgetMs ? "within" : "over");
            finish(ms <= kBudgetMs ? 3 : 2, 1, why);
        }
        else if (a.level == 1)
        {
            if (ms > kBudgetMs) { measureLevel(0, 1); return; }
            std::snprintf(why, sizeof(why), "Medium %.1f ms within the budget", ms);
            finish(1, 1, why);
        }
        else
        {
            std::snprintf(why, sizeof(why), "Low %.1f ms%s", ms, ms > kBudgetMs ? ", the render resolution down" : "");
            finish(0, ms > kBudgetMs ? 0 : 1, why);
        }
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
        renderer.ImGui().HandleWndProc(hWnd, message, wParam, lParam);

        const bool mouseMessage = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
        const bool keyMessage = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        // The function keys are the engine's debug toggles and never type text: they reach InputManager unless a text
        // field is being typed in. Held back by WantCaptureKeyboard -- true whenever an ImGui window has the focus,
        // i.e. nearly always with the editor's panels open -- F9 (the profiler overlay), F3, F5, F6, F10, F11 never
        // arrived there (2026-11-03, owner: "F9 does not work"). AppController gates their actions the same way.
        const bool functionKey = keyMessage && wParam >= VK_F1 && wParam <= VK_F24;
        const bool consumedByImGui =
            (mouseMessage && renderer.ImGui().WantsMouse()) ||
            (keyMessage && (functionKey ? renderer.ImGui().WantsTextInput() : renderer.ImGui().WantsKeyboard()));
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
    case WM_DPICHANGED:
    {
        // Another monitor's DPI (docs/scalability_plan.md §6.3, S6): the UI rescales at its next frame. A windowed window
        // dragged across takes the size Windows suggests for it there; one display::Apply placed is where it was put.
        if (app && app->systems_) { app->systems_->renderer.ImGui().RequestDpi(HIWORD(wParam)); }
        const bool popup = (GetWindowLongPtrW(hWnd, GWL_STYLE) & WS_POPUP) != 0;
        if (!display::Applying() && !popup && lParam)
        {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hWnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_SYSCHAR:
        return 0; // Alt+P is ours (Play): DefWindowProc looks for a menu with that letter, finds none, and beeps. Alt+F4 does not come through here
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
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(TC_IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(TC_IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
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
    DWORD windowStyle = WS_OVERLAPPEDWINDOW;
    // The person's window (user_settings.json "display", docs/scalability_plan.md §6.4: read BEFORE the window is made):
    // windowed or borderless, on its monitor, at its size. Nothing saved = the window as it always started (below).
    const display::DisplaySettings savedDisplay = display::Load();
    display::SetBackgroundFps(savedDisplay.backgroundFps);
    // "--window=<w>x<h>": borderless, client = exactly w x h, at the primary monitor's origin (over
    // the taskbar -- which is the point: a 3840x2160 client on the 3840x2160 panel).
    if (savedDisplay.saved && !(Boot().frame.windowSize[0] > 0 && Boot().frame.windowSize[1] > 0))
    {
        const std::vector<display::MonitorInfo> monitors = display::Monitors();
        const display::Placement placement = display::PlacementFor(savedDisplay, monitors);
        windowStyle = placement.style;
        posX = placement.rect.left;
        posY = placement.rect.top;
        windowWidth = placement.rect.right - placement.rect.left;
        windowHeight = placement.rect.bottom - placement.rect.top;
        defWidth = placement.clientWidth;
        defHeight = placement.clientHeight;
        LOG_INFO(logging::LogCategory::App, "window: {} on {}, client {}x{} (user_settings.json display)",
                 savedDisplay.mode == display::WindowMode::Borderless ? "borderless" : "windowed",
                 display::MonitorLabel(monitors, display::FindMonitor(monitors, savedDisplay.monitor)), defWidth, defHeight);
    }
    if (Boot().frame.windowSize[0] > 0 && Boot().frame.windowSize[1] > 0)
    {
        windowStyle = WS_POPUP;
        windowWidth = Boot().frame.windowSize[0];
        windowHeight = Boot().frame.windowSize[1];
        posX = 0;
        posY = 0;
        // The renderer is initialised with defWidth x defHeight below, and the WM_SIZE of the
        // window's creation arrives before there is a device to resize -- so the swapchain would
        // stay 2560x1440 inside a 3840x2160 window (it did, the first time).
        defWidth = windowWidth;
        defHeight = windowHeight;
        LOG_INFO(logging::LogCategory::App, "window: borderless {}x{} (--window)", windowWidth, windowHeight);
    }

    // A shipped build names itself and its version (docs/distribution_build_plan.md §7); dev builds keep their title.
    const std::wstring windowTitle = build::Packaged()
        ? vfs::Widen(std::string(build::ProductName()) + " " + build::VersionString() +
                     (build::Shipping() ? "" : " [Development]"))
        : std::wstring(L"D3D12 Multi-Mesh Renderer");
    hWnd_ = CreateWindow(
        wc.lpszClassName,
        windowTitle.c_str(),
        windowStyle,
        posX, posY,
        windowWidth,
        windowHeight,
        nullptr, nullptr,
        hInstance, this
    );
    SetWindowLongPtr(hWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    display::SetMainWindow(hWnd_);

    LoadLoadingScreen();
    loadingScreenVisible_ = true;
    ShowWindow(hWnd_, nCmdShow);
    UpdateWindow(hWnd_);

    auto& renderer = systems_->renderer;
    auto& input = systems_->input;
    {
        BOOT_SCOPE("Renderer::InitD3D12");
        MEM_SCOPE(MemTag::Renderer);
        renderer.InitD3D12(hWnd_, defWidth, defHeight);
    }
    input.Initialize(hWnd_);
#if PROF_GPU_ENABLED
    // Async-compute step 3: the profiler gets BOTH queues, so it can calibrate each clock, keep a
    // drain fence per queue, and put compute-recorded scopes on their own trace row. The compute
    // queue may be null (device refused it); every per-queue path in the profiler tolerates that.
    BOOT_SCOPE("Profiler::InitGpu");
    MEM_SCOPE(MemTag::Profiler);
    Profiler::Get().InitGpu(renderer.GetDevice(), renderer.GetCommandQueue(),
                            renderer.GetComputeQueue());
    // Game thread plan G10: the drain fence's signal takes the queues' one road, behind the frame it drains.
    Profiler::Get().SetQueueSignalHook([&renderer](ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
    {
        if (!renderer.Rhi().Threaded())
        {
            return SUCCEEDED(queue->Signal(fence, value));
        }
        Microsoft::WRL::ComPtr<ID3D12Fence> keep(fence);
        renderer.Rhi().Run([queue, keep, value]()
        {
            if (FAILED(queue->Signal(keep.Get(), value)))
            {
                LOG_ERROR(logging::LogCategory::Profiling, "GPU profiler fence signal failed (value {})", value);
            }
        });
        return true;
    });
#endif
}

void App::InitScene()
{
    BOOT_SCOPE("App::InitScene");
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
    {
        BOOT_SCOPE("MaterialPresets");
        MEM_SCOPE(MemTag::Renderer);
        (void)materials->LoadPresetsFromJsonFile(L"data/materials.json"); // legacy, absent post-migration
        (void)materials->LoadPresetsFromDirectory(L"data/materials");
    }
    assert(materials->PresetCount() > 0 && "No material presets found (data/materials/)!");

    UploadBatch uploadBatch;
    const bool batchBegun = uploadBatch.Begin(&renderer);
    assert(batchBegun && "Failed to begin upload batch");
    (void)batchBegun;

    {
        BOOT_SCOPE("InitTextSystem");
        MEM_SCOPE(MemTag::Text);
        renderer.InitTextSystem(uploadBatch.CommandList(), uploadBatch.KeepAlive(), L"fonts");
    }
    if (auto* debugDraw = renderer.GetDebugDrawSystem())
    {
        BOOT_SCOPE("DebugDraw::Initialize");
        MEM_SCOPE(MemTag::Renderer);
        debugDraw->Initialize(&renderer, uploadBatch.CommandList(), uploadBatch.KeepAlive());
    }

    LevelLoadContext loadCtx{ uploadBatch, renderer, scene };

    if (!levelManager.HasLevel(JsonLevel::kName))
    {
        std::string bootLevel = Boot().frame.levelPath;
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
        if (!vfs::Exists(bootLevel))
        {
            // JsonLevel::Load's assert is Debug's: Release went on with no scene, and died in the first target it recreated with the device removed and not a word why.
            LOG_FATAL(logging::LogCategory::Scene, "the boot level '{}' does not exist", bootLevel);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        levelManager.RegisterLevel<JsonLevel>(bootLevel);
    }

    bool levelLoaded = false;
    {
        BOOT_SCOPE("LevelManager::LoadLevel");
        MEM_SCOPE(MemTag::Scene); // the loaders' own scopes (meshes, textures, ocean) narrow it
        levelLoaded = levelManager.LoadLevel(JsonLevel::kName, loadCtx);
    }
    assert(levelLoaded && "Failed to load initial level");

    // Applied after the level so it beats the level's own freeCameraStart.
    if (Boot().frame.camOverride)
    {
        Camera& cam = scene.CameraRef();
        cam.SetPosition(float3(Boot().frame.camPos[0], Boot().frame.camPos[1], Boot().frame.camPos[2]));

        // Quaternion -> the camera's yaw/pitch/roll. Recovered GEOMETRICALLY rather than by an
        // Euler-extraction formula: yaw/pitch come from where the quaternion points the forward
        // axis, then roll is the leftover twist about that axis, measured as the signed angle
        // between the un-rolled up vector and the real one. This is independent of the rotation
        // ORDER the camera happens to use, so it cannot silently break if that order is ever
        // changed - and it degrades sanely at the poles instead of producing NaNs.
        const DirectX::XMVECTOR q = DirectX::XMQuaternionNormalize(
            DirectX::XMVectorSet(Boot().frame.camRot[0], Boot().frame.camRot[1], Boot().frame.camRot[2], Boot().frame.camRot[3]));
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

    {
        BOOT_SCOPE("UploadBatch::SubmitAndWait");
        uploadBatch.SubmitAndWait(&renderer);
    }
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

#if WITH_EDITOR
namespace
{
    // Local-light shadow plan S3, for get_readouts: every local light holding a shadow slot or being
    // faded, as [lightIndex, slot (-1 = none), fade, resolution, page, x, y] (plan S9: its place in the atlas -- a
    // spot's square, a point's 3f x 2f block; page -1 = none) -- what UE's CalculateShadowFadeAlpha and its
    // whole-scene sizing (plan S4: a spot's square, a point's cube face) made of it.
    // Plan S7: + "cache": this frame's {hits, misses, uncached, evicted, entries, pages} of the static-caster cache, and
    // "totals" since the process started (misses by reason, pool flushes, static-set changes, texels filled).
    std::string LocalShadowsJson(const LightManager& lm, const render::LocalShadowCache& cache)
    {
        std::string out = ",\"localShadows\":{\"spots\":[";
        char item[80];
        const char* separator = "";
        for (size_t i = 0; i < lm.SpotLights().size(); ++i)
        {
            const int slot = lm.GetSpotShadowSlot(i);
            const float fade = lm.GetSpotShadowFade(i);
            if (slot < 0 && fade >= 1.0f) { continue; }
            const LightManager::ShadowAtlasPlacement place = lm.GetSpotShadowPlacement(i); // S9
            std::snprintf(item, sizeof(item), "%s[%zu,%d,%.3f,%u,%d,%u,%u]", separator, i, slot, fade,
                          static_cast<unsigned>(lm.GetSpotShadowResolution(i)), place.page, place.x, place.y);
            out += item;
            separator = ",";
        }
        out += "],\"points\":[";
        separator = "";
        for (size_t i = 0; i < lm.PointLights().size(); ++i)
        {
            const int slot = lm.GetPointShadowSlot(i);
            const float fade = lm.GetPointShadowFade(i);
            if (slot < 0 && fade >= 1.0f) { continue; }
            const LightManager::ShadowAtlasPlacement place = lm.GetPointShadowPlacement(i); // S9: the block's corner
            std::snprintf(item, sizeof(item), "%s[%zu,%d,%.3f,%u,%d,%u,%u]", separator, i, slot, fade,
                          static_cast<unsigned>(lm.GetPointShadowResolution(i)), place.page, place.x, place.y);
            out += item;
            separator = ",";
        }
        const render::LocalShadowCache::Stats& cs = cache.FrameStats();
        const render::LocalShadowCache::Totals& ct = cache.RunTotals();
        char cacheItem[896];
        std::snprintf(cacheItem, sizeof(cacheItem),
                      "],\"cache\":{\"hits\":%u,\"misses\":%u,\"uncached\":%u,\"evicted\":%u,\"entries\":%u,\"pages\":%u,"
                      "\"directLights\":%u,\"totals\":{\"frames\":%llu,\"hits\":%llu,\"missNew\":%llu,\"missRes\":%llu,"
                      "\"missMoved\":%llu,\"missStale\":%llu,\"uncached\":%llu,\"uncachedNoRoom\":%llu,\"evicted\":%llu,\"flushes\":%llu,"
                      "\"staticBumps\":%llu,\"fillTexels\":%llu,\"blockNoRoom\":%llu}}}",
                      cs.hits, cs.misses, cs.uncached, cs.evicted, cs.entries, cache.ResidentPages(), cs.direct,
                      static_cast<unsigned long long>(ct.frames), static_cast<unsigned long long>(ct.hits),
                      static_cast<unsigned long long>(ct.missNew), static_cast<unsigned long long>(ct.missRes),
                      static_cast<unsigned long long>(ct.missMoved), static_cast<unsigned long long>(ct.missStale),
                      static_cast<unsigned long long>(ct.uncached), static_cast<unsigned long long>(ct.uncachedNoRoom),
                      static_cast<unsigned long long>(ct.evicted),
                      static_cast<unsigned long long>(ct.flushes), static_cast<unsigned long long>(ct.staticBumps),
                      static_cast<unsigned long long>(ct.fillTexels), static_cast<unsigned long long>(ct.blockNoRoom));
        out += cacheItem;
        return out;
    }

    // Claude Code's runtime door (editor MCP set_settings / get_readouts): the `--set` name table and
    // the renderer's metering, used from the frame thread the MCP tools run on. One editor process
    // then serves any number of A/B variants instead of one boot per picture.
    class McpRuntimeHost final : public editormcp::RuntimeHost
    {
    public:
        McpRuntimeHost(Scene& scene, SceneRenderSettings& settings, Renderer& renderer)
            : scene_(scene), settings_(settings), renderer_(renderer) {}
        bool Apply(const std::string& name, float value) override
        {
            // The value before the first change of a name, for reset_settings (architecture plan R2.n): a project-wide
            // switch the document does not hold no longer stays as last set (core/settings/SettingsSeries).
            const bool applied = series_.Set(SeriesHost(), name, value);
            if (applied) { LOG_INFO(logging::LogCategory::Editor, "MCP set_settings {} = {}", name, value); }
            return applied;
        }
        bool Read(const std::string& name, float& out) override { return ReadSweepValue(scene_, settings_, name, out); }
        std::vector<std::string> Names(const std::string& prefix) override
        {
            std::vector<std::string> out;
            const settings::SettingsRegistry& registry = settings::Registry();
            for (const settings::SettingDesc& d : registry.All())
            {
                if (std::string_view(d.name).substr(0, prefix.size()) == prefix) { out.emplace_back(d.name); }
            }
            for (const settings::SettingFamily& f : registry.Families())
            {
                std::vector<std::string> keys;
                f.keys(keys);
                for (const std::string& k : keys)
                {
                    std::string full = std::string(f.prefix) + k;
                    if (full.compare(0, prefix.size(), prefix) == 0) { out.push_back(std::move(full)); }
                }
            }
            return out;
        }
        std::string Restore() override
        {
            const std::string text = series_.Restore(SeriesHost());
            LOG_INFO(logging::LogCategory::Editor, "MCP reset_settings: {}", text);
            return text;
        }
        void ResetExposure() override { renderer_.Exposure().RequestReset(); }
        std::string Profile(const std::string& filter, bool resetMax) override
        {
            const auto lower = [](std::string s) {
                for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
                return s;
            };
            const std::string want = lower(filter);
            nlohmann::json gpu = nlohmann::json::array(), cpu = nlohmann::json::array();
            for (const Profiler::OverlaySample& r : Profiler::Get().OverlaySnapshot())
            {
                if (!want.empty() && lower(r.name).find(want) == std::string::npos) { continue; }
                (r.gpu ? gpu : cpu).push_back(nlohmann::json::array({ r.name, std::round(r.avgMs * 1000.0) / 1000.0,
                                                                       std::round(r.maxMs * 1000.0) / 1000.0, r.usages }));
            }
            if (resetMax) { Profiler::Get().ResetMaxNow(); }
            const nlohmann::json out = {
                { "frame", renderer_.GetTotalFrameNumber() },
                { "columns", nlohmann::json::array({ "name", "avgMs", "maxMs", "usages" }) },
                { "gpu", std::move(gpu) }, { "cpu", std::move(cpu) },
            };
            return out.dump();
        }
        std::string Memory(const std::string& filter, int top, const std::string& snapshot,
            const std::string& diff, bool report) override
        {
            return render::MemoryJson(renderer_.GetDevice(), filter, top, snapshot, diff, report);
        }
        std::string Readouts(int historyRows) override
        {
            const ExposureMetering::Readback rb = renderer_.Exposure().LatestReadback();
            char text[1024];
            int n = std::snprintf(text, sizeof(text),
                "{\"valid\":%s,\"adaptedEv100\":%.3f,\"targetEv100\":%.3f,\"eyeEv100\":%.3f,"
                "\"compensationEv\":%.3f,\"openSky\":%.3f,\"openNight\":%.3f,\"nightLightEv\":%.2f,\"nightEv100\":%.2f,"
                "\"preExposure\":%.6g,"
                "\"sunProbe\":[%.4g,%.4g,%.4g],\"sunProbeVisibleShare\":%.3f,\"fps\":%.1f,\"frame\":%llu,\"timeScale\":%.2f",
                rb.valid ? "true" : "false", rb.adaptedEv100, rb.targetEv100, rb.eyeEv100,
                rb.compensationEv, rb.openSky, rb.openNight, scene_.GetTimeOfDay().NightLightEv(),
                scene_.GetTimeOfDay().NightEv100(),
                static_cast<double>(render::g_preExposure),
                rb.sunProbe[0], rb.sunProbe[1], rb.sunProbe[2], rb.sunProbeShare,
                renderer_.GetFPS(), static_cast<unsigned long long>(renderer_.GetTotalFrameNumber()), g_timeScale);
            // The pixel probe, while one is asked for (`debug.probeU/probeV/probeRadius`): pre-exposed
            // HDR, a few frames old (the record's read-back ring).
            if (n > 0 && n < static_cast<int>(sizeof(text)) && settings_.debugProbeRadiusPx > 0.0f)
            {
                n += std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n),
                    ",\"pixelProbe\":{\"uv\":[%.4f,%.4f],\"radiusPx\":%.0f,\"max\":[%.4g,%.4g,%.4g],"
                    "\"maxAfterLocalExposure\":%.4g,\"box4x4\":%.4g,\"mean\":[%.4g,%.4g,%.4g]}",
                    settings_.debugProbeU, settings_.debugProbeV, settings_.debugProbeRadiusPx,
                    rb.pixelProbeMax[0], rb.pixelProbeMax[1], rb.pixelProbeMax[2],
                    rb.pixelProbeMaxLocalExposed, rb.pixelProbeBox4,
                    rb.pixelProbeMean[0], rb.pixelProbeMean[1], rb.pixelProbeMean[2]);
            }
            if (n <= 0 || n >= static_cast<int>(sizeof(text)) - 1)
            {
                return "{\"valid\":false}";
            }
            std::string out(text, static_cast<size_t>(n)); // without the closing brace
            out += LocalShadowsJson(scene_.GetLightManager(), scene_.GetLocalShadowCache());
            // The graphics levels (docs/scalability_plan.md §4.5): the groups' levels, the pins, the last apply.
            out += ",\"scalability\":" + settings::TheScalability().StatusJson();
            // The file system the content comes through (docs/distribution_build_plan.md §2): counts since the start.
            {
                const vfs::Counters vc = vfs::GetCounters();
                // Through the JSON library: a package's name or the root may hold a quote, a backslash or anything long
                // (a fixed buffer of printf cut it into invalid JSON).
                const nlohmann::json vfsItem = {
                    { "mounts", vfs::DescribeMounts() }, { "root", vfs::Narrow(vfs::ContentRoot().generic_wstring()) },
                    { "opens", vc.opens }, { "packageOpens", vc.packageOpens }, { "misses", vc.misses }, { "stats", vc.stats },
                    { "lists", vc.lists }, { "reads", vc.reads }, { "bytesRead", vc.bytesRead }, { "blocksDecoded", vc.blocksDecoded },
                    { "damagedBlocks", vc.damagedBlocks } };
                out += ",\"vfs\":" + vfsItem.dump();
                const Material::DistributionShaderStats shaders = Material::GetDistributionShaderStats();
                char shaderItem[128];
                std::snprintf(shaderItem, sizeof(shaderItem), ",\"shaders\":{\"cooked\":%u,\"notCooked\":%u}",
                              shaders.cookedHits, shaders.notCooked);
                out += shaderItem;
            }
            // The physics world (docs/physics_plan.md).
            {
                const physics::Stats ps = scene_.Physics().GetStats();
                char physicsItem[720];
                std::snprintf(physicsItem, sizeof(physicsItem),
                              ",\"physics\":{\"world\":%s,\"simulating\":%s,\"objects\":%u,\"static\":%u,\"kinematic\":%u,\"dynamic\":%u,"
                              "\"active\":%u,\"floating\":%u,\"shapes\":%u,\"meshShapes\":%u,\"hullShapes\":%u,\"primitiveShapes\":%u,\"shapeTriangles\":%llu,\"bytes\":%llu,\"stepsLastFrame\":%u,\"stepsTotal\":%llu,"
                              "\"stepMs\":%.3f,\"stepMsEma\":%.3f,\"lastBuildMs\":%.1f,\"fellOut\":%u,\"handOversHeld\":%llu,\"stepWaits\":%llu,\"stepErrors\":%llu}",
                              ps.worldExists ? "true" : "false", ps.simulating ? "true" : "false", ps.objects, ps.staticBodies, ps.kinematicBodies, ps.dynamicBodies,
                              ps.activeBodies, ps.floatingBodies, ps.shapes, ps.meshShapes, ps.hullShapes, ps.primitiveShapes, static_cast<unsigned long long>(ps.shapeTriangles),
                              static_cast<unsigned long long>(ps.bytes), ps.stepsLastFrame,
                              static_cast<unsigned long long>(ps.stepsTotal), ps.stepMs, ps.stepMsEma, ps.lastBuildMs,
                              ps.fellOut,
                              static_cast<unsigned long long>(ps.handOversHeld), static_cast<unsigned long long>(ps.stepWaits), static_cast<unsigned long long>(ps.stepErrors));
                out += physicsItem;
                const physics::PlayerState player = scene_.Physics().GetPlayerState();
                char playerItem[256];
                std::snprintf(playerItem, sizeof(playerItem),
                              ",\"player\":{\"exists\":%s,\"possessed\":%s,\"mode\":\"%s\",\"onGround\":%s,\"crouched\":%s,\"speed\":%.3f,\"feet\":[%.3f,%.3f,%.3f]}",
                              player.exists ? "true" : "false", player.possessed ? "true" : "false", physics::PlayerModeName(player.mode),
                              player.onGround ? "true" : "false", player.crouched ? "true" : "false", player.speed, player.feet.x, player.feet.y, player.feet.z);
                out += playerItem;
                const physics::HelmState& helm = scene_.Physics().GetHelm();
                char helmItem[480];
                std::snprintf(helmItem, sizeof(helmItem),
                              ",\"helm\":{\"exists\":%s,\"speed\":%.3f,\"throttle\":%.3f,\"rudder\":%.3f,\"immersion\":%.3f,\"bite\":%.3f,\"impact\":%.3f,\"waterHeight\":%.3f,\"position\":[%.3f,%.3f,%.3f],\"forward\":[%.4f,%.4f,%.4f],\"starboardY\":%.4f,\"buoyancyLead\":%.3f,\"trimTorque\":%.0f}",
                              helm.exists ? "true" : "false", helm.vessel.speed, helm.vessel.throttle, helm.vessel.rudder, helm.vessel.immersion, helm.vessel.bite, helm.vessel.impact, helm.vessel.waterHeight,
                              helm.position.x, helm.position.y, helm.position.z, helm.forward.x, helm.forward.y, helm.forward.z, helm.starboardY,
                              helm.vessel.buoyancyLead, helm.vessel.trimTorque);
                out += helmItem;
                // Every boat as the frame draws it: one that nobody steers rocks in another's wake (OceanWake -> OceanReadback).
                {
                    std::vector<physics::VesselHull> boats;
                    scene_.Physics().CollectHulls(boats);
                    out += ",\"vessels\":[";
                    for (size_t i = 0; i < boats.size(); ++i)
                    {
                        const physics::VesselHull& b = boats[i];
                        const float starboardY = b.up.z * b.forward.x - b.up.x * b.forward.z; // (up x forward).y
                        const float pitchDeg = std::asin(std::clamp(b.forward.y, -1.0f, 1.0f)) * 57.2957795f;
                        const float rollDeg = std::atan2(starboardY, b.up.y) * 57.2957795f;
                        // What the boat does by its own numbers (game::StatsOf): its card's, and a bot's.
                        const game::VesselStats stats = b.settings ? game::StatsOf(*b.settings, b.halfLength, b.rudderLever) : game::VesselStats{};
                        char boatItem[900];
                        std::snprintf(boatItem, sizeof(boatItem),
                                      "%s{\"geometry\":\"%s\",\"position\":[%.3f,%.3f,%.3f],\"forward\":[%.4f,%.4f,%.4f],\"pitchDeg\":%.3f,\"rollDeg\":%.3f,\"speed\":%.3f,\"atHelm\":%s,\"anchored\":%s,"
                                      "\"stats\":{\"topSpeed\":%.2f,\"toPlane\":%.2f,\"turnSpeed\":%.2f,\"turnRateDeg\":%.2f,\"turnRadius\":%.1f,\"lateralAccel\":%.2f,\"gripSpeed\":%.2f,\"braking\":%.2f,"
                                      "\"mass\":%.0f,\"thrust\":%.0f,\"surgeDrag\":%.3f,\"rudderForce\":%.3f,\"sideGrip\":%.3f,\"sideGripPlane\":%.3f,\"length\":%.2f,\"rudderLever\":%.2f}}",
                                      i > 0 ? "," : "", b.object ? b.object->GetGeometryPath().c_str() : "", b.position.x, b.position.y, b.position.z, b.forward.x, b.forward.y, b.forward.z,
                                      pitchDeg, rollDeg, b.speed, b.atHelm ? "true" : "false", b.anchored ? "true" : "false",
                                      stats.topSpeed, stats.toPlane, stats.turnSpeed, stats.turnRate * 57.2957795f, stats.turnRadius, stats.lateralAccel, stats.gripSpeed, stats.braking,
                                      stats.mass, stats.thrust, b.settings ? b.settings->surgeDrag : 0.0f, b.settings ? b.settings->rudderForce : 0.0f, b.settings ? b.settings->sideGrip : 0.0f, b.settings ? b.settings->sideGripPlane : 0.0f, stats.length,
                                      b.rudderLever);
                        out += boatItem;
                    }
                    out += "]";
                }
                // The race (game::Race): the bots at their helms, the player's own laps.
                out += scene_.RaceRef().ReadoutJson();
                // The boats' spray (vfx/VesselSpray): boats with emitters, drops a second thrown now, slam bursts so far.
                {
                    const vfx::VesselSpray& spray = scene_.VesselSprayRef();
                    char sprayItem[320];
                    std::snprintf(sprayItem, sizeof(sprayItem),
                                  ",\"spray\":{\"boats\":%zu,\"rate\":%.1f,\"sheetRate\":%.1f,\"bursts\":%d,\"sheetAlive\":%u,\"sheetCapacity\":%u,\"mistAlive\":%u,\"mistCapacity\":%u,\"hullEntries\":%llu}",
                                  spray.Boats(), spray.Rate(), spray.SheetRate(), spray.Bursts(), spray.SheetAlive(), spray.SheetCapacity(),
                                  spray.MistAlive(), spray.MistCapacity(), static_cast<unsigned long long>(spray.HullEntries()));
                    out += sprayItem;
                }
                // The wake's bow wave fronts (ocean/OceanWake: the two foam arms a boat): alive, their points, handed to the GPU.
                if (const OceanRenderable* wakeOcean = scene_.FindOceanRenderable())
                {
                    std::uint32_t fronts = 0, points = 0, drawn = 0;
                    wakeOcean->GetWakeFrontReadout(fronts, points, drawn);
                    std::uint32_t breaks = 0;
                    float longestLit = 0.0f;
                    wakeOcean->GetWakeFrontBreaks(breaks, longestLit);
                    char wakeItem[192];
                    std::snprintf(wakeItem, sizeof(wakeItem), ",\"wake\":{\"fronts\":%u,\"frontPoints\":%u,\"frontsDrawn\":%u,\"breaks\":%u,\"longestLit\":%.2f}",
                                  fronts, points, drawn, breaks, longestLit);
                    out += wakeItem;
                }
                // The objects OceanBuoyancy floats (not the physics world's boats) and the water the CPU reads under them.
                {
                    OceanRenderable* ocean = scene_.FindOceanRenderable();
                    std::vector<OceanBuoyancy::Readout> floaters;
                    scene_.Buoyancy().Readouts(ocean, floaters);
                    if (ocean)
                    {
                        const OceanReadback& readback = ocean->GetReadback();
                        const OceanReadback::ShoreParams& shore = readback.Shore();
                        char shoreItem[200];
                        std::snprintf(shoreItem, sizeof(shoreItem),
                                      ",\"oceanReadback\":{\"surface\":%s,\"adoptedFrame\":%llu,\"shore\":%s,\"shoreCenter\":[%.2f,%.2f],\"shoreExtent\":%.1f}",
                                      readback.HasSurface() ? "true" : "false", static_cast<unsigned long long>(readback.AdoptedFrame()),
                                      readback.HasShore() ? "true" : "false", shore.view.x, shore.view.y,
                                      shore.view.w > 0.0f ? 1.0f / shore.view.w : 0.0f);
                        out += shoreItem;
                    }
                    out += ",\"floaters\":[";
                    for (size_t i = 0; i < floaters.size(); ++i)
                    {
                        const OceanBuoyancy::Readout& f = floaters[i];
                        char floatItem[400];
                        std::snprintf(floatItem, sizeof(floatItem),
                                      "%s{\"geometry\":\"%s\",\"center\":[%.3f,%.3f,%.3f],\"heave\":%.4f,\"heaveVelocity\":%.4f,\"tiltDeg\":%.3f,\"water\":%.4f,\"shore\":%s,\"waterDepth\":%.3f,\"damping\":%.3f}",
                                      i > 0 ? "," : "", f.geometry.c_str(), f.center.x, f.center.y, f.center.z, f.heave, f.heaveVelocity, f.tiltDeg,
                                      f.water, f.shore ? "true" : "false", f.waterDepth, f.damping);
                        out += floatItem;
                    }
                    out += "]";
                }
                // Who holds the keys: a key a panel's focus keeps never reaches InputManager (App::WndProc), and a script's posted key cannot tell that from a broken action.
                char uiItem[96];
                std::snprintf(uiItem, sizeof(uiItem), ",\"ui\":{\"wantsKeyboard\":%s,\"wantsMouse\":%s,\"typing\":%s}",
                              renderer_.ImGui().WantsKeyboard() ? "true" : "false", renderer_.ImGui().WantsMouse() ? "true" : "false", renderer_.ImGui().WantsTextInput() ? "true" : "false");
                out += uiItem;
            }
            // Plan S10e: the VSM page pool's load and the dynamic-resolution bias it drives (a kFrameCount-old frame).
            {
                const VirtualShadowMap::DynamicResStats& dr = scene_.Vsm().DynamicRes();
                const VirtualShadowMap::RemapStats& rs = scene_.Vsm().Remaps(); // S10c: views moved with their light
                const VirtualShadowMap::DistantStats& ds = scene_.Vsm().Distant(); // S10d: distant lights, round robin
                char vsmItem[512];
                std::snprintf(vsmItem, sizeof(vsmItem),
                              ",\"vsmPool\":{\"valid\":%s,\"load\":%.3f,\"refused\":%d,\"lodBias\":%.3f,"
                              "\"remapFrames\":%llu,\"remapMoved\":%u,\"remapDropped\":%u,"
                              "\"parked\":%u,\"parkedBack\":%u,\"parkedExpired\":%u,\"parkedStale\":%u,"
                              "\"distantLights\":%u,\"distantUpdates\":%llu,\"distantTurned\":%llu,\"mobileLights\":%u}",
                              dr.valid ? "true" : "false", dr.poolLoad, dr.refused, dr.lodBias,
                              static_cast<unsigned long long>(rs.frames), rs.moved, rs.dropped,
                              rs.parked, rs.returned, rs.expired, rs.stale,
                              ds.lights, static_cast<unsigned long long>(ds.updates),
                              static_cast<unsigned long long>(ds.turned), scene_.GetLightManager().GetMobileLightCount());
                out += vsmItem;
            }
            // Tessy N0/N3: the VSM page render's (page, caster) pairs and their clusters, frame N - 3; the master switch,
            // the cluster view's verdict and the meshlet records of the cluster rows (atlases, S7's static, the view).
            {
                const VirtualShadowMap::TessyStats& t = scene_.Vsm().GetTessyStats();
                const ShadowGpuData& sg = scene_.ShadowGpu();
                const auto& as = sg.TessyAtlasStats();
                char tessyItem[2048];
                std::snprintf(tessyItem, sizeof(tessyItem),
                              ",\"tessy\":{\"enabled\":%s,\"debugView\":%u,\"view\":\"%s\",\"gbuffer\":\"%s\",\"gbufferTwins\":%u,\"lod\":%s,\"dagGroups\":%u,\"dagClusters\":%u,\"camDag\":%s,\"atlasRecords\":%u,"
                              "\"staticRecords\":%u,\"viewRecords\":%u,\"viewsNotFit\":%u,\"rowClustersTested\":%u,\"camDeferred\":%u,"
                              "\"camTris\":%u,\"camDagClusters\":%u,\"rowRectCulled\":%u,"
                              "\"sdsm\":\"%s\",\"sdsmRecords\":%u,\"sdsmNotFit\":%u,\"sdsmClustersTested\":%u,\"sdsmTris\":%u,"
                              "\"sdsmRectCulled\":%u,\"rectCulled\":%u,"
                              "\"valid\":%s,\"on\":%s,\"knob\":%s,\"offReason\":\"%s\",\"meshShader\":%s,"
                              "\"rasterKnob\":%u,\"meshShaderOffReason\":\"%s\",\"groupsWithClusters\":%u,"
                              "\"groups\":%u,\"clusterTable\":%u,\"wholePairs\":%u,\"wholeTris\":%u,\"tessyPairs\":%u,"
                              "\"tessyWholeTris\":%u,\"runs\":%u,\"clustersTested\":%u,\"clusters\":%u,\"records\":%u,\"clusterTris\":%u,"
                              "\"clustersLost\":%u,\"runsLost\":%u,\"padMisses\":%u,\"padMissMaxUm\":%u,"
                              "\"padNeedCm\":%u,\"padGotCm\":%u,\"padMaxOffCm\":%u,\"padDCm\":%u,\"padFCm\":%u}",
                              tessy::g_enabled_game ? "true" : "false", tessy::g_debugView_game, sg.TessyViewVerdict(),
                              sg.TessyGBufferVerdict(), sg.TessyGBufferRuns(), tessy::g_lod_game ? "true" : "false",
                              sg.TessyDagGroups(), sg.TessyDagClusters(), sg.TessyCameraDagThisFrame() ? "true" : "false",
                              as[0], as[1], as[2], as[3], as[4], as[5], as[6], as[7], as[16],
                              sg.TessySunVerdict(), as[8], as[9], as[10], as[11], as[12], t.rectCulled,
                              t.valid ? "true" : "false", t.tessyOn ? "true" : "false", tessy::g_vsm_game ? "true" : "false",
                              scene_.Vsm().TessyOffReason(), t.tessyMs ? "true" : "false", tessy::g_vsmMeshShader_game ? 1u : 0u,
                              scene_.Vsm().TessyMsOffReason(), sg.TessyGroupsWithClusters(), sg.TessyGroupsTotal(),
                              sg.TessyClusterCount(), t.wholePairs, t.wholeTris, t.tessyPairs, t.tessyWholeTris, t.runs,
                              t.clustersTested, t.clusters, t.records, t.clusterTris, t.clustersLost, t.runsLost,
                              t.padMisses, t.padMissMaxUm, t.padNeedCm, t.padGotCm, t.padMaxOffCm, t.padDCm, t.padFCm);
                out += tessyItem;
            }
            // The sky's diffuse fill as lighting_cs takes it: the procedural sky's environment capture, or the
            // HDRI skybox with its lux calibration, times the light's skyFillIntensity (FillLightingConstants).
            if (const Skybox* sky = scene_.GetSkybox())
            {
                const float fill = scene_.SunLightRef().GetSkyFillIntensity();
                char skyItem[256];
                std::snprintf(skyItem, sizeof(skyItem),
                              ",\"skyFill\":{\"environment\":%s,\"ibl\":%s,\"exposure\":%.6g,\"hdriLux\":%.6g,"
                              "\"physicalScale\":%.6g,\"fillIntensity\":%.4g,\"irradianceScale\":%.6g}",
                              sky->HasEnvironment() ? "true" : "false", sky->HasIbl() ? "true" : "false",
                              sky->GetExposure(), sky->GetIlluminanceLux(), sky->PhysicalScale(), fill,
                              sky->GetExposure() * fill);
                out += skyItem;
            }
            if (historyRows <= 0)
            {
                return out + "}";
            }
            // Per-frame rows, oldest first: a flicker is read here, not across calls.
            std::vector<ExposureMetering::HistoryRow> rows(static_cast<size_t>(historyRows));
            rows.resize(renderer_.Exposure().History(rows.data(), rows.size()));
            out += ",\"history\":{\"columns\":[\"frame\",\"adaptedEv100\",\"targetEv100\",\"preExposure\","
                   "\"probeMeanR\",\"probeMeanG\",\"probeMeanB\",\"probeMaxLuma\",\"sunProbeLuma\","
                   "\"sunProbeShare\"],\"rows\":[";
            char row[256];
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const ExposureMetering::HistoryRow& r = rows[i];
                std::snprintf(row, sizeof(row), "%s[%llu,%.4f,%.4f,%.6g,%.5g,%.5g,%.5g,%.5g,%.5g,%.4f]",
                    i ? "," : "", static_cast<unsigned long long>(r.frame), r.adaptedEv100, r.targetEv100,
                    static_cast<double>(r.preExposure), r.probeMean[0], r.probeMean[1], r.probeMean[2],
                    r.probeMaxLuma, r.sunProbeLuma, r.sunProbeShare);
                out += row;
            }
            out += "]}}";
            return out;
        }
    private:
        // set_settings / reset_settings through the scene: what a series changed, and the scalability pins before it.
        settings::SettingsSeries::Host SeriesHost()
        {
            settings::SettingsSeries::Host h;
            h.read = [this](const std::string& n, float& out) { return ReadSweepValue(scene_, settings_, n, out); };
            h.apply = [this](const std::string& n, float v) { return ApplySweepValue(scene_, settings_, n, v); };
            h.isAction = [](const std::string& n) {
                const settings::SettingDesc* row = settings::Registry().Find(n);
                return row != nullptr && (row->flags & settings::SettingAction) != 0u;
            };
            h.rewritesKnobs = [](const std::string& n) { return n.rfind("sg.", 0) == 0; };
            h.knobs = [] {
                std::vector<std::string> out;
                for (const settings::ScalabilityKnob& k : settings::ScalabilityKnobs()) { out.emplace_back(k.name); }
                return out;
            };
            h.pins = [] { return settings::TheScalability().Pins(); };
            h.pin = [](const std::string& n, bool on) {
                if (on) { settings::TheScalability().Pin(n); }
                else { settings::TheScalability().Unpin(n); }
            };
            return h;
        }
        Scene& scene_;
        SceneRenderSettings& settings_;
        Renderer& renderer_;
        settings::SettingsSeries series_;
    };
}
#endif

void App::ApplyFixedSettings(Scene& scene)
{
    for (const auto& kv : Boot().frame.fixedSettings)
    {
        const bool known = ApplySweepValue(scene, appController_.SettingsRef(), kv.first, kv.second);
        if (known)
        {
            LOG_INFO(logging::LogCategory::App, "--set {} = {}", kv.first, kv.second);
            settings::TheScalability().Pin(kv.first); // stronger than the graphics levels for the whole run (§3.3)
        }
        else
        {
            LOG_WARNING(logging::LogCategory::App, "--set {} = {}: UNKNOWN SETTING (ignored)",
                        kv.first, kv.second);
        }
    }
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
    // Game thread plan (docs/game_thread_plan.md section 3.4): boot and shutdown are the hand-over role -- nothing
    // else runs, both halves' state is built or torn down here. The frame loop below gives each phase its role.
    threading::SetCurrentRole(threading::ThreadRole::Sync);
    knob::Exchange(); // G5: the command line set the game's knobs; the boot below reads the render half's
    {
        // The systems (renderer, scene, input, levels) are built here, before any of their own
        // scopes exist: most of what they allocate at construction is the renderer's.
        MEM_SCOPE(MemTag::Renderer);
        systems_ = std::make_unique<Systems::AppSystems>();
        Systems::Init(systems_.get());
    }

    {
        auto& renderer = systems_->renderer;
        auto& scene = systems_->scene;
        auto& input = systems_->input;
        auto& levelManager = systems_->levelManager;

        {
            BOOT_SCOPE("App::InitWindow");
            MEM_SCOPE(MemTag::Renderer);
            InitWindow(hInstance, nCmdShow);
        }
        {
            BOOT_SCOPE("TaskSystem::Start");
            MEM_SCOPE(MemTag::Tasks);
            TaskSystem::Get().Start(static_cast<unsigned int>(std::thread::hardware_concurrency() * 0.75f));
        }
        //TaskSystem::Get().Start(8);
        Profiler::Get().SetThreadName("MainThread");

        InitScene();

        // Project-wide quality settings are applied after the renderer and level exist, so the
        // exact same live setters used by the developer window can rebuild dependent targets.
        {
            MEM_SCOPE(MemTag::Renderer);
            appController_.InitializeGraphicsSettings(renderer, scene);
            // The graphics levels (docs/scalability_plan.md) over the settings file and the level: the player's saved
            // choice, or --scalability's. Nothing saved and nothing asked = nothing managed: today's picture.
            settings::Scalability& scalability = settings::TheScalability();
            SceneRenderSettings& renderSettings = appController_.SettingsRef();
            scalability.SetAccess({
                [&scene, &renderSettings](const std::string& name, float value) { return ApplySweepValue(scene, renderSettings, name, value); },
                [&scene, &renderSettings](const std::string& name, float& value) { return ReadSweepValue(scene, renderSettings, name, value); },
                [&renderer]() { return renderer.IsDlssAvailable(); },
                // One batch for the target knobs: the reflections' scales here, the DLSS mode and the render scale taken
                // from their requests inside it (not at the next frame's safe point, which would recreate again).
                [&renderer]()
                {
                    threading::WaitRenderForGame("scalability apply");
                    renderer.BeginTargetBatch();
                },
                [&renderer]() -> int
                {
                    const int before = renderer.TargetRecreations();
                    threading::RoleScope renderRole(threading::ThreadRole::Render);
                    if (g_requestDlssMode >= 0)
                    {
                        renderer.SetDlssMode(static_cast<sl::DLSSMode>(g_requestDlssMode));
                        g_requestDlssMode = -1;
                    }
                    if (g_requestRenderScale >= 0.0f)
                    {
                        if (!renderer.IsDlssActive()) { renderer.SetRenderResolutionScale(g_requestRenderScale); }
                        g_requestRenderScale = -1.0f;
                    }
                    renderer.EndTargetBatch();
                    return renderer.TargetRecreations() - before;
                } });
            if (scalability.LoadTable())
            {
                scalability.LoadUser();
                // The boot's own switches are stronger than the levels, as --set is: a --shadow-mode=vsm GBV run must not
                // be turned into SDSM by a saved Low, nor --dlss= undone by the first level load.
                if (render::g_shadowModeFromCli_game) { scalability.Pin("shadow.mode"); }
                if (Boot().frame.dlssMode >= 0) { scalability.Pin("dlss.mode"); }
                if (Boot().frame.scalability >= -1)
                {
                    scalability.SetPreset(Boot().frame.scalability);
                    if (Boot().frame.scalability == -1) { scalability.SetResolutionLevel(-1); } // off: nothing managed at all
                    LOG_INFO(logging::LogCategory::App, "--scalability: {}", Boot().frame.scalability >= 0
                        ? scalability.Table().levelNames[static_cast<std::size_t>(Boot().frame.scalability)] : std::string("off"));
                }
                else if (Boot().frame.scalability == -3)
                {
                    scalability.RequestAutodetect(build::Packaged()); // --scalability=auto: a dev build's file stays as it is
                }
#if TC_PACKAGED
                else
                {
                    // A packaged build's first start on a system -- no measurement saved, another GPU, or another
                    // computer (its CPU and memory) since the last one: auto-detect (§5), FORCED (Scalability.h
                    // AutodetectForced). A driver update alone keeps the player's levels (it would replace a choice made
                    // by hand). A record from before 2026-11-17 has no "system": its GPU alone decides, as it did. A
                    // developer build measures only when asked.
                    std::uint64_t vram = 0;
                    const std::string gpu = GpuIdentity(renderer.GetDevice(), vram);
                    const std::string system = SystemIdentity();
                    const auto adapterOnly = [](const std::string& identity) { return identity.substr(0, identity.find(" / ")); };
                    const nlohmann::json record = nlohmann::json::parse(settings::Scalability::ReadUserSection("autodetect"), nullptr, false);
                    const auto recorded = [&record](const char* key) {
                        return record.is_object() && record.contains(key) && record[key].is_string() ? record[key].get<std::string>() : std::string();
                    };
                    const std::string measuredOn = recorded("gpu");
                    const std::string measuredSystem = recorded("system");
                    const char* why = measuredOn.empty()                                  ? "the first start on this system"
                                    : adapterOnly(measuredOn) != adapterOnly(gpu)          ? "another GPU"
                                    : !measuredSystem.empty() && measuredSystem != system ? "another computer"
                                                                                          : nullptr;
                    if (why)
                    {
                        LOG_INFO(logging::LogCategory::App, "auto-detect: {} -- {}, {}{}", why, gpu, system,
                                 measuredOn.empty() ? std::string() : " (measured on " + measuredOn +
                                     (measuredSystem.empty() ? std::string() : ", " + measuredSystem) + ")");
                        scalability.RequestAutodetect(true);
                    }
                }
#endif
                scalability.Apply("boot");
            }
            GameSettingsMenu::ApplySavedDisplay(); // a 2026-11-13 file's VSync and frame-rate limit (they are graphics now)
            settings::ApplySavedImageAndControls(); // the player's brightness, field of view, look
            knob::Exchange(); // graphics_settings.json and the graphics levels set the game's knobs
        }
#if WITH_EDITOR
        // The editor MCP's set_settings / get_readouts reach the app through this; it outlives every
        // frame that could call it and is withdrawn before the systems go away.
        static McpRuntimeHost mcpRuntimeHost(scene, appController_.SettingsRef(), renderer);
        editormcp::SetRuntimeHost(&mcpRuntimeHost);
#endif

        // Dumped HERE, not only at exit: under Debug + GBV a boot takes minutes, and a run that is
        // abandoned (or that hangs in frame 1) must still have produced its boot timing. Dumped
        // again after the first frame, because "device ready" and "first pixel" are different
        // questions -- PSO warmup and the first upload flush land between them.
        boot::Dump("InitScene complete");

        // "--dlss=<mode>": apply the boot override once the device, the DLSS handler and the
        // deferred targets exist, so this takes exactly the same path as the dev-window combo
        // (resolution update + target recreation) rather than a second, untested init order.
        if (Boot().frame.dlssMode >= 0)
        {
            renderer.SetDlssMode(static_cast<sl::DLSSMode>(Boot().frame.dlssMode));
        }

        // 1 ms scheduler granularity for the frame cap's Sleep (WaitOutFrameBudget). Since
        // Windows 10 2004 the resolution is per process: one that never asked has its sleeps
        // rounded up to the 15.6 ms default tick, whatever other processes requested, and a 6 ms
        // sleep inside an 8.3 ms budget would take the whole tick. UE sets the same at boot.
        timeBeginPeriod(1);

        MSG msg = {};
        double lastTime = GetTimeSeconds();
        bool firstFrameDumped = false;
        double frameStart = GetTimeSeconds();
        double now = lastTime;
        float deltaTime = 0.0f;
        float realDeltaTime = 0.0f; // the wall clock's: what the fps counter and the shader watcher go by (the world's is deltaTime)
        // The frame's phases (plan sections 3.2-3.4). The loop runs as the GAME half; the render half's calls take the
        // Render role, the hand-over between them the Sync role. On one thread (frame.renderThread 0) a frame is: its
        // start, the messages, the UI, Scene::Tick, the hand-over, Scene::Render, the work after Present. With a render
        // thread (1, plan G8) Scene::Render runs THERE, and this thread meanwhile runs the next frame's game half --
        // the messages, the clock, the UI and the editor, Scene::Tick (G8b) -- and then waits for it: the frame start,
        // the hand-over and the screenshots after Present run while it is idle.
        threading::SetCurrentRole(threading::ThreadRole::Game);
        threading::RenderThread renderThread;

        const auto beginFrame = [&]()
        {
            if (!firstFrameDumped && renderer.GetTotalFrameNumber() > 0)
            {
                firstFrameDumped = true;
                boot::Dump("first frame presented");
            }
            Profiler::Get().BeginFrame(renderer.GetTotalFrameNumber());
            logging::SetFrameNumber(renderer.GetTotalFrameNumber());
            threading::RoleScope renderRole(threading::ThreadRole::Render);
            render::g_renderStats.NextFrame(); // snapshot last frame's draw/primitive counts
            render::g_visibilityStats.NextFrame(); // occlusion plan S0: per-view visibility, same moment
            // No join of tracked tasks here: the render half joins its own before the frame is submitted
            // (SceneRenderer::Render), and a game-half join would wait on nothing (TaskSystem: one list per role).
            renderer.BeginFrame();
            crash::SelfTestFrame(); // --crash-selftest=terminate-frame: std::terminate here, on the game thread
            render::PassThrowSelfTestFrame(); // --crash-selftest=pass-throw: a run whose pass threw and went on ends here
            if (render::RetireSelfTestFrame(renderer)) { isRunning_ = false; } // --retire-selftest: the verdict is in
            texdecode::ThrowSelfTestFrame(); // --throw-selftest=texdecode: the decode it requests (nothing otherwise)
            if (diag::ThrowSelfTestFrame()) { isRunning_ = false; } // --throw-selftest, view-throw: the verdict is in
#if PROF_GPU_ENABLED
            Profiler::Get().CollectGpuResults();
#endif
        };
        const auto pumpMessages = [&]() -> bool // false on WM_QUIT
        {
            CPU_SCOPE(ProfilerScopes::kWinMessages);
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) {
                    break; // Break out of the loop and stop rendering
                }
            }
            return msg.message != WM_QUIT;
        };
        const auto beginUi = [&]()
        {
            // Everything from here to AppController::Tick used to be a blank stretch on the
            // timeline between "Win Messages" and the first scoped call — three unattributed
            // calls, one of which (the ImGui frame start) is not obviously cheap.
            {
                CPU_SCOPE(ProfilerScopes::kBeginImGuiFrame);
                MEM_SCOPE(MemTag::Ui);
                renderer.ImGui().BeginFrame();
            }
            {
                CPU_SCOPE(ProfilerScopes::kProfilerTick);
                MEM_SCOPE(MemTag::Profiler);
                Profiler::Get().Tick();
            }
        };
        const auto advanceTime = [&]()
        {
            now = GetTimeSeconds();
            deltaTime = static_cast<float>(now - lastTime);
            lastTime = now;

            deltaTime = Math::Clamp(deltaTime, 1e-6f, 0.1f);
            realDeltaTime = deltaTime;
            if (g_timeScale != 1.0f) { deltaTime = Math::Clamp(deltaTime * g_timeScale, 1e-6f, 0.1f); }
#if WITH_EDITOR
            if (g_recordDt > 0.0f)
            {
                deltaTime = editormcp::ScreenshotCapturesThisFrame() ? g_recordDt : 1e-6f;
            }
#endif

            // "--cam-fly=x,z": drift the camera at a constant world velocity (m/s). The ONLY
            // headless way to exercise motion-dependent paths — window relocations (wetness,
            // surf sim), clipmap snapping, DLSS history — a fixed --cam-pos never triggers them.
            if (Boot().frame.camFly[0] != 0.0f || Boot().frame.camFly[1] != 0.0f || Boot().frame.camFlyYaw != 0.0f)
            {
                static const double flyStart = GetTimeSeconds();
                if (now - flyStart >= (double)Boot().frame.camFlyDelay)
                {
                    Camera& cam = scene.CameraRef();
                    const float3 p = cam.GetPosition();
                    cam.SetPosition(float3(p.x + Boot().frame.camFly[0] * deltaTime, p.y,
                                           p.z + Boot().frame.camFly[1] * deltaTime));
                    if (Boot().frame.camFlyYaw != 0.0f)
                    {
                        cam.AddYaw(DirectX::XMConvertToRadians(Boot().frame.camFlyYaw) * deltaTime);
                    }
                }
            }
            if (g_camFlyForward != 0.0f)
            {
                Camera& cam = scene.CameraRef();
                cam.SetPosition(cam.GetPosition() + cam.GetDirection() * (g_camFlyForward * deltaTime));
            }
            if (Boot().frame.camOrbit[0] > 0.0f)
            {
                static const double orbitStart = GetTimeSeconds();
                static float3 orbitCentre;
                static bool orbitInit = false;
                static std::uint64_t orbitFrames = 0; // the fixed-step clock (third value)
                Camera& cam = scene.CameraRef();
                if (!orbitInit) { orbitCentre = cam.GetPosition(); orbitInit = true; }
                const double t = Boot().frame.camOrbit[2] > 0.0f
                    ? static_cast<double>(orbitFrames++) * Boot().frame.camOrbit[2] - (double)Boot().frame.camFlyDelay
                    : now - orbitStart - (double)Boot().frame.camFlyDelay;
                if (t >= 0.0)
                {
                    const float ang = DirectX::XMConvertToRadians(Boot().frame.camOrbit[1]) * static_cast<float>(t);
                    const float r = Boot().frame.camOrbit[0];
                    cam.SetPosition(float3(orbitCentre.x + r * std::cos(ang), orbitCentre.y,
                                           orbitCentre.z + r * std::sin(ang)));
                    // Tangent of the circle; yaw is measured from +Z toward +X.
                    cam.SetYawPitch(std::atan2(-std::sin(ang), std::cos(ang)), cam.GetPitch());
                }
            }
        };
        const auto renderUpkeep = [&]()
        {
            // The render half's own upkeep (fonts, the shader watcher, PSO hot reloads) and the memory line. With a
            // render thread too it runs here, on this thread, while that one is idle.
            threading::RoleScope renderRole(threading::ThreadRole::Render);
            {
                MEM_SCOPE(MemTag::Renderer);
                renderer.Tick(realDeltaTime); // the wall clock: frame.timeScale x12 read 465 fps as 39, frame.recordDt as a million
            }
            // The memory line (rendering/core/MemoryReport.h): every 5 s, after the previous
            // frame's work is joined so the providers read settled state.
            render::TickMemoryReport(renderer.GetDevice(), now); // providers tex.ret / rt.as.bin / rt.as.retired; the rest is the GPU census
        };
        const auto tickUi = [&]()
        {
            MEM_SCOPE(MemTag::Ui); // the HUD, the developer window, the editor (its own scope narrows it)
            appController_.Tick(input, renderer, scene, levelManager, deltaTime);
        };
        const auto tickScene = [&]()
        {
            MEM_SCOPE(MemTag::Scene);
            scene.Tick(deltaTime);
            levelManager.Tick(deltaTime);
        };
        const auto loadPendingLevel = [&]()
        {
            if (auto pendingLevel = levelManager.ConsumePendingLevelRequest())
            {
                MEM_SCOPE(MemTag::Scene); // the loaders' own scopes (meshes, textures, ocean) narrow it
                BOOT_SCOPE("Level switch (runtime)"); // game thread plan G10d: what the frame stands still for
                {
                    BOOT_SCOPE("WaitForPreviousFrame (level switch)");
                    renderer.WaitForPreviousFrame();
                }

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
                        {
                            BOOT_SCOPE("UploadBatch::SubmitAndWait");
                            levelBatch.SubmitAndWait(&renderer);
                        }
                        appController_.ApplySceneGraphicsSettings(scene);
                        // The level set its own clouds, fog, bloom, GTAO...: the graphics levels' ceilings again.
                        settings::TheScalability().OnContentReloaded("level load");
                        ++harness::g_levelLoadsCompleted;
                        // --scene-stress pins --set again over every level it loads (the level's sections overwrote
                        // them): otherwise a pass behind a setting would be validated in its default (R3.3e).
                        if (Boot().harness.sceneStress) { ApplyFixedSettings(scene); }
                    }
                }
#if WITH_EDITOR
                appController_.OnLevelChangeRequestCompleted(*pendingLevel, levelLoaded, renderer, scene, levelManager);
#endif
                if (levelLoaded) { appController_.OnLevelLoaded(renderer, scene, input); }
            }
        };
        const auto closeUi = [&]()
        {
            appController_.WaitForHudBuild();
            renderer.ImGui().EndFrame(); // the UI frame is final here; the hand-over gives it to the render half
        };
        const auto handOver = [&]()
        {
            // The hand-over (plan section 3.3): the frame's values from game state go to the render half.
            threading::RoleScope syncRole(threading::ThreadRole::Sync);
            scene.Sync(&renderer);
            renderer.HandOverImGui(); // G8b: the UI's textures into this frame's slot, the atlas uploaded
            appController_.PublishHud(renderer); // G6: the HUD's lines into the render half's text buffer
#if WITH_EDITOR
            editormcp::ServiceHandOver(); // G6: the MCP calls that read the render half (get_readouts)
#endif
        };
        const auto renderScene = [&]()
        {
            MEM_SCOPE(MemTag::Renderer); // the render graph's tasks inherit it
            scene.Render(&renderer);
        };
        // After Present: the harness, the screenshots, the profiler's frame end.
        // The capture harnesses (architecture plan R3.3; app/harness/CaptureHarnesses.h), run by afterFrame.
        harness::ShotHarness shotHarness([](Scene& s, SceneRenderSettings& rs, const std::string& name, float value)
                                         { return ApplySweepValue(s, rs, name, value); });
        harness::ProfDumpHarness profDumpHarness;
        harness::TraceHarness traceHarness;
        harness::ScalabilityCheckHarness scalabilityCheckHarness(
            [](Scene& s, const SceneRenderSettings& rs, const std::string& name, float& out) { return ReadSweepValue(s, rs, name, out); });
        harness::MemGateHarness memGateHarness;
        harness::RtAuditHarness rtAuditHarness;
        harness::IntentHarness intentHarness;
        harness::SettingsDumpHarness settingsDumpHarness(
            [](Scene& s, const SceneRenderSettings& rs, const std::string& name, float& out) { return ReadSweepValue(s, rs, name, out); },
            [this](harness::FrameContext& ctx, bool reset, std::vector<std::string>& lines) {
                GraphicsSettingsManager& g = appController_.GraphicsSettings();
                if (reset) // writes user_settings.json: a gate's copy
                {
                    const std::string& one = Boot().harness.graphicsResetSetting;
                    if (one.empty()) { g.ResetAll(ctx.renderer, ctx.scene, ctx.settings); }
                    else { g.ResetSetting(one, ctx.renderer, ctx.scene, ctx.settings); }
                }
                g.DescribeFile(ctx.renderer, ctx.scene, ctx.settings, reset ? "graphicsReset:" : "graphics:", lines);
            });
        std::unique_ptr<harness::Harness> sceneStressHarness = harness::MakeSceneStressHarness(hWnd_, renderer, scene, levelManager);
        // R3.4: the run's harnesses, one list for each of afterFrame's three places, in the order they always ran there;
        // each asked whether its switch is on. Before the renderer's requests: a scalability step's dlss.mode is taken
        // in the same frame. After Present: the captures, the dumps, the stress's churn. After the frame's end: the gates
        // that judge it.
        harness::Harness* const harnessesBeforeRequests[] = { &scalabilityCheckHarness };
        harness::Harness* const harnessesAfterPresent[] = { &shotHarness, &intentHarness, &profDumpHarness, &settingsDumpHarness,
                                                            &traceHarness, sceneStressHarness.get() };
        harness::Harness* const harnessesAfterFrameEnd[] = { &memGateHarness, &rtAuditHarness };
        const auto runHarnesses = [](harness::Harness* const* list, std::size_t count, harness::FrameContext& ctx)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                if (list[i]->Wanted()) { list[i]->OnFrame(ctx); }
            }
        };
        const auto afterFrame = [&]()
        {
            // "--set=<name>:<value>;...": pin settings for the run. Applied once, here, because the
            // scene has to exist first — several names in the table reach through it. Same
            // dispatcher as --sweep, so a name works in both or in neither.
            if (!Boot().frame.fixedSettings.empty())
            {
                static bool fixedApplied = false;
                if (!fixedApplied)
                {
                    fixedApplied = true;
                    ApplyFixedSettings(scene);
                    // Occlusion plan S0: the stress grid, once the level is in and every --set is applied.
                    if (g_sceneReplicateRequest > 1u)
                    {
                        ReplicateLevelObjects(scene, renderer, std::string(levelManager.GetActiveLevelSourcePath()),
                                              g_sceneReplicateRequest);
                        g_sceneReplicateRequest = 0u;
                    }
                }
            }

            // Auto-detect (§5): one step a frame while it runs, nothing otherwise.
            {
                static ScalabilityAutodetect autodetect;
                StepScalabilityAutodetect(autodetect, renderer, GetTimeSeconds());
            }
            harness::FrameContext harnessFrame{ renderer, scene, levelManager, appController_.SettingsRef(), isRunning_ };
            // "--scalability-check": one step a frame; its verdict ends the run (app/harness/GateHarnesses.h).
            runHarnesses(harnessesBeforeRequests, std::size(harnessesBeforeRequests), harnessFrame);

            // dlss.mode asked for by --set / set_settings: here, where the frame has been presented.
            if (g_requestDlssMode >= 0)
            {
                threading::RoleScope renderRole(threading::ThreadRole::Render); // the renderer's targets
                renderer.SetDlssMode(static_cast<sl::DLSSMode>(g_requestDlssMode));
                g_requestDlssMode = -1;
            }
            // aa.renderScale (the resolution group without DLSS): the same safe point. Under an active DLSS the
            // upscaler owns the scale, as the dev window's AA tab has it.
            if (g_requestRenderScale >= 0.0f)
            {
                threading::RoleScope renderRole(threading::ThreadRole::Render);
                if (!renderer.IsDlssActive()) { renderer.SetRenderResolutionScale(g_requestRenderScale); }
                g_requestRenderScale = -1.0f;
            }
            // The window's mode, monitor or size (the menu's Display, --set display.*): its WM_SIZE asks the renderer for
            // the new size, which the next frame's BeginFrame takes.
            {
                display::DisplaySettings request;
                if (display::TakeRequest(request)) { display::Apply(hWnd_, request); }
            }

#if WITH_EDITOR
            // A screenshot Claude Code asked for over MCP. Same safe point as --shot below: the
            // frame has been presented and the GPU can be waited on. One uncontended lock when
            // nobody asked.
            {
                threading::RoleScope renderRole(threading::ThreadRole::Render); // after Present: the render half's
                editormcp::ServiceScreenshot(renderer, scene.RenderCamera()); // the camera of the frame it captures
            }
#endif

            // --shot, --shot-count, --sweep; --intent's end (the verdict, or the timeout); --profdump; --settings-dump (R2.0);
            // --trace; --scene-stress's churn (R3.3e).
            runHarnesses(harnessesAfterPresent, std::size(harnessesAfterPresent), harnessFrame);


            Profiler::Get().EndFrame();
            memtrace::OnFrameEnd(renderer.GetTotalFrameNumber()); // the frame marker; a delayed --memtrace start
            // --mem-selftest's leak, --mem-readout's samples and verdict; --rt-audit's verdict (its probes are in the loop).
            runHarnesses(harnessesAfterFrameEnd, std::size(harnessesAfterFrameEnd), harnessFrame);
        };
        const auto paceFrame = [&]()
        {
            // Frame budget = the tighter of two caps: the user's (render::g_maxFps_game, UE t.MaxFPS)
            // and the model throttle below. One wait serves both (WaitOutFrameBudget).
            //
            // While the local model is answering, STOP rendering flat out.
            //
            // The editor draws the atoll in 1.56 ms, which is about 640 frames a second,
            // and every one of them is the main thread plus the render graph's workers on
            // every core. The model's experts run on those same cores, and the bill was
            // measured: a phrase that takes 21 s on an idle machine takes 57 s inside a
            // running editor, and a chat's first token 33 s instead of 5. The user found
            // it from the outside -- alt-tabbing away made the model noticeably faster,
            // because Windows was doing this throttling for us and only when unfocused.
            //
            // Nobody needs 640 fps while waiting for an answer. At 30 the editor still
            // feels live and hands back about 95% of the duty cycle. This costs nothing
            // when no request is in flight, which is almost always.
            // 30 while someone waits on an answer; 120 while the model does background work
            // it was never asked for. The second number exists because the startup warmup
            // runs for about 44 s, and holding the editor at 30 for that long right after it
            // opens trades frames the user IS looking at for an answer nobody requested.
            // 120 still hands over most of the duty cycle -- the unthrottled frame is 1.56 ms.
            const double busyFrameSeconds =
                g_modelBusy.load(std::memory_order_relaxed) ? 1.0 / 30.0
                : g_modelBusyBackground.load(std::memory_order_relaxed) ? 1.0 / 120.0
                : 0.0;
            const int maxFps = render::ClampMaxFps(render::g_maxFps_game);
            const double capFrameSeconds = maxFps > 0 ? 1.0 / static_cast<double>(maxFps) : 0.0;
            // In the background (another window has the focus) the person's own limit there, Lyra's (§6.2 item 1; a
            // packaged build 30 by default, a dev build none) -- not while auto-detect measures: an idle GPU clocks down
            // and its frame reads slower than it is.
            const int backgroundFps = display::BackgroundFps();
            const bool background = backgroundFps > 0 && !systems_->input.WindowIsForeground() &&
                                    !settings::TheScalability().AutodetectRunning();
            const double backgroundFrameSeconds = background ? 1.0 / static_cast<double>(backgroundFps) : 0.0;
            static bool s_wasBackground = false;
            if (background != s_wasBackground)
            {
                s_wasBackground = background;
                LOG_INFO(logging::LogCategory::Render, "frame pacing: {} the background ({} fps at most there)",
                         background ? "in" : "out of", backgroundFps);
            }
            const double frameSeconds = std::max({ busyFrameSeconds, capFrameSeconds, backgroundFrameSeconds });
            if (frameSeconds > 0.0)
            {
                WaitOutFrameBudget(frameStart, frameSeconds);
            }
            frameStart = GetTimeSeconds();

            // The pacing state as the loop APPLIES it: at boot (after graphics_settings.json) and
            // on every change -- a --set override, a Developer Controls edit. A headless run that
            // times frames reads its cap from here instead of trusting the file.
            static int s_loggedMaxFps = -1;
            static bool s_loggedVsync = false;
            if (maxFps != s_loggedMaxFps || render::g_vsync_game != s_loggedVsync)
            {
                s_loggedMaxFps = maxFps;
                s_loggedVsync = render::g_vsync_game;
                LOG_INFO(logging::LogCategory::Render, "frame pacing: vsync={} maxFps={}{}",
                         render::g_vsync_game ? "on" : "off", maxFps, maxFps == 0 ? " (uncapped)" : "");
            }
        };

        // main read the flag before the settings file set the knob.
        if (!build::Shipping() && std::strstr(GetCommandLineA(), "--no-async-compute")) // a shipped build's switches: main.cpp
        {
            render::g_noAsyncCompute_game = true;
        }
        // --set's threading knobs take effect from the first frame (the rest of --set waits for the first frame, in
        // afterFrame): "--set=frame.renderThread:0" is one thread from the start, not after a threaded first frame.
        for (const auto& kv : Boot().frame.fixedSettings)
        {
            if (kv.first == "frame.renderThread" || kv.first == "frame.oneFrameThreadLag" || kv.first == "frame.rhiThread" ||
                kv.first == "frame.gbvCreationExcludesSubmission")
            {
                ApplySweepValue(scene, appController_.SettingsRef(), kv.first, kv.second);
            }
        }
        bool renderFramePending = false; // kicked, its after-frame work not done (a park may have finished it early)
        double gameWaitLogStart = GetTimeSeconds();
        // "Whole Cycle" is the main thread's frame from its start (after BeginFrame's wait for the GPU) to the moment the
        // frame is drawn. On one thread that is one block: the game half, the hand-over, Scene::Render. With a render
        // thread (G8) it runs from the hand-over, through the next frame's game half beside the render thread, to the
        // wait for it -- across the loop's turn, so it is held here and closed where that wait returns, before afterFrame
        // ends the profiler's frame.
        std::optional<Profiler::ScopedCpu> wholeCycle;
        // G9: frames between reading the input and presenting the frame built from it -- 0 on one thread, 1 with the
        // render thread a frame behind (UE r.OneFrameThreadLag 1), 0 again at frame.oneFrameThreadLag 0 (plan section
        // 5). The game thread counts the input it reads; the hand-over stamps the frame with that count; the frame,
        // presented, compares it with the count now.
        std::atomic<std::uint64_t> inputFrames{0};
        std::uint64_t handedInputFrame = 0; // the game thread's at the hand-over; read by the frame it handed over
        std::atomic<std::uint64_t> lagSum{0};
        std::atomic<std::uint64_t> lagFrames{0};
        std::atomic<std::uint64_t> lagMax{0};
        const auto notePresented = [&]()
        {
            const std::uint64_t lag = inputFrames.load(std::memory_order_acquire) - handedInputFrame;
            lagSum.fetch_add(lag, std::memory_order_relaxed);
            lagFrames.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t seen = lagMax.load(std::memory_order_relaxed);
            while (lag > seen && !lagMax.compare_exchange_weak(seen, lag, std::memory_order_relaxed)) {}
        };
        const auto logInputLag = [&](const char* when)
        {
            const std::uint64_t frames = lagFrames.exchange(0, std::memory_order_relaxed);
            const std::uint64_t sum = lagSum.exchange(0, std::memory_order_relaxed);
            const std::uint64_t worst = lagMax.exchange(0, std::memory_order_relaxed);
            if (frames > 0)
            {
                LOG_INFO(logging::LogCategory::Core, "frame: input to present {:.2f} frames (max {}) over {} frames{}",
                         static_cast<double>(sum) / static_cast<double>(frames), worst, frames, when);
            }
        };
        harness::FrameContext beginFrameContext{ renderer, scene, levelManager, appController_.SettingsRef(), isRunning_ };
        // The frames as one call: run bare, or inside --scene-stress's catch (below).
        const auto runFrames = [&]()
        {
        while (isRunning_) {
            // Switched only here, between frames. --rt-audit (G9) runs with the render thread whatever the settings say.
            const bool threaded = threading::g_renderThreadEnabled || rtaudit::Enabled();
            if (threaded)
            {
                // G8b: the next frame's game half while the render thread draws this one -- the messages, the clock,
                // the UI and the editor, Scene::Tick, a level load. What in it reaches the render half waits for that
                // frame first (threading::WaitRenderForGame; a park, Renderer::WaitForPreviousFrame) or goes through
                // the hand-over (ImGui's textures, GPU work, the knobs' twins, the scene's render copies).
                CPU_SCOPE(ProfilerScopes::kGameSimulate);
                rtaudit::Jitter(); // --rt-audit: a different overlap every frame (nothing otherwise)
                inputFrames.fetch_add(1, std::memory_order_release); // G9: this frame's input is being read
                if (!pumpMessages()) {
                    break;
                }
                input.NewFrame(); // after the pump: this frame takes what came since the last one, the wait's too
                advanceTime();
                beginUi();
                tickUi();
                tickScene();
                rtaudit::GameHalf(scene); // --rt-audit-selftest's write into a frozen copy
                render::RetireSelfTestGameHalf(renderer); // --retire-selftest=upload: a submit beside the render thread's frame
                loadPendingLevel();
                rtaudit::Jitter();
                closeUi();
            }
            if (renderFramePending)
            {
                {
                    CPU_SCOPE(ProfilerScopes::kRenderThreadWait);
                    renderThread.WaitIdle(); // UE FFrameEndSync: never more than one frame ahead; pumps messages
                }
                wholeCycle.reset(); // the frame is drawn
                renderFramePending = false;
                scene.Physics().KickStep(); // every core is idle from here to the hand-over: the wait for the GPU
                afterFrame();
                if (!isRunning_) {
                    break;
                }
                paceFrame();
                if (frameStart - gameWaitLogStart >= 5.0)
                {
                    threading::LogGameWaits(frameStart - gameWaitLogStart); // what took the overlap away, by site
                    renderer.LogGpuIdleWaits(frameStart - gameWaitLogStart); // R16.10: the full idles, by site
                    logInputLag("");
                    gameWaitLogStart = frameStart;
                }
            }

            beginFrame();
            if (sceneStressHarness->Wanted()) { sceneStressHarness->OnBeginFrame(beginFrameContext); } // R3.3e: mid-frame resize requests
            if (threaded)
            {
                wholeCycle.emplace(ProfilerScopes::kWholeCycle); // closed where the wait for this frame returns
                CPU_SCOPE(ProfilerScopes::kGameHandOver);
                renderUpkeep();
                handOver();
                handedInputFrame = inputFrames.load(std::memory_order_relaxed); // G9: the input this frame is built from
            }
            else
            {
                CPU_SCOPE(ProfilerScopes::kWholeCycle);
                inputFrames.fetch_add(1, std::memory_order_release); // G9: this frame's input is being read
                if (!pumpMessages()) {
                    break;
                }
                input.NewFrame(); // after the pump (as with a render thread)
                beginUi();
                advanceTime();
                renderUpkeep();
                tickUi();
                tickScene();
                render::RetireSelfTestGameHalf(renderer); // --retire-selftest=upload (one thread: before the hand-over)
                loadPendingLevel();
                closeUi();
                handOver();
                handedInputFrame = inputFrames.load(std::memory_order_relaxed); // G9: the input this frame is built from
                threading::RoleScope renderRole(threading::ThreadRole::Render);
                renderScene();
                notePresented();
            }
            if (threaded)
            {
                renderThread.Kick([&renderScene, &scene, &notePresented]()
                {
                    CPU_SCOPE(ProfilerScopes::kRenderThreadFrame);
                    rtaudit::BeginRenderFrame(scene); // --rt-audit: what the hand-over froze, hashed (nothing otherwise)
                    rtaudit::Jitter();
                    renderScene();
                    notePresented();
                    rtaudit::Jitter();
                    rtaudit::EndRenderFrame(scene);   // ... and compared: a hash that moved is a game-half write
                });
                renderFramePending = true;
                if (!threading::g_oneFrameThreadLag)
                {
                    // UE's r.OneFrameThreadLag 0: the next frame's game half starts only once this one is drawn.
                    {
                        CPU_SCOPE(ProfilerScopes::kRenderThreadWait);
                        renderThread.WaitIdle();
                    }
                    wholeCycle.reset(); // the frame is drawn
                }
                continue;
            }
            afterFrame();
            paceFrame();
            if (frameStart - gameWaitLogStart >= 5.0)
            {
                renderer.LogGpuIdleWaits(frameStart - gameWaitLogStart); // R16.10: the full idles, by site (one thread)
                gameWaitLogStart = frameStart;
            }
        }
        };
        // A frame that throws ends a --scene-stress with its FAULT verdict and DRED, as its own loop did (R3.3e) -- caught
        // there only. A catch that rethrows unwinds the stack to itself first: every other run's crash report (the
        // unhandled-exception filter's minidump and frames) would show this catch instead of the throw site (the second
        // review of wave 2, item 1; --crash-selftest=throw-frame).
        if (sceneStressHarness->Wanted())
        {
            try {
                runFrames();
            }
            catch (const std::exception& e) {
                sceneStressHarness->OnFrameThrew(e.what()); // the verdict, DRED, the process ended
                throw;
            }
        }
        else
        {
            runFrames();
        }
        wholeCycle.reset();
        renderer.LogGpuIdleWaits(GetTimeSeconds() - gameWaitLogStart); // R16.10: the rest of the run's, and the total
        renderThread.Stop(); // a frame still in flight finishes (its after-frame work is not wanted: the loop is over)
        logInputLag(" (the rest of the run)");
        // The stress harness holds the device's InfoQueue and its DRED file: gone before the GPU teardown, as its driver
        // always was (R3.3e). (The access violation its runs had at exit was not this reference but DLSS's feature released
        // through Streamline's deferred queue after the stress's resize: DlssHandler::ReleaseFeatureIfResized.) Its list's
        // pointer is not used after the loop.
        sceneStressHarness.reset();

        timeEndPeriod(1);
        threading::SetCurrentRole(threading::ThreadRole::Sync); // teardown: nothing else runs
        // The memory trace ends with the main loop: its End record is the steady state, not the teardown.
        memtrace::Stop();
#if WITH_EDITOR
        editormcp::SetRuntimeHost(nullptr);
#endif
        appController_.FlushGraphicsSettings(renderer, scene);
        TaskSystem::Get().Stop();

        renderer.WaitForPreviousFrame();
        appController_.ReleaseGpuResources(renderer); // the UI's textures before the device (the debug layer's exit report)
        memgate::ReleaseSelfTest();
        scene.Clear();
        Systems::DestroyOceanSimulation();
#if PROF_GPU_ENABLED
        Profiler::Get().SetQueueSignalHook(nullptr); // the renderer it routes through goes next (G10)
#endif
        renderer.Shutdown();
    }

    threading::LogRoleCheckSummary(); // the verdict a gate reads (Debug: "thread roles: clean")
    Systems::Shutdown();
    systems_.reset();

    mi_register_output(MiOut, nullptr);
    mi_collect(true);
    // Printed HERE, through the session log, instead of `mi_option_show_stats` at process exit:
    // by then WinMain's log session is already closed and the table would only reach DBWIN
    // (which is exactly where it used to vanish on a headless run). The output callback stays
    // registered so an allocator warning during teardown still lands somewhere.
    mi_stats_print_out(MiOut, nullptr);
}
