#pragma once

// Renderer submission stress harness, run via
// "test_cube.exe --renderer-submission-stress" instead of the app. CPU-only:
// drives the registration/gathering layer (SubmitTimeline) with fake
// command-list pointer values — registration only stores pointers; the real
// submit path is never run (it calls Close()). Covers: retention beyond the old 8-entry
// inline capacity, registration-order preservation (single-threaded and
// concurrent), persistent-pool reuse across frames with varying batch counts,
// deterministic submit ordering (shuffled registration order always yields the
// same localOrder-sorted gather), the invariant death tests (null
// registration, stale/out-of-range batch index, duplicate registration,
// duplicate localOrder among directs and among bundles — each spawned in a
// child process that is EXPECTED to abort). The four ResourceStateTracker
// scenarios went with that class at barrier-plan Step 7; resource states are
// now compiled by the render graph and have no recording layer to stress.
// Writes details to renderer_submission_stress.log
// in the working directory and returns the number of failed checks
// (0 = success).
//
// cmdLine is the raw command line; when it contains one of the death-test
// sub-flags (--stress-null-cl, --stress-invalid-batch, --stress-duplicate-cl)
// the process runs that single violation instead and must abort — surviving
// returns 100, which the parent treats as the failing outcome.
int RunRendererSubmissionStress(const char* cmdLine);
