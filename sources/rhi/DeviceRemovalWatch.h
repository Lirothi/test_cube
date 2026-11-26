#pragma once

#include <d3d12.h>

// The MOMENT a device is removed, which nothing else in the process knows: every other report is the first call that
// happened to notice, up to a frame later and on whatever thread it was. A fence reaches UINT64_MAX at the removal
// (the documented pattern, D3D12 "device removed" sample), so a thread that sleeps on that event wakes right then and
// logs the reason and where every thread of the process stands, return addresses by module: `python tools/symbolize.py`
// reads them. Costs one sleeping thread.
namespace render {

// `ahead` (GBV, --dr-check): also names whoever calls D3DKMTMarkDeviceAsError, and takes the threads' stacks at once,
// from the handles RefreshDeviceRemovalWatch opened; without it the stacks come tens of milliseconds late.
void StartDeviceRemovalWatch(ID3D12Device* device, bool ahead);
void StopDeviceRemovalWatch();
// Opens the threads that appeared since the last call.
void RefreshDeviceRemovalWatch();
// For a process about to die of the removal: gives the watch's report time to reach the log.
void WaitDeviceRemovalWatch(unsigned milliseconds);

} // namespace render
