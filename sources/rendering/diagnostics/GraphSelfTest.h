#pragma once

#include <string_view>

// Self-tests of the render graph's contracts (docs/architecture_review_plan.md R15), each through --crash-selftest.
namespace render
{
// --crash-selftest=pass-throw[@<Pass>] (R15.3): arms the named pass (RenderPassToWString's name, case-insensitive;
// empty = Skybox) to throw once where RunPassBody calls its body. The game then starts as always; the pass's frame task
// must end the process with a Fatal naming it. False (and an Error) for an unknown name.
bool ArmPassThrowSelfTest(std::string_view passName);
// The game thread, once a frame: the armed pass threw and frames still go on -- the exception was taken for a finished
// pass (the task system before R15.3). Says so and ends the run with exit 1. Nothing when unarmed.
void PassThrowSelfTestFrame();
// --crash-selftest=graphcap (R15.4, a crash::SelfTestArm): a graph of its own, no renderer, whose CL group is given one
// member past kMaxGroupMembers. Must end the process with the graph's Fatal naming that pass; returning is the failure.
void CrashSelfTestGraphCap();
} // namespace render
