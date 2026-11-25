#pragma once

class Renderer;

// --retire-selftest[=control|upload] (docs/architecture_review_plan.md R16.1, R16.8): the deferred-release service proves
// the frames it waits for. The game starts as always; on its 20th frame:
//   (default) a buffer is copied from in a list submitted the way a game half submits (deferred to the hand-over,
//     Renderer::ExecuteOrDefer) and handed to render::deferred::Retire. PASS when it is released at a later frame AND
//     Renderer::IsFrameNumberComplete(the frame it was retired in) is true at that moment; FAIL when released early, or
//     not within 120 frames. With it a crowd of 512 entries is retired: FAIL when a boundary asks the fences more than
//     32 times (once a distinct frame number waiting, not once an entry -- the second review of wave 1, item 4).
//   control: the buffer is released at once instead, with the list still to go: the debug layer (Debug) / GBV must
//     name the destroyed resource -- the verdict is the log's.
//   upload (R16.8): from the GAME HALF while the render thread draws (App.cpp's game half), an UploadBatch whose upload
//     buffer lives only in the batch's keep-alive is submitted and the batch dropped at once -- its list waits for the
//     NEXT hand-over. PASS when what the batch held is released only after the GPU finished the frame that list goes
//     with (the frame number at Submit + 1); a release by the number of the call alone fails it.
namespace render
{
enum class RetireSelfTestMode { Retire, Control, Upload };
void ArmRetireSelfTest(RetireSelfTestMode mode);
// The game thread, once a frame after Renderer::BeginFrame (App.cpp): true when the verdict is in and the app should quit.
bool RetireSelfTestFrame(Renderer& renderer);
// The game thread's game half (App.cpp, beside the render thread's frame): the upload arm's submit.
void RetireSelfTestGameHalf(Renderer& renderer);
// After App::Run: 0 PASS (or the control arm done), 1 FAIL, -1 not armed.
int RetireSelfTestExitCode();
} // namespace render
