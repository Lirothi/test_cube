#pragma once

// The content cook (docs/distribution_build_plan.md §8.0, §8.1): a build profile (tools/dist/<name>.json) -> the shipped
// levels' dependencies, the files code names, the record runs' list, a report, and the packages base.tcpak + shaders.tcpak.
//
//   x64/Cook/test_cube.exe --cook=tools/dist/test.json --cook-out=<folder> [--cook-check] [--levels=a.json,b.json]
//                          [--boot-level=a.json] [--version=1.2.3] [--no-encrypt] [--pak-key=<key.json>]
//
// --cook-check walks and reports (sizes before compression and an estimate after), builds nothing. The report is
// <folder>/cook_report.json; the exit code is the number of errors (0 = cooked). No window, no device.

namespace dist
{
    // -1 = no --cook on the command line; otherwise the process's exit code.
    int RunCook(const char* commandLine);
}
