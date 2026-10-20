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
// overflowDeathTest: instead of the scenarios, intentionally exceeds the
// dependents_ capacity — the process is EXPECTED to abort (validates the
// fail-fast overflow handling). Run via "--tasksystem-stress --stress-overflow";
// the caller must treat a non-zero/abnormal exit as the passing outcome.
int RunTaskSystemStress(bool overflowDeathTest);
