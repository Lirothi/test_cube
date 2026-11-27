#include "app/BootTools.h"

// The tools of WinMain that run instead of the game (architecture plan R3.2d): each function is the run of blocks
// main.cpp had at that point, moved verbatim and called where they stood -- the order of what exists by then (the log,
// the crash handler, the content, DPI awareness) is theirs as before. A function answers the exit code of the tool
// the command line names, or -1: the boot goes on.
#include <crtdbg.h>
#include "core/memory/MemScope.h"
#include <windows.h>
#include "core/diagnostics/DiagPaths.h"
#include <mimalloc.h>
#include "physics/PhysicsWorld.h"
#include "app/scene/PlayerGates.h" // R14.3a / R14.6: the world layer's gates (the boats', the player's) beside the physics'
#include "app/App.h"
#include "app/BuildVersion.h"
#include "app/GpuRequirements.h"
#include "app/Shipping.h"
#include "app/BootParse.h"    // R3.2b: ExtractArgValue, the switches' parse
#include "app/BootSwitches.h"
#include "core/settings/SettingsRegistry.h"
#include "app/dist/Cook.h"
#include "app/settings/Scalability.h" // --scalability-selftest
#include "core/diagnostics/CrashHandler.h"
#include "rendering/core/StreamlineSignature.h"
#include "assets/AssetImporter.h"
#include "editor/assets/MaterialFileGen.h" // --seam-ratio (empty outside WITH_EDITOR)
#include "app/diagnostics/CullBenchmark.h"
#include "app/diagnostics/RtAudit.h" // game thread plan G9: --rt-audit
#include "rendering/scene/SceneRenderQueue.h"
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "core/vfs/PakCrypto.h"
#include "materials/Material.h"
#include "core/logging/diagnostics/LogStress.h"
#include "core/memory/diagnostics/MemBench.h"
#include "core/memory/MemTrace.h"
#include "rhi/MemGates.h"
#include "core/task/diagnostics/TaskSystemStress.h"
#include "core/threading/ThreadRole.h" // --thread-check-selftest (R15.5)
#include "core/profiling/ProfilerScopes.h"
#include "ocean/OceanRenderable.h"
#include "rhi/BarrierTranslation.h"
#include "core/diagnostics/BootProfile.h"
#include "rendering/core/GBufferBindingGuard.h"
#include "rendering/core/GbvPassFilter.h"
#include "rhi/Device.h"
#include "rhi/MemoryReport.h"
#include "rendering/diagnostics/RendererSubmissionStress.h"
#include "rendering/diagnostics/GraphSelfTest.h" // --crash-selftest=pass-throw (R15.3), graphcap (R15.4)
#include "rendering/diagnostics/RetireSelfTest.h" // --retire-selftest (R16.1)
#include "core/diagnostics/ThrowSelfTest.h" // --throw-selftest, --crash-selftest=view-throw (the second review of wave 1)
#include "rendering/meshes/MeshManager.h" // W7.1b: g_meshBakeMode (--bake-meshes)
#include "rendering/meshes/LodSwitchReport.h" // --lod-report
#include "rendering/streaming/TextureStreamingIo.h" // --texstream-selftest
#include <fstream>
#include "meshoptimizer.h"                // --reimport-drop-small= -> meshopt_SimplifyPrune
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/rt/RtSmoke.h"
#include "rendering/visibility/HzbCullSelfTest.h" // occlusion plan S2: --hzb-cull-selftest
#include "rendering/lighting/Astronomy.h"          // time-of-day plan T1: --tod-selftest
#include "rendering/lighting/TimeOfDaySettings.h"  // T3: --tod-legacy-light
#include "rendering/shadows/VirtualShadowMap.h" // temporary VSM perf-harness tunables (--vsm-*)
#include "rendering/meshes/LodSelect.h"          // shadow caster LOD curve (--vsm-lodbias/--vsm-lodstride)
#include "rendering/renderables/InstanceTypes.h"  // S0: g_shadowMode / g_csmDebugMode (--shadow-mode, --csm-tint)
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/TessySettings.h" // Tessy N7: --tessy-lod-build
#include "rendering/core/VisibilityStats.h" // occlusion plan S0: --vis-readout
#include "rhi/CommandListBindState.h" // --no-bind-batching
#include "text/TextManager.h"
#include "rendering/wind/WindState.h"                       // W8: g_windFreeze / g_windFrozenTime (--wind-freeze)
#include "rendering/lighting/VolumetricCloud.h"  // render::g_cloudProfilePath (--cloud-profile)
#include <algorithm>
#include <filesystem> // --tessy-upgrade=all walks models/
#include <vector>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include "app/BootTools.h"
#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader

namespace boot
{
int RunEarlyTools(const char* lpCmdLine)
{
        // "--help" (architecture plan R3.2): the switch table (app/BootSwitches.cpp) to the console the program was started
        // from and to the session log, then quit. A Shipping build drops the switch before it gets here, like every switch
        // its table row does not keep.
        if (lpCmdLine && std::strstr(lpCmdLine, "--help")) {
            const std::string text = "test_cube switches ([shipping] = a Shipping build keeps it):\n" + boot::HelpText(false);
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
                DWORD written = 0;
                if (out && out != INVALID_HANDLE_VALUE) {
                    WriteFile(out, "\r\n", 2, &written, nullptr);
                    WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
                }
                FreeConsole();
            }
            logging::WriteRawLines(logging::LogLevel::Info, logging::LogCategory::App, text);
            return 0;
        }

        // --crash-selftest=<arm> (any build; §9): a crash of that kind, its minidump and -- in Shipping -- the message box.
        if (lpCmdLine) {
            if (const std::string arm = ExtractArgValue(lpCmdLine, "--crash-selftest"); arm == "terminate-frame" || arm == "throw-frame") {
                // No early exit: the game starts as always (and installs the handlers as always), and its first frame throws
                // after the renderer's BeginFrame (crash::SelfTestFrame, App.cpp) -- terminate-frame through the renderer's
                // terminate handler, throw-frame to the unhandled-exception filter with the throw site's frames.
                crash::ArmSelfTestInFrame(arm == "terminate-frame");
            } else if (arm == "pass-throw" || arm.rfind("pass-throw@", 0) == 0) {
                // No early exit either: a render pass throws once inside its frame task (R15.3) -- a Fatal naming the pass
                // and the dump; render::PassThrowSelfTestFrame (App.cpp) ends a run that goes on instead.
                if (!render::ArmPassThrowSelfTest(arm.size() > 11 ? std::string_view(arm).substr(11) : std::string_view())) {
                    return 2;
                }
            } else if (arm == "view-throw" || arm == "view-throw@other") {
                // Likewise: a culling task of Scene::PrepareViews throws once (the camera view's, or the first other view's)
                // -- a frame task (R15.3; the second review of wave 1, item 1): a Fatal naming it and the dump.
                // diag::ThrowSelfTestFrame (App.cpp) ends a run that goes on instead.
                if (!diag::ArmThrowSelfTest(arm == "view-throw" ? "view" : "view-other")) {
                    return 2;
                }
            } else if (!arm.empty()) {
                crash::Options options;
                options.dumpFolder = build::Packaged() ? L"crashes" : L"logs/crashes";
                options.messageBox = build::Packaged();
                // Contracts other subsystems own, each broken once (docs/architecture_review_plan.md R15).
                static constexpr crash::SelfTestArm kMoreArms[] = {
                    { "taskdep", &CrashSelfTestTaskDependencyAfterSubmit },
                    { "taskdep-scheduled", &CrashSelfTestTaskDependencyOnScheduled },
                    { "graphcap", &render::CrashSelfTestGraphCap },
                    { "graphgroups", &render::CrashSelfTestGraphGroups },
                    { "graphpasses", &render::CrashSelfTestGraphPasses },
                    { "graphdep", &render::CrashSelfTestGraphDep },
                    { "graphdecl", &render::CrashSelfTestGraphDecl },
                    { "graphcycle", &render::CrashSelfTestGraphCycle },
                };
                return crash::SelfTest(arm, options, kMoreArms);
            }
        }
    return -1;
}

#if TC_PACKAGED
int PreparePackagedContent(const char* lpCmdLine, const std::vector<std::string>& droppedSwitches,
                           const std::filesystem::path& shippingUserFolder)
{
#if TC_SHIPPING
    for (const std::string& dropped : droppedSwitches) {
        LOG_WARNING(logging::LogCategory::App, "a shipped build ignores {}", dropped);
    }
#else
    // Development: the tools that rewrite or walk the loose tree have nothing to work on over packages.
    for (const char* tool : { "--tessy-upgrade", "--lod-report", "--water-mask-lid", "--reimport", "--import", "--uv-density",
                              "--cook", "--shader-cook", "--shader-record", "--pak-build", "--vfs-selftest" }) {
        if (lpCmdLine && std::strstr(lpCmdLine, tool)) {
            LOG_ERROR(logging::LogCategory::App, "{} needs the loose tree (a dev checkout's Release), not a packaged build", tool);
            return 2;
        }
    }
    LOG_INFO(logging::LogCategory::App, "a Development build: packaged content, the developer features on");
#endif
    {
        crash::Options options;
        options.messageBox = true;
        crash::Install(options);
    }
    // The packages beside the exe, and what the player saved over them (§2.1, §6).
    {
        vfs::Config config;
        config.mode = vfs::Config::Mode::Packages;
        config.packageDir = shipping::ExeFolder() / L"content";
        // What the player saved over what shipped: the bots' memory, and the ocean window's presets (Development's F7).
        config.overlays = { { "data/bot_memory/", shippingUserFolder / L"bot_memory" },
                            { "data/ocean/", shippingUserFolder / L"ocean" } };
#if !TC_SHIPPING
        // Development takes a package key besides the one built in (an encrypted build made with another): --pak-key or
        // TC_PAK_KEY, as a dev build does.
        if (const std::string keyFile = ExtractArgValue(lpCmdLine, "--pak-key="); !keyFile.empty()) {
            config.keyFiles.push_back(std::filesystem::path(vfs::Widen(keyFile)));
        } else if (const DWORD n = GetEnvironmentVariableW(L"TC_PAK_KEY", nullptr, 0); n > 1) {
            std::wstring value(n, L'\0');
            value.resize(GetEnvironmentVariableW(L"TC_PAK_KEY", value.data(), n));
            config.keyFiles.push_back(std::filesystem::path(value));
        }
#endif
        std::string error;
        if (!vfs::Configure(config, error)) {
            LOG_ERROR(logging::LogCategory::Vfs, "vfs: the game's content does not open: {}", error);
            shipping::ShowMessage(L"test_cube", L"The game's content is missing or damaged:\n\n" + vfs::Widen(error) +
                                  L"\n\nCopy the whole game folder again (the content folder beside test_cube.exe).");
            return 1;
        }
        for (const std::string& mount : vfs::DescribeMounts()) {
            LOG_INFO(logging::LogCategory::Vfs, "vfs: mounted {}", mount);
        }
    }
    // The D3D12 runtime the build asks for (AgilitySdk.cpp: 619, from D3D12\ beside the exe). Without the file no device is
    // made where the system's own runtime is older -- every Windows 10, and Windows 11 26100 too (measured): the player is
    // told the file is missing, not that the card cannot.
    {
        std::error_code ec;
        const std::filesystem::path core = shipping::ExeFolder() / L"D3D12" / L"D3D12Core.dll";
        if (!std::filesystem::is_regular_file(core, ec)) { // vfs-ok: a runtime DLL beside the exe, not content
            LOG_ERROR(logging::LogCategory::Render, "d3d12: {} is missing", vfs::Narrow(core.wstring()));
            shipping::ShowMessage(L"test_cube", L"A file of the game is missing:\n\n" + core.wstring() +
                                  L"\n\nCopy the whole game folder again.");
            return 1;
        }
    }
    // The card, before a window exists (§7).
    {
        const gpu::RequirementCheck check = gpu::CheckRequirements();
        LOG_INFO(logging::LogCategory::Render, "gpu check: {} -- {} (driver {})", check.ok ? "PASS" : "FAIL", check.adapter, check.driver);
        if (!check.ok) {
            const std::wstring message = gpu::RequirementMessage(check, L"test_cube");
            LOG_ERROR(logging::LogCategory::Render, "gpu check: {}", vfs::Narrow(message));
            shipping::ShowMessage(L"test_cube", message);
            return 1;
        }
    }
    // NVIDIA's signature on the Streamline interposer, before it is loaded (delay-loaded in Shipping): a file that is not
    // NVIDIA's is never loaded -- the game runs without DLSS.
    {
        std::string why;
        if (!render::VerifyStreamlineSignature(shipping::ExeFolder() / L"sl.interposer.dll", why)) {
            LOG_WARNING(logging::LogCategory::Render, "streamline: sl.interposer.dll {} -- not loaded, no DLSS", why);
            render::g_noStreamline_game = true;
        } else {
            LOG_INFO(logging::LogCategory::Render, "streamline: sl.interposer.dll is signed by NVIDIA");
        }
    }
    (void)droppedSwitches;
    (void)shippingUserFolder;
    return -1;
}

int ReadPackagedBootLevel()
{
    // What the build starts (config/boot.json, from the profile): its level, and Play from the level's Player Start.
    {
        // Without it, or without its level, the content is not whole (a package missing, a damaged one): the player is told,
        // where a missing level used to end in a Fatal and exit code 3 with nothing on screen (the clean-place gate's arm
        // "base.tcpak gone, shaders.tcpak there").
        shipping::BootConfig boot;
        std::string error;
        if (!shipping::ReadBootConfig(boot, error)) {
            LOG_ERROR(logging::LogCategory::App, "boot: {}", error);
            shipping::ShowMessage(L"test_cube", L"The game's content is missing or damaged:\n\n" + vfs::Widen(error) +
                                  L"\n\nCopy the whole game folder again (the content folder beside test_cube.exe).");
            return 1;
        }
        if (Boot().frame.levelPath.empty()) { Boot().frame.levelPath = boot.bootLevel; }
        if (!vfs::IsFile(Boot().frame.levelPath)) {
            LOG_ERROR(logging::LogCategory::App, "boot: the level {} is not in the game's content", Boot().frame.levelPath);
            shipping::ShowMessage(L"test_cube", L"The level " + vfs::Widen(Boot().frame.levelPath) +
                                  L" is not in the game's content.\n\nCopy the whole game folder again.");
            return 1;
        }
        if (boot.startMode == "play")
        {
            Boot().editor.play = true;
            Boot().dist.playByLevelRule = true; // a level without a Player Start: the free camera (AppController)
        }
        LOG_INFO(logging::LogCategory::App, "boot: {} {} -- {} ({})", boot.product, boot.version, Boot().frame.levelPath, boot.startMode);
    }
    return -1;
}
#else
int RunContentTools(const char* lpCmdLine)
{
    // The file system the content comes through (docs/distribution_build_plan.md §2.3): --vfs=loose|packages|both,
    // --pak-dir and --vfs-record mount before anything reads; --pak-build / --pak-list / --pak-verify / --vfs-selftest
    // run here, no window, the exit code their failures.
    if (lpCmdLine) {
        if (const int vfsExit = vfs::RunCommandLine(lpCmdLine); vfsExit >= 0) {
            return vfsExit;
        }
        // Shaders for a distribution (plan §5): --shader-model=6.6 caps every compile's target (a mounted shaders.tcpak
        // sets its own), --shader-record=<file> lists every compile request, --shader-cook=<list>,<out.tcpak> compiles a
        // list into a shaders package (no window; --pak-level, --pak-encrypt --pak-key=<key.json> as for --pak-build).
        if (const std::string model = ExtractArgValue(lpCmdLine, "--shader-model"); !model.empty()) {
            const size_t dot = model.find_first_of("._");
            const unsigned major = static_cast<unsigned>(std::atoi(model.c_str()));
            const unsigned minor = dot == std::string::npos ? 0u : static_cast<unsigned>(std::atoi(model.c_str() + dot + 1));
            Material::SetShaderModelCap((major << 4) | (minor & 0xFu));
        }
        if (const std::string record = ExtractArgValue(lpCmdLine, "--shader-record"); !record.empty()) {
            Material::SetShaderRecordFile(vfs::Widen(record));
        }
        if (const std::string cook = ExtractArgValue(lpCmdLine, "--shader-cook"); !cook.empty()) {
            const size_t comma = cook.find(',');
            if (comma == std::string::npos) {
                LOG_ERROR(logging::LogCategory::Render, "--shader-cook=<list>,<out.tcpak>");
                return 1;
            }
            std::optional<vfs::pak::Key> key;
            if (std::strstr(lpCmdLine, "--pak-encrypt")) {
                std::string why;
                const std::string keyFile = ExtractArgValue(lpCmdLine, "--pak-key");
                key = keyFile.empty() ? std::nullopt : vfs::pak::LoadKeyFile(vfs::Widen(keyFile), why);
                if (!key) {
                    LOG_ERROR(logging::LogCategory::Render, "--shader-cook --pak-encrypt needs --pak-key=<key.json> ({})", why);
                    return 1;
                }
            }
            const std::string level = ExtractArgValue(lpCmdLine, "--pak-level");
            return Material::CookShaders(vfs::Widen(cook.substr(0, comma)), vfs::Widen(cook.substr(comma + 1)),
                                         key ? &*key : nullptr, level.empty() ? 19 : std::atoi(level.c_str()));
        }
        // The content cook (plan §8.1): --cook=<profile.json> --cook-out=<folder> [--cook-check] -- no window.
        if (const int cookExit = dist::RunCook(lpCmdLine); cookExit >= 0) {
            return cookExit;
        }
    }
    return -1;
}
#endif

int RunHarnessTools(const char* lpCmdLine)
{
        // "--thread-check-selftest" (docs/architecture_review_plan.md R15.5): one thread-role violation planted on purpose -- a
        // render-half check made from the game role. Exit 0 when this build's checks are compiled in (Debug, or
        // /p:TcThreadChecks=true) and reported it: a "thread role:" Error and "thread roles: 1 violations"; exit 1 when they
        // are compiled out (Release_Editor without the switch -- the switch's control arm) or missed it.
        if (lpCmdLine && std::strstr(lpCmdLine, "--thread-check-selftest") != nullptr) {
            threading::RoleScope gameRole(threading::ThreadRole::Game);
            TC_CHECK_RENDER("--thread-check-selftest: a render-half call planted on the game role");
            threading::LogRoleCheckSummary();
            const bool reported = threading::RoleViolationCount() == 1u;
            LOG_INFO(logging::LogCategory::Core, "thread-check self-test: checks {}, the planted violation {}",
                     TC_THREAD_CHECKS_ON ? "compiled in" : "compiled out", reported ? "reported" : "NOT reported");
            return reported ? 0 : 1;
        }

        // "--tasksystem-stress" runs the task-system stress harness instead of the
        // app; exit code is the number of failed checks. With "--stress-overflow"
        // the process is expected to end through the dependents_ fan-out invariant (exit 3).
        if (lpCmdLine && std::strstr(lpCmdLine, "--tasksystem-stress") != nullptr) {
            const bool overflow = std::strstr(lpCmdLine, "--stress-overflow") != nullptr;
            return RunTaskSystemStress(overflow);
        }

        // "--physics-gate[=<name>]": the physics gates (docs/physics_plan.md section 5), no window; exit code = the failed ones.
        if (lpCmdLine) {
            if (const char* gate = std::strstr(lpCmdLine, "--physics-gate")) {
                std::string name;
                if (gate[std::strlen("--physics-gate")] == '=') {
                    const char* value = gate + std::strlen("--physics-gate=");
                    const char* end = std::strchr(value, ' ');
                    name = end ? std::string(value, end) : std::string(value);
                }
                return physics::RunGates(name, SceneGates());
            }
        }

        // "--mem-bench" times the counted operator new/delete against raw mimalloc (memory profiling
        // plan M0); verdict lines in the session log.
        if (lpCmdLine && std::strstr(lpCmdLine, "--mem-bench") != nullptr) {
            return RunMemBench(lpCmdLine);
        }

        // "--renderer-submission-stress" runs the CPU-only submission-timeline
        // stress harness instead of the app; exit code is the number of failed
        // checks. Its death-test sub-flags (--stress-*) intentionally abort and
        // are spawned by the harness itself in child processes.
        if (lpCmdLine && std::strstr(lpCmdLine, "--renderer-submission-stress") != nullptr) {
            return RunRendererSubmissionStress(lpCmdLine);
        }

        if (lpCmdLine && std::strstr(lpCmdLine, "--textmanager-benchmark") != nullptr) {
            return RunTextManagerBenchmark(diag::LogPath("textmanager_benchmark.csv").c_str());
        }

        // "--cull-benchmark" times the per-object frustum intersect (legacy DirectXMath vs the
        // precomputed-plane path) over a fixed AABB set; results in cull_benchmark.txt.
        if (lpCmdLine && std::strstr(lpCmdLine, "--cull-benchmark") != nullptr) {
            return RunCullBenchmark(diag::LogPath("cull_benchmark.txt").c_str());
        }

        // "--cull-nofuse" forces the split Bucketize()+Cull() path for self-culling views, for an
        // in-engine A/B against the default fused BucketizeCull. Benchmark-only.
        if (lpCmdLine && std::strstr(lpCmdLine, "--cull-nofuse") != nullptr) {
            g_useFusedBucketizeCull = false;
        }

        // "--rt-smoke" runs the headless DXR smoke harness (device + RT caps + a
        // trivial BLAS/TLAS build) instead of the app; verdict in rt_smoke.txt,
        // exit code 0 on PASS/SKIP, non-zero on FAIL.
        if (lpCmdLine && std::strstr(lpCmdLine, "--rt-smoke") != nullptr) {
            return RunRtSmoke(diag::LogPath("rt_smoke.txt").c_str());
        }

        // "--hzb-cull-selftest" (occlusion plan S2) runs the headless box -> HZB visibility self-test
        // instead of the app: a synthetic pyramid, hand-set verdicts, the GPU held equal to the CPU
        // mirror. Per-case lines and the verdict go to the session log; exit code = failed checks.
        if (lpCmdLine && std::strstr(lpCmdLine, "--hzb-cull-selftest") != nullptr) {
            return RunHzbCullSelfTest();
        }

        // "--tod-selftest" (time-of-day plan T1): the sun, the moon, the star frame and the simple paths
        // against reference values. Per-case lines and the verdict go to the session log; exit code =
        // failed checks. No renderer.
        if (lpCmdLine && std::strstr(lpCmdLine, "--tod-selftest") != nullptr) {
            return astro::RunSelfTest();
        }

        // "--scalability-selftest" (docs/scalability_plan.md §8.1): the graphics levels' table and its control arms, no
        // window. Verdict in the session log; exit code = failed checks.
        if (lpCmdLine && std::strstr(lpCmdLine, "--scalability-selftest") != nullptr) {
            return settings::RunScalabilitySelfTest();
        }

        // "--rt-force-as-fail" (S13 test hook): make every acceleration-structure
        // allocation fail, so the graceful RT-disable → SSR-fallback path can be
        // exercised in the live app without actually exhausting VRAM.
        if (lpCmdLine && std::strstr(lpCmdLine, "--rt-force-as-fail") != nullptr) {
            rt::AccelerationStructureManager::SetForceAllocFailureForTest(true);
        }

        // "--import <staging-dir> [--skybox <file.hdr>]" runs the offline asset conversion backend
        // (Part H1) headless — PNG/JPG -> mipped BC7 DDS, texture-set MR synthesis, flipbook atlases,
        // .hdr -> BC6H cubemap — then exits. Verdict in asset_import.log; exit code = failed count.
        if (lpCmdLine && std::strstr(lpCmdLine, "--import") != nullptr) {
            assets::ImportOptions opts;
            opts.stagingDir = ExtractArgValue(lpCmdLine, "--import");
            opts.skyboxHdr  = ExtractArgValue(lpCmdLine, "--skybox");
            if (const char* m = std::strstr(lpCmdLine, "--max-size=")) {
                opts.maxTextureSize = std::atoi(m + std::strlen("--max-size="));
            }
            if (const char* m = std::strstr(lpCmdLine, "--sky-face=")) {
                opts.skyboxFaceSize = std::atoi(m + std::strlen("--sky-face="));
            }
            // "--sky-target=<median luminance>": sky calibration target, 0 to keep the source's own
            // radiance. See ImportOptions::skyTargetMedianLuma.
            if (const char* m = std::strstr(lpCmdLine, "--sky-target=")) {
                opts.skyTargetMedianLuma = (float)std::atof(m + std::strlen("--sky-target="));
            }
            // P16.3: "--sky-keep-sun" leaves the sun disc in the LIGHTING derivatives (the pre-P16.3
            // behaviour, for a sky used with no directional light of its own); "--sky-sun-radius=<deg>"
            // sets the cut. See ImportOptions::skyRemoveSunFromIbl.
            opts.skyRemoveSunFromIbl = std::strstr(lpCmdLine, "--sky-keep-sun") == nullptr;
            if (const char* m = std::strstr(lpCmdLine, "--sky-sun-radius=")) {
                opts.skySunRadiusDeg = (float)std::atof(m + std::strlen("--sky-sun-radius="));
            }
            // "--sky-out=<textures root>": move the finished cube, its two IBL siblings and the shared
            // brdf_lut to where the engine loads them from, instead of leaving them in the staging
            // folder. Same destinations the GUI import panel uses.
            opts.skyOutputRoot = ExtractArgValue(lpCmdLine, "--sky-out");
            opts.highQuality = std::strstr(lpCmdLine, "--high") != nullptr; // opt-in exhaustive BC7
            opts.flipGreen   = std::strstr(lpCmdLine, "--flip-green") != nullptr;
            opts.bc5Normal   = std::strstr(lpCmdLine, "--bc5-normal") != nullptr;
            opts.centerNormals = std::strstr(lpCmdLine, "--center-normals") != nullptr;
            opts.useGpu      = std::strstr(lpCmdLine, "--cpu") == nullptr; // H5: GPU BC encode default-on
            return assets::RunImport(opts);
        }
    return -1;
}

int RunAssetTools(const char* lpCmdLine)
{
    if (!lpCmdLine) {
        return -1;
    }
    // "--uv-density=models/x/x.mesh.json[,models/y/y.mesh.json...]" (texture streaming A1): measure
    // the per-slot UV density of each manifest's geometry as it is on disk (the baked .bin) and
    // write it as "uvDensity" -- nothing else in the file and nothing in the .bin changes. For
    // manifests that predate the key; a bake writes it on its own. Exit code = failures.
    if (const char* flag = std::strstr(lpCmdLine, "--uv-density=")) {
        const char* p = flag + std::strlen("--uv-density=");
        std::string list;
        while (*p && !std::isspace(static_cast<unsigned char>(*p))) { list.push_back(*p); ++p; }
        int failed = 0;
        for (size_t i = 0; i < list.size();) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) { j = list.size(); }
            const std::string path = list.substr(i, j - i);
            i = j + 1;
            if (path.empty()) { continue; }
            const std::vector<float> density = MeshManager::ComputeUvDensitiesForManifest(path);
            if (density.empty() || !MeshManager::WriteManifestUvDensity(path, density)) {
                LOG_ERROR(logging::LogCategory::Asset, "--uv-density: FAILED for {}", path);
                ++failed;
                continue;
            }
            std::string values;
            for (float d : density) { values += (values.empty() ? "" : " ") + std::to_string(d); }
            LOG_INFO(logging::LogCategory::Asset, "--uv-density: {} -> uvDensity [{}]", path, values);
        }
        return failed;
    }
#if WITH_EDITOR
    // "--seam-ratio=<geometry>[,<geometry>...]": how OPEN each submesh's surface is, measured
    // exactly as the importer measures it, next to that slot's glTF flags, the twoSided the
    // importer would write (materialgen::DecideTwoSided) and its leaf-card verdict with the
    // share of the surface the cutout removes (materialgen::ClassifyLeafCards). Read-only: one
    // line per submesh, nothing written. Exit code = geometries that could not be read.
    if (const char* flag = std::strstr(lpCmdLine, "--seam-ratio=")) {
        const char* p = flag + std::strlen("--seam-ratio=");
        std::string list;
        while (*p && !std::isspace(static_cast<unsigned char>(*p))) { list.push_back(*p); ++p; }
        int failed = 0;
        for (size_t i = 0; i < list.size();) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) { j = list.size(); }
            const std::string path = list.substr(i, j - i);
            i = j + 1;
            if (path.empty()) { continue; }
            const std::vector<float> ratios = materialgen::MeasureSeamRatios(path);
            if (ratios.empty()) {
                LOG_ERROR(logging::LogCategory::Asset, "--seam-ratio: could not read {}", path);
                ++failed;
                continue;
            }
            // The leaf-card verdict the importer reaches for each slot, from the same call.
            const std::vector<materialgen::LeafVerdict> leaves = materialgen::ClassifyLeafCards(path);
            for (size_t s = 0; s < ratios.size(); ++s) {
                const GltfMaterialDesc material =
                    MeshManager::DescribeGltfMaterial(path, static_cast<int>(s));
                if (!material.valid) {
                    LOG_INFO(logging::LogCategory::Asset,
                        "--seam-ratio: {} submesh {}: {:.5f}, no glTF material (slot stays auto)",
                        path, s, ratios[s]);
                    continue;
                }
                const materialgen::TwoSidedDecision decision = materialgen::DecideTwoSided(
                    material.doubleSided, material.alphaMask, true, ratios[s]);
                LOG_INFO(logging::LogCategory::Asset,
                    "--seam-ratio: {} submesh {} '{}': {:.5f}, glTF doubleSided={}{} -> {}; {}",
                    path, s, material.name, ratios[s], material.doubleSided ? 1 : 0,
                    material.alphaMask ? " MASK" : (material.alphaBlend ? " BLEND" : ""), decision.reason,
                    s < leaves.size() ? leaves[s].reason : std::string("leaf verdict missing"));
            }
        }
        return failed;
    }
#endif
    // Tessy N1 (docs/tessy_plan.md): "--tessy-upgrade=<a.mesh.bin>,<b.mesh.bin>" rewrites each older
    // .mesh.bin IN PLACE as the current version -- the same vertices, the triangles of every LOD in
    // cluster order, the cluster tables added, and (v5, N7e) the cluster DAG with the manifest's weights
    // (MeshManager::UpgradeBinaryToClusters). "--tessy-upgrade=all" takes every models/**/*.mesh.bin.
    // Headless, CPU only. Exit code = the files that failed. Writing into models/ is the owner's call: back
    // the files up first.
    if (const char* up = std::strstr(lpCmdLine, "--tessy-upgrade=")) {
        std::string list;
        for (const char* p = up + std::strlen("--tessy-upgrade="); *p && !std::isspace(static_cast<unsigned char>(*p)); ++p) {
            list.push_back(*p);
        }
        std::vector<std::string> paths;
        if (list == "all") {
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator("models", ec); // vfs-ok: --tessy-upgrade rewrites the loose models/
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) { // vfs-ok: the same walk
                const std::string p = it->path().generic_string();
                if (it->is_regular_file(ec) && p.size() > 9 && p.compare(p.size() - 9, 9, ".mesh.bin") == 0) { paths.push_back(p); } // vfs-ok: the same walk
            }
            std::sort(paths.begin(), paths.end());
        } else {
            for (size_t i = 0; i < list.size();) {
                size_t j = list.find(',', i);
                if (j == std::string::npos) { j = list.size(); }
                if (j > i) { paths.push_back(list.substr(i, j - i)); }
                i = j + 1;
            }
        }
        int failed = 0;
        for (const std::string& path : paths) {
            std::string report;
            const bool ok = MeshManager::UpgradeBinaryToClusters(path, &report);
            if (ok) { LOG_INFO(logging::LogCategory::Asset, "--tessy-upgrade: {}: {}", path, report); }
            else { LOG_ERROR(logging::LogCategory::Asset, "--tessy-upgrade: {}: {}", path, report); ++failed; }
        }
        LOG_INFO(logging::LogCategory::Asset, "--tessy-upgrade: {} file(s), {} failed", paths.size(), failed);
        return failed;
    }
    // "--lod-report[=<part of a path>]": the Mesh Editor's "LOD switching" for every models/**/*.mesh.json (or those
    // whose path holds the text) -- where each LOD comes in on UE's reference view and every warning with its fix
    // (rendering/meshes/LodSwitchReport.h), at the compiled defaults of lod.* (graphics_settings.json is not read).
    // Headless, read-only. Exit code = the assets with a warning + those whose geometry cannot be read (an Error each).
    // "--water-mask-lid=<geometry .mesh.bin>[,<out.obj>[,<glass slot;slot...>]]": a hull's water mask (ocean/WaterMask.h)
    // built as the frame builds it the first time it meets the hull -- its basins and their lids in the log -- and its lid
    // written as an OBJ to look at (per basin a group of the cap and one of the curtains and glass faces). The glass slots
    // are what the object would report (its thinTranslucent slots with no alpha test); none given = none. Headless,
    // read-only. Exit code 0 = a lid, 1 = none, 2 = the geometry could not be read.
    if (const char* wm = std::strstr(lpCmdLine, "--water-mask-lid=")) {
        std::string arg;
        for (const char* p = wm + std::strlen("--water-mask-lid="); *p && !std::isspace(static_cast<unsigned char>(*p)); ++p) {
            arg.push_back(*p);
        }
        std::string out;
        std::vector<int> glass;
        if (const size_t comma = arg.find(','); comma != std::string::npos) {
            out = arg.substr(comma + 1);
            arg.resize(comma);
        }
        if (const size_t comma = out.find(','); comma != std::string::npos) {
            std::string list = out.substr(comma + 1);
            out.resize(comma);
            for (size_t at = 0; at < list.size();) {
                const size_t end = std::min(list.find(';', at), list.size());
                if (end > at) { glass.push_back(std::atoi(list.substr(at, end - at).c_str())); }
                at = end + 1;
            }
        }
        // Where still water stands on it, as the object knows: the manifest beside the geometry, its vessel's draft.
        float waterline = 3.0e38f;
        if (const size_t bin = arg.rfind(".mesh.bin"); bin != std::string::npos) {
            std::ifstream manifest(arg.substr(0, bin) + ".mesh.json"); // vfs-ok: --water-mask-lid, a tool on the loose asset
            const nlohmann::json doc = nlohmann::json::parse(manifest, nullptr, false, true);
            if (!doc.is_discarded() && doc.contains("vessel") && doc["vessel"].is_object()) {
                waterline = jsonfield::Value(doc["vessel"], "draft", 0.38f);
            }
        }
        const std::shared_ptr<const ocean::WaterMaskField> field = ocean::watermask::BuildField(arg, glass, waterline);
        if (!field) { return 2; }
        // The bake's round trip (ocean/WaterMask.h, THE BAKE): what a frame would read back from the file is this field,
        // byte for byte, and a key that differs is refused.
        {
            ocean::WaterMaskBakeKey key;
            std::string why;
            const bool keyed = ocean::watermask::MakeBakeKey(arg, glass, waterline, -3.0e38f, key);
            const std::vector<std::uint8_t> bytes = ocean::watermask::SerializeField(*field, key);
            const std::shared_ptr<ocean::WaterMaskField> back = ocean::watermask::DeserializeField(bytes.data(), bytes.size(), &key, why);
            ocean::WaterMaskBakeKey other = key;
            other.waterline += 0.01f;
            std::string otherWhy;
            const bool refused = !ocean::watermask::DeserializeField(bytes.data(), bytes.size(), &other, otherWhy);
            if (keyed && back && ocean::watermask::SameField(*field, *back) && refused) {
                LOG_INFO(logging::LogCategory::Ocean, "--water-mask-lid: the bake round-trips ({} KB, identical), another draft refused ({})",
                         bytes.size() / 1024u, otherWhy);
            } else {
                LOG_ERROR(logging::LogCategory::Ocean, "--water-mask-lid: the bake does NOT round-trip (keyed {}, read back {}, identical {}, other refused {}): {}",
                          keyed, back != nullptr, back && ocean::watermask::SameField(*field, *back), refused, why);
                return 3;
            }
        }
        if (!out.empty()) {
            std::ofstream obj(out);
            for (const Math::float3& v : field->lidVertices) { obj << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n'; }
            for (size_t b = 0; b < field->basins.size(); ++b) {
                const ocean::WaterMaskBasin& basin = field->basins[b];
                const size_t capEnd = static_cast<size_t>(basin.firstIndex) + basin.capIndexCount;
                const size_t end = b + 1 < field->basins.size() ? field->basins[b + 1].firstIndex : field->lidIndices.size();
                const auto faces = [&](size_t from, size_t to) {
                    for (size_t t = from; t + 2 < to; t += 3) {
                        obj << "f " << field->lidIndices[t] + 1 << ' ' << field->lidIndices[t + 1] + 1 << ' ' << field->lidIndices[t + 2] + 1 << '\n';
                    }
                };
                obj << "g cap" << b << '\n';
                faces(basin.firstIndex, capEnd);
                obj << "g curtains" << b << '\n';
                faces(capEnd, end);
            }
            LOG_INFO(logging::LogCategory::Ocean, "--water-mask-lid: wrote {}", out);
        }
        return field->lidIndices.empty() ? 1 : 0;
    }
    if (const char* rep = std::strstr(lpCmdLine, "--lod-report")) {
        std::string filter;
        if (rep[std::strlen("--lod-report")] == '=') {
            for (const char* p = rep + std::strlen("--lod-report="); *p && !std::isspace(static_cast<unsigned char>(*p)); ++p) {
                filter.push_back(*p);
            }
        }
        std::vector<std::string> manifests;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator("models", ec); // vfs-ok: --lod-report scans the loose models/
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) { // vfs-ok: the same walk
            const std::string p = it->path().generic_string();
            if (it->is_regular_file(ec) && p.size() > 10 && p.compare(p.size() - 10, 10, ".mesh.json") == 0 && // vfs-ok: the same walk
                (filter.empty() || p.find(filter) != std::string::npos)) { manifests.push_back(p); }
        }
        std::sort(manifests.begin(), manifests.end());
        int withWarnings = 0;
        int unreadable = 0;
        size_t warnings = 0;
        for (const std::string& path : manifests) {
            std::ifstream in(path); // vfs-ok: --lod-report, a tool on the loose assets
            const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false, true);
            if (doc.is_discarded() || !doc.is_object()) { continue; }
            std::string geometry = jsonfield::Value(doc, "geometry", std::string());
            if (const size_t hash = geometry.find('#'); hash != std::string::npos) { geometry.resize(hash); }
            if (geometry.empty()) { continue; }
            MeshManager::BinaryInfo info;
            std::string why;
            if (!MeshManager::DescribeMeshBinary(geometry, info, &why)) {
                // Skipped in silence until 2026-11-24 (docs/architecture_review_plan.md M5): a damaged or missing
                // geometry read as an asset with nothing to report.
                LOG_ERROR(logging::LogCategory::Asset, "--lod-report: {}: its geometry {} cannot be read: {}", path, geometry, why);
                ++unreadable;
                continue;
            }
            const lodreport::Report r = lodreport::Build(info, doc, lodreport::ManifestHasFoliage(doc));
            std::string levels;
            for (size_t i = 0; i < r.rows.size(); ++i) {
                char part[96];
                std::snprintf(part, sizeof(part), "%sLOD%zu %u tris dev %.4f from %s%s", i ? " | " : "", i, r.rows[i].tris,
                              static_cast<double>(r.rows[i].deviation), lodreport::Metres(r.rows[i].distance).c_str(),
                              r.rows[i].skipped ? " (skipped)" : "");
                levels += part;
            }
            LOG_INFO(logging::LogCategory::Asset, "--lod-report: {}: PixelError {:.1f}: {}", path, r.pixelError, levels);
            for (const lodreport::Warning& w : r.warnings) {
                LOG_WARNING(logging::LogCategory::Asset, "--lod-report: {}: {} Fix: {}", path, w.text, w.fix);
            }
            warnings += r.warnings.size();
            withWarnings += r.warnings.empty() ? 0 : 1;
        }
        LOG_INFO(logging::LogCategory::Asset, "--lod-report: {} asset(s), {} with warnings, {} warning(s){}", manifests.size(),
                 withWarnings, warnings, unreadable == 0 ? std::string() : ", " + std::to_string(unreadable) + " UNREADABLE");
        return withWarnings + unreadable;
    }
    // "--reimport --reimport-src=<glTF> --reimport-out=<.mesh.bin>": headless CPU-only bake
    // (no device/window). Reads a staging glTF, regenerates normals/tangents + LODs, writes our
    // binary geometry that mesh.json's "geometry" references. The explicit content reimport step.
    if (std::strstr(lpCmdLine, "--reimport-src=")) {
        const auto getArg = [lpCmdLine](const char* key) -> std::string {
            const char* flag = std::strstr(lpCmdLine, key);
            if (!flag) { return {}; }
            const char* p = flag + std::strlen(key);
            std::string v;
            // A quoted value may hold spaces: a split import's node selector does
            // ("scene.gltf#node:Floor tile", the_bathroom_free).
            if (*p == '"')
            {
                for (++p; *p && *p != '"'; ++p) { v.push_back(*p); }
                return v;
            }
            while (*p && !std::isspace(static_cast<unsigned char>(*p))) { v.push_back(*p); ++p; }
            return v;
        };
        const std::string src = getArg("--reimport-src=");
        const std::string out = getArg("--reimport-out=");
        if (src.empty() || out.empty()) { return 2; }
        MeshLoadOptions opt{};
        // Match the runtime mesh load exactly (StaticMesh/TransparentStaticMesh): wantCW=false keeps
        // the file's own order, and ParseGltfFile's right- to left-handed flip (det -1) turns it for the
        // FrontCounterClockwise=FALSE PSOs. Baking with the default wantCW=true flips the triangles a
        // second time -> the .bin renders backfaces.
        opt.wantCW = false;
        // "--reimport-recompute=2,3,4" mirrors the mesh.json recomputeNormalSlots so the baked
        // normals/tangents match what the runtime glTF path produced (two-sided foliage fix).
        const std::string recompute = getArg("--reimport-recompute=");
        for (size_t i = 0; i < recompute.size();) {
            size_t j = recompute.find(',', i);
            if (j == std::string::npos) { j = recompute.size(); }
            if (j > i) { opt.recomputeNormalSlots.push_back(static_cast<uint32_t>(std::atoi(recompute.substr(i, j - i).c_str()))); }
            i = j + 1;
        }
        // "--reimport-foliage=0,0,1,0,0" mirrors mesh.json "windFoliage" so the bake knows which
        // slots are wood. Without it the along-limb weight falls back to a per-component ramp,
        // which steps at every junction the modeller happened to cut a leaf at.
        const std::string foliage = getArg("--reimport-foliage=");
        for (size_t i = 0; i < foliage.size();) {
            size_t j = foliage.find(',', i);
            if (j == std::string::npos) { j = foliage.size(); }
            opt.slotFoliage.push_back(static_cast<float>(std::atof(foliage.substr(i, j - i).c_str())));
            i = j + 1;
        }
        // "--reimport-chunk=6" mirrors the mesh.json "chunkGrid": the LOD0 triangles are split
        // into a 6x6 grid of submeshes so a shadow page can rasterize only the chunks it
        // overlaps. MUST match what mesh.json carries at runtime — the runtime reads the grid
        // from mesh.json to decide the submeshes are independent casters, and a .bin baked
        // unchunked would then hand it one giant "chunk" with the object's own bounds.
        const std::string chunk = getArg("--reimport-chunk=");
        if (!chunk.empty()) {
            opt.chunkGrid = static_cast<unsigned int>(std::atoi(chunk.c_str()));
        }
        // "--reimport-manifest=models/x/x.mesh.json": take EVERY bake option from the manifest.
        // Applied before the individual flags below so an explicit flag still wins, which is
        // what makes "re-bake exactly what this asset says, but with one thing changed" a
        // one-liner. Without it a headless re-bake silently dropped every manifest key that had
        // no flag of its own -- lod3Aggressive, lodRatioScale, the per-level drop slots -- and
        // handed back geometry that disagreed with the manifest it was baked from.
        const std::string manifest = getArg("--reimport-manifest=");
        if (!manifest.empty()) {
            if (!MeshManager::ApplyManifestOptions(manifest, opt)) {
                LOG_ERROR(logging::LogCategory::Asset, "--reimport-manifest: could not read {}", manifest);
                return 3;
            }
        }
        // "--reimport-normal-weight=0.75" mirrors mesh.json "lodNormalWeight": the LOD
        // simplifier weights the NORMAL alongside position, so collapses stop breaking the
        // shading of a surface whose vertex normals it cannot rewrite (LODs share one vertex
        // buffer). 0 / absent = the position-only metric, byte-identical to the old bake.
        const std::string normalWeight = getArg("--reimport-normal-weight=");
        if (!normalWeight.empty()) {
            opt.lodNormalWeight = static_cast<float>(std::atof(normalWeight.c_str()));
        }
        // "--reimport-scale=0.0107" mirrors the import dialog's unit-fix bakeScale (see
        // MeshLoadOptions::bakeScale) — without it a centimetre-authored asset (the tent)
        // bakes 100x too large. The GUI import was the only way to pass this before.
        const std::string bakeScale = getArg("--reimport-scale=");
        if (!bakeScale.empty()) {
            opt.bakeScale = static_cast<float>(std::atof(bakeScale.c_str()));
        }
        // "--reimport-prune=0.35" — LOD3 foliage prune keep fraction (0 = off, plain meshopt).
        // See MeshLoadOptions::foliagePruneKeep; only acts on slots the --reimport-foliage
        // list marks as leaves.
        const std::string prune = getArg("--reimport-prune=");
        if (!prune.empty()) {
            opt.foliagePruneKeep = static_cast<float>(std::atof(prune.c_str()));
        }
        // "--reimport-lod3=0" — opt out of the harsh LOD3 solid budget (mirrors mesh.json
        // "lod3Aggressive"; the terrain manifests carry false and a CLI re-bake of them
        // must pass this or the dunes' far shadow casters coarsen).
        const std::string lod3 = getArg("--reimport-lod3=");
        if (!lod3.empty()) {
            opt.lod3Aggressive = std::atoi(lod3.c_str()) != 0;
        }
        // LOD3's own budget multipliers (mesh.json "lod3RatioScale"/"lod3ErrorScale").
        const std::string lod3r = getArg("--reimport-lod3-ratio=");
        if (!lod3r.empty()) { opt.lod3RatioScale = static_cast<float>(std::atof(lod3r.c_str())); }
        const std::string lod3e = getArg("--reimport-lod3-error=");
        if (!lod3e.empty()) { opt.lod3ErrorScale = static_cast<float>(std::atof(lod3e.c_str())); }
        // "--reimport-drop-small=1" — the dialog's "Drop small disconnected parts"
        // (meshopt_SimplifyPrune), so a GUI-observed chain is reproducible headless.
        const std::string dropSmall = getArg("--reimport-drop-small=");
        if (!dropSmall.empty() && std::atoi(dropSmall.c_str()) != 0) {
            opt.lodSimplifyOptions |= meshopt_SimplifyPrune;
        }
        // LOD3 leaf interior decimation (mesh.json "foliageInnerRatio"/"foliageInnerError").
        const std::string innerR = getArg("--reimport-inner-ratio=");
        if (!innerR.empty()) { opt.foliageInnerRatio = static_cast<float>(std::atof(innerR.c_str())); }
        const std::string innerE = getArg("--reimport-inner-error=");
        if (!innerE.empty()) { opt.foliageInnerError = static_cast<float>(std::atof(innerE.c_str())); }
        // "--reimport-lod3-drop=2,3" — slots that vanish at LOD3 (mesh.json "lod3DropSlots").
        const std::string lod3Drop = getArg("--reimport-lod3-drop=");
        for (size_t i = 0; i < lod3Drop.size();) {
            size_t j = lod3Drop.find(',', i);
            if (j == std::string::npos) { j = lod3Drop.size(); }
            if (j > i) { opt.lod3DropSlots.push_back(static_cast<uint32_t>(std::atoi(lod3Drop.substr(i, j - i).c_str()))); }
            i = j + 1;
        }
        MeshManager mm;
        std::vector<float> uvDensity;
        const bool baked = mm.BakeToBinary(src, out, opt, &uvDensity);
        // Texture streaming A1: the density the bake measured belongs in the manifest it was
        // baked from (a manifest must describe its bake). Only when one was named -- a bare
        // --reimport-src/--reimport-out has no file to write.
        if (baked && !manifest.empty() && !MeshManager::WriteManifestUvDensity(manifest, uvDensity)) {
            LOG_ERROR(logging::LogCategory::Asset, "--reimport-manifest: could not write uvDensity to {}", manifest);
            return 4;
        }
        return baked ? 0 : 1;
    }
    return -1;
}
} // namespace boot
