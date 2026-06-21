#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

#include "third_party/robin_hood.h"

class Mesh;

namespace rt {

// A bottom-level acceleration structure (the built result buffer). Geometry in
// this engine is static, so a BLAS is built once and cached per mesh.
struct Blas {
    Microsoft::WRL::ComPtr<ID3D12Resource> result;
    D3D12_GPU_VIRTUAL_ADDRESS Address() const {
        return result ? result->GetGPUVirtualAddress() : 0;
    }
};

// Builds and caches BLASes for meshes. The result buffers live in
// D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE and must NOT be routed
// through the normal resource-state tracker (see S5). Scratch buffers are
// retained until the caller confirms the build's fence has signaled, then
// released via ReleaseCompletedScratch().
class AccelerationStructureManager {
public:
    void Init(ID3D12Device5* device5) { device5_ = device5; }

    // Returns the cached BLAS for `mesh`, building it on `cmdList4` on first use.
    // The build's UAV barrier is recorded too. On failure the returned Blas has
    // a null result (Address() == 0).
    const Blas& GetOrBuildBlas(Mesh* mesh, ID3D12GraphicsCommandList4* cmdList4);

    // Release scratch buffers retained from builds. Call ONLY after the fence for
    // the command list(s) that ran the builds has completed — the GPU is still
    // reading scratch until then.
    void ReleaseCompletedScratch() { pendingScratch_.clear(); }

    // Drop every cached BLAS and any retained scratch (e.g. on device teardown).
    void Reset() {
        blasCache_.clear();
        pendingScratch_.clear();
    }

    bool HasPendingScratch() const { return !pendingScratch_.empty(); }

private:
    ID3D12Device5* device5_ = nullptr;
    robin_hood::unordered_map<Mesh*, Blas> blasCache_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> pendingScratch_;
};

} // namespace rt
