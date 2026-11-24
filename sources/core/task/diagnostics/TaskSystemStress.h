#pragma once

// Task-system stress harness, run via "test_cube.exe --tasksystem-stress"
// instead of the app. Hammers the interleavings normal frames never produce:
// nested worker-context waits, dependency fan-out at capacity, concurrent
// create/submit/recycle churn, repeated Start/Stop with in-flight work, and
// tiny worker counts. Also covers queue saturation, exception-safe range joins,
// recursive tracking, targeted waits and pool ownership across prolonged reuse.
// Regressions have a watchdog. Events/verdict go to the session log; the existing
// logs/tasksystem_stress.log artifact retains its summaries. Returns failed checks
// (0 = success, 98 = watchdog timeout).
//
// overflowDeathTest: instead of the scenarios, fills one task's dependents to TaskSystem::kDependentCapacity
// and adds one more — the process is EXPECTED to end there through the fan-out invariant (its Fatal line in the
// session log, abort, exit 3). Run via "--tasksystem-stress --stress-overflow"; returning (100) is the failure.
int RunTaskSystemStress(bool overflowDeathTest);

// --crash-selftest arms for the task system's dependency contract (docs/architecture_review_plan.md R15.2): each
// breaks it once and is expected to end the process through InvariantFailure; returning is the failure.
// taskdep: a dependency given to an already submitted task. taskdep-scheduled: a dependency on a task that ran.
void CrashSelfTestTaskDependencyAfterSubmit();
void CrashSelfTestTaskDependencyOnScheduled();
