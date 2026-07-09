#pragma once

// Task-system stress harness, run via "test_cube.exe --tasksystem-stress"
// instead of the app. Hammers the interleavings normal frames never produce:
// nested worker-context waits, dependency fan-out at capacity, concurrent
// create/submit/recycle churn, repeated Start/Stop with in-flight work, and
// tiny worker counts. Writes details to tasksystem_stress.log in the working
// directory and returns the number of failed checks (0 = success).
//
// overflowDeathTest: instead of the scenarios, intentionally exceeds the
// dependents_ capacity — the process is EXPECTED to abort (validates the
// fail-fast overflow handling). Run via "--tasksystem-stress --stress-overflow";
// the caller must treat a non-zero/abnormal exit as the passing outcome.
int RunTaskSystemStress(bool overflowDeathTest);
