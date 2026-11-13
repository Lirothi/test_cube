// The DirectX 12 Agility SDK (docs/distribution_build_plan.md §7, §11 item 7; third_party/agility/README.md).
//
// A shipped build asks for D3D12 runtime 619 from D3D12\ beside the exe: Windows 10's own runtime has no shader model 6.6,
// which the cooked shaders target. D3D12 reads these two exports off the process's exe when the device is first made; when
// the system's runtime is newer, the system keeps it. Dev builds export nothing: the system's runtime and its debug layer
// (GBV) as before -- an Agility runtime needs the matching d3d12SDKLayers.dll beside it.

#if TC_SHIPPING
extern "C"
{
    __declspec(dllexport) extern const unsigned int D3D12SDKVersion = 619;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif
