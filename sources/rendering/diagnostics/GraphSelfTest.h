#pragma once

#include <string_view>

// Self-tests of the render graph's contracts (docs/architecture_review_plan.md R15), each through --crash-selftest.
namespace render
{
// --crash-selftest=pass-throw[@<Pass>[:deep]] (R15.3): arms the named pass (RenderPassToWString's name,
// case-insensitive; empty = Skybox) to throw once where RunPassBody calls its body -- or, with :deep, inside the body
// right after its BeginCL, its command list open. The game then starts as always; the pass's frame task must end the
// process with a Fatal naming it, after the throw site's frames and dump (crash::NoteThrowSite). False (and an Error)
// for an unknown name.
bool ArmPassThrowSelfTest(std::string_view passName);
// The game thread, once a frame: the armed pass threw and frames still go on -- the exception was taken for a finished
// pass (the task system before R15.3). Says so and ends the run with exit 1. Nothing when unarmed.
void PassThrowSelfTestFrame();
// --crash-selftest=graphcap (R15.4, a crash::SelfTestArm): a graph of its own, no renderer, whose CL group is given one
// member past kMaxGroupMembers. Must end the process with the graph's Fatal naming that pass; returning is the failure.
void CrashSelfTestGraphCap();
// The other five checks of R15.4, one arm each (crash::SelfTestArm, a graph of its own, no renderer): a 17th CL group in
// a graph of 16 passes (graphgroups), a 17th pass (graphpasses), a prereq past the graph (graphdep), 17 declarations in
// a list of 16 (graphdecl), two passes waiting on each other (graphcycle). Each must end the process with the graph's
// Fatal; returning is the failure.
void CrashSelfTestGraphGroups();
void CrashSelfTestGraphPasses();
void CrashSelfTestGraphDep();
void CrashSelfTestGraphDecl();
void CrashSelfTestGraphCycle();
} // namespace render
