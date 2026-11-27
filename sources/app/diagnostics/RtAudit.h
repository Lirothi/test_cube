#pragma once
// The render thread's audit -- game thread plan G9 (docs/game_thread_plan.md section 5).
//
//     test_cube.exe --level=data/levels/wind_test.json --rt-audit=600        (10 minutes; the default)
//     test_cube.exe --level=data/levels/wind_test.json --rt-audit=30 --rt-audit-selftest
//
// Runs with the render thread on (frame.renderThread 1 whatever the settings say). Random 0-2 ms pauses where the two
// halves meet make every frame overlap differently; the render thread hashes what the hand-over froze for it (every
// object's render state -- R7.1, ObjectRenderState: matrices, position, visibility, shadow switch, move flag -- and
// every light's render desc) at the start of its frame and again at the end -- a hash that moved means the game half
// wrote it while the frame was drawn, and the log names the field. The verdict is one line in the session log, `rt
// audit: PASS ...` / `rt audit: FAIL ...`, and the process's exit code is the number of fields that moved. The self-test
// writes a field of one object's render state from the game half while the render thread draws, one such frame in
// twenty, each field in turn: the audit must FAIL naming them, or its silence proves nothing (exit 0 when it does, 1
// when it does not). With --scene-stress it rides the harness's churn (levels, spawns, resizes) and
// gives its verdict at the harness's end.

class Scene;

namespace rtaudit
{
void Configure(double seconds, bool selfTest);
bool Enabled();
// A random 0-2 ms pause (a spin: Sleep cannot do under a millisecond). Nothing when the audit is off.
void Jitter();
// The render thread, around Scene::Render.
void BeginRenderFrame(const Scene& scene);
void EndRenderFrame(const Scene& scene);
// The game thread, in the game half (beside the render thread's frame): the self-test's write.
void GameHalf(Scene& scene);
// The game thread, after the wait for a frame: true once the run is over and its verdict is logged. `now`: the verdict
// at once, whatever the time (the scene-stress harness, at its end).
bool Finished(double nowSeconds, bool now = false);
int ExitCode();
} // namespace rtaudit
