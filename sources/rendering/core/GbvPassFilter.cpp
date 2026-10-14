#include "rendering/core/GbvPassFilter.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <atomic>
#include <string_view>
#include <utility>

#include "core/diagnostics/BootProfile.h"
#include "core/logging/Log.h"

namespace render::gbv
{
namespace
{
constexpr std::size_t kPassCount = static_cast<std::size_t>(RenderPass::Count);

std::string g_requested;
// Written by ArmPassFilter before the first frame and by JoinCLGroup at graph build; read on every
// recording thread. Atomic so a group joining on one thread is not a data race with a list being
// named on another.
std::atomic<bool> g_listed[kPassCount]; // static storage: all false
int g_listedMode = 3; // D3D12_GPU_BASED_VALIDATION_SHADER_PATCH_MODE_GUARDED_VALIDATION
// Every unlisted list and the device default. STATE_TRACKING_ONLY (1) unless `--gbv-rest=none`:
// measured on wind_test it costs +1..3.5 s to the first frame (16.2 -> 17.0/19.6 s), and in exchange
// GBV's resource-state tracking stays as exact as in a full run -- under NONE the runtime declares
// it "possibly imprecise" from the first unpatched list on (id=1044).
int g_restMode = 1;

const char* ModeName(int mode)
{
    static const char* kNames[] = { "NONE", "STATE_TRACKING_ONLY", "UNGUARDED_VALIDATION",
                                     "GUARDED_VALIDATION" };
    return (mode >= 0 && mode <= 3) ? kNames[mode] : "?";
}

std::string PassName(RenderPass pass)
{
    const std::wstring_view wide = RenderPassToWString(pass);
    std::string narrow;
    narrow.reserve(wide.size());
    for (wchar_t c : wide) { narrow.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?'); }
    return narrow;
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// `token` names `pass` exactly, or is a dotted prefix of it ("GBuffer" -> "GBuffer.Driver").
bool Names(std::string_view token, std::string_view pass)
{
    if (pass.size() < token.size()) { return false; }
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (Lower(token[i]) != Lower(pass[i])) { return false; }
    }
    return pass.size() == token.size() || pass[token.size()] == '.';
}

bool IsListed(RenderPass pass)
{
    const std::size_t index = static_cast<std::size_t>(pass);
    return index < kPassCount && g_listed[index].load(std::memory_order_relaxed);
}

// Every valid name, wrapped into lines: the whole set is longer than one log record (1 KiB) holds,
// and a list that silently stops halfway would read as the complete one.
void LogAllPassNames()
{
    std::string text = "--gbv-passes: valid names:\n ";
    std::size_t lineLength = 0;
    for (std::size_t i = 0; i < kPassCount; ++i) {
        const std::string name = PassName(static_cast<RenderPass>(i));
        if (name.empty()) { continue; }
        if (lineLength > 100) { text += "\n "; lineLength = 0; }
        text += ' ';
        text += name;
        lineLength += name.size() + 1;
    }
    logging::WriteRawLines(logging::LogLevel::Info, logging::LogCategory::RenderRhi, text);
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
} // namespace

void RequestPassFilter(std::string list)
{
#ifdef _DEBUG
    g_requested = std::move(list);
#else
    LOG_WARNING(logging::LogCategory::RenderRhi,
                "--gbv-passes={} ignored: GPU-based validation exists only in the Debug build", list);
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

bool ArmPassFilter(int listedPatchMode)
{
    if (g_requested.empty()) { return false; }
    g_listedMode = listedPatchMode;

    std::string patched;
    std::size_t count = 0;
    bool anyUnknown = false;
    std::size_t begin = 0;
    while (begin <= g_requested.size()) {
        std::size_t end = g_requested.find(',', begin);
        if (end == std::string::npos) { end = g_requested.size(); }
        std::string_view token(g_requested.data() + begin, end - begin);
        while (!token.empty() && token.front() == ' ') { token.remove_prefix(1); }
        while (!token.empty() && token.back() == ' ') { token.remove_suffix(1); }
        begin = end + 1;
        if (token.empty()) { continue; }

        bool matched = false;
        for (std::size_t i = 0; i < kPassCount; ++i) {
            const std::string name = PassName(static_cast<RenderPass>(i));
            if (name.empty() || !Names(token, name)) { continue; }
            matched = true;
            if (g_listed[i].exchange(true, std::memory_order_relaxed)) { continue; }
            if (!patched.empty()) { patched += ", "; }
            patched += name;
            ++count;
        }
        if (!matched) {
            anyUnknown = true;
            LOG_WARNING(logging::LogCategory::RenderRhi,
                        "--gbv-passes: '{}' names no render pass -- ignored (names are RenderPassToWString's, "
                        "case-insensitive; the list follows)", token);
        }
    }
    if (anyUnknown) { LogAllPassNames(); }

    if (count == 0) {
        LOG_WARNING(logging::LogCategory::RenderRhi,
                    "--gbv-passes: no name matched a pass -- GBV patches EVERY pass, as without the flag");
        return false;
    }
    g_passFilterArmed = true;
    LOG_INFO(logging::LogCategory::RenderRhi,
             "--gbv-passes: shader patching ({}) ONLY on the command lists of {} pass(es): {}. Every other "
             "list is {} -- {}. A CL group shares one list, so one listed member patches the whole group "
             "(logged when a group joins).",
             ModeName(listedPatchMode), count, patched, ModeName(g_restMode),
             g_restMode == 1 ? "patched for resource-state tracking only, its shaders not validated"
                             : "not rewritten, its shaders not validated");
    boot::AddCount("gbv-passes: passes listed", static_cast<long long>(count));
    return true;
}

void ApplyPassPatchModeSlow(ID3D12GraphicsCommandList* cl, RenderPass pass)
{
    SetListMode(cl, IsListed(pass));
}

void ResetListPatchModeSlow(ID3D12GraphicsCommandList* cl)
{
    SetListMode(cl, false);
}

void JoinCLGroup(const RenderPass* members, std::size_t count)
{
    if (!g_passFilterArmed || !members) { return; }
    bool anyListed = false;
    for (std::size_t i = 0; i < count && !anyListed; ++i) { anyListed = IsListed(members[i]); }
    if (!anyListed) { return; }

    std::string group;
    std::string joined;
    for (std::size_t i = 0; i < count; ++i) {
        const std::string name = PassName(members[i]);
        if (!group.empty()) { group += ", "; }
        group += name;
        const std::size_t index = static_cast<std::size_t>(members[i]);
        if (index >= kPassCount || g_listed[index].exchange(true, std::memory_order_relaxed)) { continue; }
        if (!joined.empty()) { joined += ", "; }
        joined += name;
    }
    // The graph is rebuilt every frame; only the first build adds anyone, so this fires once.
    if (!joined.empty()) {
        LOG_INFO(logging::LogCategory::RenderRhi,
                 "--gbv-passes: CL group [{}] records into ONE command list and has a listed member, so the "
                 "whole list is patched -- added: {}", group, joined);
    }
}

} // namespace render::gbv
