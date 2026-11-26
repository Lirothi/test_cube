#include "rhi/GbvListMode.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include "core/diagnostics/BootProfile.h"
#include "core/logging/Log.h"

namespace render::gbv
{
namespace
{
int g_listedMode = 3; // D3D12_GPU_BASED_VALIDATION_SHADER_PATCH_MODE_GUARDED_VALIDATION
// Every unlisted list and the device default. STATE_TRACKING_ONLY (1) unless `--gbv-rest=none`:
// measured on wind_test it costs +1..3.5 s to the first frame (16.2 -> 17.0/19.6 s), and in exchange
// GBV's resource-state tracking stays as exact as in a full run -- under NONE the runtime declares
// it "possibly imprecise" from the first unpatched list on (id=1044).
int g_restMode = 1;
} // namespace

const char* PatchModeName(int mode)
{
    static const char* kNames[] = { "NONE", "STATE_TRACKING_ONLY", "UNGUARDED_VALIDATION",
                                     "GUARDED_VALIDATION" };
    return (mode >= 0 && mode <= 3) ? kNames[mode] : "?";
}

void SetListMode(ID3D12GraphicsCommandList* cl, bool patched)
{
#ifdef _DEBUG
    Microsoft::WRL::ComPtr<ID3D12DebugCommandList1> debugList;
    HRESULT hr = cl->QueryInterface(IID_PPV_ARGS(&debugList));
    if (SUCCEEDED(hr)) {
        D3D12_DEBUG_COMMAND_LIST_GPU_BASED_VALIDATION_SETTINGS settings{};
        settings.ShaderPatchMode =
            static_cast<D3D12_GPU_BASED_VALIDATION_SHADER_PATCH_MODE>(patched ? g_listedMode : g_restMode);
        hr = debugList->SetDebugParameter(D3D12_DEBUG_COMMAND_LIST_PARAMETER_GPU_BASED_VALIDATION_SETTINGS,
                                          &settings, sizeof(settings));
    }
    if (FAILED(hr)) {
        LOG_WARNING_ONCE(logging::LogCategory::RenderRhi,
                         "--gbv-passes: SetDebugParameter on a command list failed (hr=0x{:08X}) -- that list keeps "
                         "its previous patch mode, so what is validated is no longer what was listed",
                         static_cast<unsigned>(hr));
        return;
    }
    // The evidence that the per-list half reached the runtime, beside the device half's counters:
    // a narrowed run that patches nothing would otherwise look exactly like a fast, clean one.
    // CALLS, not lists: a BeginCL list is set twice (BeginCL, then SetCommandListName).
    if (patched) { boot::AddCount("gbv-passes: patched-mode SetDebugParameter calls"); }
#else
    (void)cl;
    (void)patched;
#endif
}

void RequestRestPatchMode(int mode)
{
    g_restMode = mode;
}

int RestPatchMode()
{
    return g_restMode;
}

void SetListedPatchMode(int mode)
{
    g_listedMode = mode;
}

void ResetListPatchModeSlow(ID3D12GraphicsCommandList* cl)
{
    SetListMode(cl, false);
}
} // namespace render::gbv
