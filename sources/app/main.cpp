#include <windows.h>
#include <mimalloc.h>
#pragma warning(push)
#pragma warning(disable: 28251)
#include "mimalloc-new-delete.h"
#pragma warning(pop)
#include "app/App.h"
#include "app/diagnostics/CullBenchmark.h"
#include "app/diagnostics/SceneStress.h"
#include "app/scene/SceneRenderQueue.h"
#include "core/task/diagnostics/TaskSystemStress.h"
#include "rendering/core/GraphicsDevice.h"
#include "rendering/diagnostics/RendererSubmissionStress.h"
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/rt/RtSmoke.h"
#include "text/TextManager.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

int ForceMi() { return mi_version(); }


namespace
{
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
    }

    App app;
    app.Run(hInstance, nShowCmd);
    return 0;
}
