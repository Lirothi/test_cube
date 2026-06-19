#pragma once
#include <d3d12.h>
#include "rendering/core/RenderContext.h" // RenderContext::kMaxBindings

// Step 3 (draw batching / state-change minimization): per-command-list cache of the
// last-bound pipeline/root state, so Material::Bind and Mesh::Draw can skip redundant
// SetRootSignature / SetPipelineState / descriptor-table / CBV / IA calls across
// back-to-back objects that share state (e.g. a sorted run of identical-material meshes).
//
// One instance per recording THREAD (thread_local). Reset by Renderer::BeginThreadCommandList
// at the start of every command list / bundle — a fresh command list inherits no state, so
// the first draw in it always binds fully. Concurrent chunk recording is safe: each worker
// thread owns its own cache.
namespace render
{
    // Toggle (env-driven, for before/after measurement). Default on.
    inline bool g_bindBatchingEnabled = true;

    struct CommandListBindState
    {
        ID3D12RootSignature* rs = nullptr;
        ID3D12PipelineState* pso = nullptr;
        bool isCompute = false;

        // Root arguments, keyed by shader register (same layout as RenderContext).
        D3D12_GPU_VIRTUAL_ADDRESS  cbv[RenderContext::kMaxBindings] = {};
        D3D12_GPU_DESCRIPTOR_HANDLE srvTable[RenderContext::kMaxBindings] = {};
        D3D12_GPU_DESCRIPTOR_HANDLE uavTable[RenderContext::kMaxBindings] = {};
        D3D12_GPU_DESCRIPTOR_HANDLE samplerTable[RenderContext::kMaxBindings] = {};

        // Input assembler.
        D3D12_GPU_VIRTUAL_ADDRESS vb = 0;
        D3D12_GPU_VIRTUAL_ADDRESS ib = 0;
        D3D12_PRIMITIVE_TOPOLOGY  topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

        // SetRootSignature invalidates ALL previously-set root arguments, so a root-sig
        // change must drop the cached root-argument values (IA state is unaffected).
        void OnRootSignatureChanged()
        {
            for (auto& v : cbv) { v = 0; }
            for (auto& v : srvTable) { v = { 0 }; }
            for (auto& v : uavTable) { v = { 0 }; }
            for (auto& v : samplerTable) { v = { 0 }; }
        }

        void Reset() { *this = CommandListBindState{}; }
    };

    inline thread_local CommandListBindState g_clBindState;
}
