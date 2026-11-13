# DirectX 12 Agility SDK runtime -- 1.619.6

`x64/D3D12Core.dll` from the NuGet package `Microsoft.Direct3D.D3D12` 1.619.6 (published 2026-09-14,
https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.6), `D3D12_SDK_VERSION` 619. Only the runtime is here:
the package's `distributable files.txt` names it as redistributable under `LICENSE.txt` (Microsoft DirectX terms).

Why (docs/distribution_build_plan.md §7, §11 item 7): Windows 10's own D3D12 runtime has no shader model 6.6, which the
shipped shaders are compiled for; a Shipping build exports `D3D12SDKVersion = 619` and `D3D12SDKPath = ".\D3D12\"`
(sources/app/AgilitySdk.cpp), and tools/build_dist.py puts this DLL in `D3D12\` beside the exe. Where the system's runtime
is newer, Windows keeps its own. Dev builds export nothing and run the system's runtime (and its debug layer, GBV).

Updating: take a release at least a few weeks old, replace the DLL and the licence, change the number in AgilitySdk.cpp.
