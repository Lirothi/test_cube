#include "rendering/diagnostics/GraphSelfTest.h"

#include "core/logging/Log.h"
#include "core/task/TaskSystem.h"
#include "rendering/core/RenderGraph.h"

#include <cstdlib>
#include <cwctype>
#include <memory>

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
    constexpr std::string_view kDeep = ":deep";
    const bool deep = passName.size() >= kDeep.size() && passName.substr(passName.size() - kDeep.size()) == kDeep;
    if (deep)
    {
        passName.remove_suffix(kDeep.size());
        g_passThrowSelfTestDeep.store(true, std::memory_order_relaxed);
    }
    if (passName.empty()) { passName = "Skybox"; }
    for (int i = 0; i < static_cast<int>(RenderPass::Count); ++i)
    {
        if (SameNameIgnoringCase(RenderPassToWString(static_cast<RenderPass>(i)), passName))
        {
            g_passThrowSelfTest.store(i, std::memory_order_relaxed);
            LOG_INFO(logging::LogCategory::RenderGraph, "crash self-test: pass-throw armed -- pass '{}' throws once in its "
                     "frame task, {}", passName, deep ? "inside its body after BeginCL (its command list open)"
                                                      : "where RunPassBody calls its body");
            return true;
        }
    }
    LOG_ERROR(logging::LogCategory::RenderGraph, "--crash-selftest=pass-throw@{}: no render pass of that name", passName);
    return false;
}

void PassThrowSelfTestFrame()
{
    static int framesSinceThrow = 0; // the game thread only
    static int framesArmed = 0;
    if (!g_passThrowSelfTestThrown.load(std::memory_order_relaxed))
    {
        // An armed pass that never reaches its throw point -- not in the frame, or (with :deep) a body that opens its
        // list without ctx.BeginCL (Skybox does) -- judges nothing: say so instead of running for ever.
        if (g_passThrowSelfTest.load(std::memory_order_relaxed) < 0 || ++framesArmed < 600) { return; }
        LOG_ERROR(logging::LogCategory::RenderGraph, "crash self-test pass-throw: the armed pass never reached its throw "
                  "point in {} frames{} -- nothing judged", framesArmed,
                  g_passThrowSelfTestDeep.load(std::memory_order_relaxed) ? " (:deep throws from ctx.BeginCL)" : "");
        logging::Flush(2000);
        std::_Exit(1);
    }
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

// The other five checks of R15.4, each broken once in a graph of its own (no renderer: nothing runs). Each must end the
// process with the graph's Fatal naming what broke; returning is the failure (the second review of wave 1: until then
// only the members check had a self-test).
namespace
{
using SelfTestGraph = RenderGraph<16>;
constexpr std::size_t kSelfTestPasses = 16; // SelfTestGraph's MaxPasses

RenderPass SelfTestPassName(std::size_t i)
{
    return static_cast<RenderPass>(static_cast<std::size_t>(RenderPass::Main_Skybox) + i);
}

[[noreturn]] void DidNotCrash(const char* arm, const char* what)
{
    LOG_ERROR(logging::LogCategory::RenderGraph, "crash self-test {}: {} -- did not crash", arm, what);
    logging::Flush(2000);
    std::_Exit(1);
}
} // namespace

void CrashSelfTestGraphGroups()
{
    auto graph = std::make_unique<SelfTestGraph>();
    const auto body = [](RenderGraphPassContext) {};
    for (std::size_t i = 0; i <= kSelfTestPasses; ++i)
    {
        LOG_INFO(logging::LogCategory::RenderGraph, "crash self-test graphgroups: CL group {} of {}", i + 1,
                 kSelfTestPasses);
        logging::Flush(1000);
        graph->BeginCLGroup(); // the 17th must stop here
        if (i < kSelfTestPasses) { graph->AddPass(SelfTestPassName(i), {}, body); }
        graph->EndCLGroup();
    }
    DidNotCrash("graphgroups", "a graph of 16 passes took 17 CL groups");
}

void CrashSelfTestGraphPasses()
{
    auto graph = std::make_unique<SelfTestGraph>();
    const auto body = [](RenderGraphPassContext) {};
    for (std::size_t i = 0; i <= kSelfTestPasses; ++i)
    {
        graph->AddPass(SelfTestPassName(i % kSelfTestPasses), {}, body); // the 17th must stop here
    }
    DidNotCrash("graphpasses", "a graph declared for 16 passes took 17");
}

void CrashSelfTestGraphDep()
{
    auto graph = std::make_unique<SelfTestGraph>();
    const auto body = [](RenderGraphPassContext) {};
    graph->AddPass(SelfTestPassName(0), {}, body);
    LOG_INFO(logging::LogCategory::RenderGraph, "crash self-test graphdep: pass 2 depends on pass {} of a {}-pass graph",
             kSelfTestPasses + 4, kSelfTestPasses);
    logging::Flush(1000);
    graph->AddPass(SelfTestPassName(1), { kSelfTestPasses + 4 }, body);
    DidNotCrash("graphdep", "a prereq past the graph was taken");
}

void CrashSelfTestGraphDecl()
{
    auto graph = std::make_unique<SelfTestGraph>();
    const auto body = [](RenderGraphPassContext) {};
    const ResourceStateDecl d{};
    LOG_INFO(logging::LogCategory::RenderGraph, "crash self-test graphdecl: a pass declares 17 states, its list holds 16");
    logging::Flush(1000);
    graph->AddPass(SelfTestPassName(0), {}, { d, d, d, d, d, d, d, d, d, d, d, d, d, d, d, d, d }, body);
    DidNotCrash("graphdecl", "17 declarations went into a list of 16");
}

void CrashSelfTestGraphCycle()
{
    auto graph = std::make_unique<SelfTestGraph>();
    const auto body = [](RenderGraphPassContext) {};
    graph->AddPass(SelfTestPassName(0), { 1 }, body); // waits on the pass added next
    graph->AddPass(SelfTestPassName(1), { 0 }, body); // which waits on it
    LOG_INFO(logging::LogCategory::RenderGraph, "crash self-test graphcycle: '{}' and '{}' wait on each other; unrolling",
             RenderPassToWString(SelfTestPassName(0)), RenderPassToWString(SelfTestPassName(1)));
    logging::Flush(1000);
    // The topological order is built first, before anything touches the renderer or a task: the cycle's Fatal is there.
    graph->ExecuteParallel(nullptr, TaskSystem::Get());
    DidNotCrash("graphcycle", "a cycle was unrolled");
}
} // namespace render
