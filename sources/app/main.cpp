#include <windows.h>
#include <mimalloc.h>
#pragma warning(push)
#pragma warning(disable: 28251)
#include "mimalloc-new-delete.h"
#pragma warning(pop)
#include "app/App.h"
#include "assets/AssetImporter.h"
#include "app/diagnostics/CullBenchmark.h"
#include "app/diagnostics/SceneStress.h"
#include "app/scene/SceneRenderQueue.h"
#include "core/task/diagnostics/TaskSystemStress.h"
#include "rendering/core/GraphicsDevice.h"
#include "rendering/diagnostics/RendererSubmissionStress.h"
#include "rendering/meshes/MeshManager.h" // W7.1b: g_meshBakeMode (--bake-meshes)
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/rt/RtSmoke.h"
#include "text/TextManager.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

int ForceMi() { return mi_version(); }


namespace
{
// Extract a CLI value for `key`, accepting both "key=value" and "key value" (optionally quoted
// for paths with spaces). Returns "" when the key is absent or has no value.
std::string ExtractArgValue(const char* cmd, const char* key)
{
    const char* p = std::strstr(cmd, key);
    if (!p) { return {}; }
    p += std::strlen(key);
    while (*p == '=' || std::isspace(static_cast<unsigned char>(*p))) { ++p; }
    std::string v;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') { v.push_back(*p); ++p; }
    } else {
        while (*p && !std::isspace(static_cast<unsigned char>(*p))) { v.push_back(*p); ++p; }
    }
    return v;
}

void EnableDpiAwareness()
{
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

    // Try to opt-in to the most precise DPI awareness available on the host OS.
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setProcessDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setProcessDpiAwarenessContext &&
            setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        {
            return;
        }
    }

    HMODULE shcore = GetModuleHandleW(L"shcore.dll");
    if (!shcore)
    {
        shcore = LoadLibraryW(L"shcore.dll");
    }
    if (shcore)
    {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto setProcessDpiAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setProcessDpiAwareness &&
            SUCCEEDED(setProcessDpiAwareness(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/)))
        {
            return;
        }
    }

    if (user32)
    {
        using SetProcessDpiAwareFn = BOOL(WINAPI*)();
        if (auto setProcessDpiAware = reinterpret_cast<SetProcessDpiAwareFn>(
                GetProcAddress(user32, "SetProcessDPIAware")))
        {
            setProcessDpiAware();
        }
    }
}
} // namespace

// Entry point
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nShowCmd
)
{
    // "--tasksystem-stress" runs the task-system stress harness instead of the
    // app; exit code is the number of failed checks. With "--stress-overflow"
    // the process is expected to abort (dependents_ overflow death test).
    if (lpCmdLine && std::strstr(lpCmdLine, "tasksystem-stress") != nullptr) {
        const bool overflow = std::strstr(lpCmdLine, "stress-overflow") != nullptr;
        return RunTaskSystemStress(overflow);
    }

    // "--renderer-submission-stress" runs the CPU-only submission-timeline
    // stress harness instead of the app; exit code is the number of failed
    // checks. Its death-test sub-flags (--stress-*) intentionally abort and
    // are spawned by the harness itself in child processes.
    if (lpCmdLine && std::strstr(lpCmdLine, "renderer-submission-stress") != nullptr) {
        return RunRendererSubmissionStress(lpCmdLine);
    }

    if (lpCmdLine && std::strstr(lpCmdLine, "textmanager-benchmark") != nullptr) {
        return RunTextManagerBenchmark("textmanager_benchmark.csv");
    }

    // "--cull-benchmark" times the per-object frustum intersect (legacy DirectXMath vs the
    // precomputed-plane path) over a fixed AABB set; results in cull_benchmark.txt.
    if (lpCmdLine && std::strstr(lpCmdLine, "cull-benchmark") != nullptr) {
        return RunCullBenchmark("cull_benchmark.txt");
    }

    // "--cull-nofuse" forces the split Bucketize()+Cull() path for self-culling views, for an
    // in-engine A/B against the default fused BucketizeCull. Benchmark-only.
    if (lpCmdLine && std::strstr(lpCmdLine, "cull-nofuse") != nullptr) {
        g_useFusedBucketizeCull = false;
    }

    // "--rt-smoke" runs the headless DXR smoke harness (device + RT caps + a
    // trivial BLAS/TLAS build) instead of the app; verdict in rt_smoke.txt,
    // exit code 0 on PASS/SKIP, non-zero on FAIL.
    if (lpCmdLine && std::strstr(lpCmdLine, "rt-smoke") != nullptr) {
        return RunRtSmoke("rt_smoke.txt");
    }

    // "--rt-force-as-fail" (S13 test hook): make every acceleration-structure
    // allocation fail, so the graceful RT-disable → SSR-fallback path can be
    // exercised in the live app without actually exhausting VRAM.
    if (lpCmdLine && std::strstr(lpCmdLine, "rt-force-as-fail") != nullptr) {
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
        opts.highQuality = std::strstr(lpCmdLine, "--high") != nullptr; // opt-in exhaustive BC7
        opts.flipGreen   = std::strstr(lpCmdLine, "--flip-green") != nullptr;
        opts.bc5Normal   = std::strstr(lpCmdLine, "--bc5-normal") != nullptr;
        opts.centerNormals = std::strstr(lpCmdLine, "--center-normals") != nullptr;
        opts.useGpu      = std::strstr(lpCmdLine, "--cpu") == nullptr; // H5: GPU BC encode default-on
        return assets::RunImport(opts);
    }

    EnableDpiAwareness();

    // "--scene-stress" (optionally "--scene-stress=<iterations>") boots the real
    // renderer/device/scene and then autonomously hammers the scene-lifecycle
    // churn operations (level reload/switch, window resize, DLSS mode, render/
    // reflection scale, editor spawn/delete) to reproduce the intermittent
    // launch/render crash. Verdict in scene_stress.log; exit 0 = clean through
    // all iterations, nonzero = a fault was caught (the log names the op).
    if (lpCmdLine) {
        if (const char* flag = std::strstr(lpCmdLine, "scene-stress")) {
            int iterations = 0; // 0 => driver default
            if (const char* eq = std::strchr(flag, '=')) {
                iterations = std::atoi(eq + 1);
            }
            // DRED before device creation so the driver can name the faulting
            // op/resource on a device removal (cheap; does not perturb the race).
            GraphicsDevice::EnableDredForStress(true);
            // GPU-based validation is a heavier, opt-in second signal
            // (--scene-stress-gbv); it perturbs timing so it's off by default.
            // In GBV mode, InfoQueue errors are logged-but-not-fatal so the run
            // reaches the actual device-removal with GBV annotating each frame.
            const bool gbv = std::strstr(lpCmdLine, "scene-stress-gbv") != nullptr;
            if (gbv) {
                GraphicsDevice::EnableGbvForStress(true);
            }
            return RunSceneStress(hInstance, nShowCmd, iterations, /*gbvContinue=*/gbv);
        }
    }

    // "--level=<path>" overrides the boot level (headless verification of a specific level).
    if (lpCmdLine) {
        if (const char* flag = std::strstr(lpCmdLine, "--level=")) {
            const char* p = flag + std::strlen("--level=");
            std::string path;
            while (*p && !std::isspace(static_cast<unsigned char>(*p))) { path.push_back(*p); ++p; }
            g_bootLevelPath = path;
        }
        // "--shot=<path>" grabs the backbuffer to a PNG after "--shot-delay=<sec>" (default 7)
        // and exits — reliable headless verification on the flip-model swapchain.
        if (const char* flag = std::strstr(lpCmdLine, "--shot=")) {
            const char* p = flag + std::strlen("--shot=");
            std::string path;
            while (*p && !std::isspace(static_cast<unsigned char>(*p))) { path.push_back(*p); ++p; }
            g_shotPath = path;
        }
        if (const char* flag = std::strstr(lpCmdLine, "--shot-delay=")) {
            g_shotDelaySec = std::atof(flag + std::strlen("--shot-delay="));
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
                while (*p && !std::isspace(static_cast<unsigned char>(*p))) { v.push_back(*p); ++p; }
                return v;
            };
            const std::string src = getArg("--reimport-src=");
            const std::string out = getArg("--reimport-out=");
            if (src.empty() || out.empty()) { return 2; }
            MeshLoadOptions opt{};
            // Match the runtime mesh load exactly (StaticMesh/TransparentStaticMesh): the engine
            // loads glTF with CCW winding (wantCW=false) for its FrontCounterClockwise=FALSE PSOs.
            // Baking with the default wantCW=true flips the triangles -> the .bin renders backfaces.
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
            MeshManager mm;
            return mm.BakeToBinary(src, out, opt) ? 0 : 1;
        }
    }

    App app;
    app.Run(hInstance, nShowCmd);
    return 0;
}
