#pragma once

// Logging stress harness, run via "test_cube.exe --log-stress" instead of the app. Exercises
// what a normal frame never does: more producers than cores hammering one ring, a ring far too
// small for the load (exact delivered + dropped accounting), oversized records, a session file
// that cannot be created, repeated Initialize/Shutdown, synchronous mode, the viewer ring, a
// child process that dies on LOG_FATAL without Shutdown, and two children opening sessions at
// the same instant. Verdict in logs/log_stress.log; returns the number of failed checks.
//
// The harness owns the logger's lifetime itself, so main.cpp must dispatch to it BEFORE the
// normal session is created. Two internal child modes exist for the process tests
// ("--log-stress-fatal-child", "--log-stress-session-child"); they are spawned by the harness
// and are not meant to be run by hand.
int RunLogStress(const char* commandLine);
