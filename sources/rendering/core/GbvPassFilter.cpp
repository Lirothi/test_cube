#include "rendering/core/GbvPassFilter.h"

#include <d3d12.h>

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

bool ArmPassFilter(int listedPatchMode)
{
    if (g_requested.empty()) { return false; }
    SetListedPatchMode(listedPatchMode);

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
             PatchModeName(listedPatchMode), count, patched, PatchModeName(RestPatchMode()),
             RestPatchMode() == 1 ? "patched for resource-state tracking only, its shaders not validated"
                             : "not rewritten, its shaders not validated");
    boot::AddCount("gbv-passes: passes listed", static_cast<long long>(count));
    return true;
}

void ApplyPassPatchModeSlow(ID3D12GraphicsCommandList* cl, RenderPass pass)
{
    SetListMode(cl, IsListed(pass));
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
