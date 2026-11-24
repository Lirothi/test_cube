#include "rendering/diagnostics/GraphSelfTest.h"

#include "core/logging/Log.h"
#include "rendering/core/RenderGraph.h"

#include <cstdlib>
#include <cwctype>

namespace render
{
namespace
{
bool SameNameIgnoringCase(std::wstring_view a, std::string_view b)
{
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::towlower(a[i]) != std::towlower(static_cast<wchar_t>(static_cast<unsigned char>(b[i])))) { return false; }
    }
    return true;
}
} // namespace

bool ArmPassThrowSelfTest(std::string_view passName)
{
    if (passName.empty()) { passName = "Skybox"; }
    for (int i = 0; i < static_cast<int>(RenderPass::Count); ++i)
    {
        if (SameNameIgnoringCase(RenderPassToWString(static_cast<RenderPass>(i)), passName))
        {
            g_passThrowSelfTest.store(i, std::memory_order_relaxed);
            LOG_INFO(logging::LogCategory::RenderGraph, "crash self-test: pass-throw armed -- pass '{}' throws once in its "
                     "frame task", passName);
            return true;
        }
    }
    LOG_ERROR(logging::LogCategory::RenderGraph, "--crash-selftest=pass-throw@{}: no render pass of that name", passName);
    return false;
}

void PassThrowSelfTestFrame()
{
    static int framesSinceThrow = 0; // the game thread only
    if (!g_passThrowSelfTestThrown.load(std::memory_order_relaxed)) { return; }
    // A few frames: the throw happens on the render half, which may run a frame behind the game half.
    if (++framesSinceThrow < 8) { return; }
    LOG_ERROR(logging::LogCategory::RenderGraph, "crash self-test pass-throw: the pass threw and {} frames went on -- the "
              "frame task's exception was taken for a finished pass; did not crash", framesSinceThrow);
    logging::Flush(2000);
    std::_Exit(1);
}

void CrashSelfTestGraphCap()
{
    // Graph-building only: AddPass records the pass, nothing runs, so no renderer is needed. The members form the
    // contiguous prereq chain a group asks for; the bodies are never called.
    using Graph = RenderGraph<16>;
    auto graph = std::make_unique<Graph>();
    graph->BeginCLGroup();
    size_t previous = Graph::kNoGroup;
    for (size_t i = 0; i <= Graph::kMaxGroupMembers; ++i)
    {
        const RenderPass name = static_cast<RenderPass>(static_cast<std::size_t>(RenderPass::Main_Skybox) + i);
        LOG_INFO(logging::LogCategory::RenderGraph, "crash self-test graphcap: member {} of {} is '{}'", i + 1,
                 Graph::kMaxGroupMembers, RenderPassToWString(name));
        logging::Flush(1000);
        const auto body = [](RenderGraphPassContext) {};
        previous = i == 0 ? graph->AddPass(name, {}, body) : graph->AddPass(name, { previous }, body);
    }
    LOG_ERROR(logging::LogCategory::RenderGraph, "crash self-test graphcap: a CL group took {} members, one past its "
              "capacity -- did not crash", Graph::kMaxGroupMembers + 1);
    logging::Flush(2000);
    std::_Exit(1);
}
} // namespace render
