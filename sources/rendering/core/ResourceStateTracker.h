#pragma once

#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "third_party/robin_hood.h"

// Tracks D3D12 resource states across parallel command-list recording.
//
// Two layers:
//  - a global "known states" map (locked; written from any thread),
//  - per-command-list first-use/current maps kept in lock-striped lanes with a
//    thread-local fast path, filled by Transition() while worker threads record.
//
// At submit time (single-threaded), the caller walks the command lists in
// execution order and uses FindCLStateForCmd/AppendAcquireBarriers/
// ApplyFinalStates to inject the cross-list transition barriers. Those helpers
// intentionally access the global map without locking, matching the original
// single-threaded submit-phase behavior.
class ResourceStateTracker
{
public:
    struct CLState {
        // First required resource state in this command list (not barriered inside the CL)
        robin_hood::unordered_flat_map<ID3D12Resource*, D3D12_RESOURCE_STATES> firstUse;
        // Current (latest) resource state within THIS command list (for transitions and final state)
        robin_hood::unordered_flat_map<ID3D12Resource*, D3D12_RESOURCE_STATES> current;
    };

    // --- Global known states (locked) ---
    void SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state);
    void ClearResourceState(ID3D12Resource* res);
    D3D12_RESOURCE_STATES GetGlobalKnownState(ID3D12Resource* res);
    void ClearAllKnownStates();
    void ForgetResources(const std::vector<ID3D12Resource*>& resources);

    // --- Per-CL tracking (worker threads) ---
    void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after);
    void RegisterCurrentThreadCL(ID3D12GraphicsCommandList* cl);
    void UnregisterCurrentThreadCL();

    // --- Submit-phase helpers (single-threaded, unlocked by design) ---
    const CLState* FindCLStateForCmd(ID3D12CommandList* cmd) const;
    // Folds a command list's final states into the global map.
    void ApplyFinalStates(const CLState& st);
    // Emits the barriers needed to bring resources from their globally known
    // state into the command list's first-use state, updating the global map.
    void AppendAcquireBarriers(const CLState& st, std::vector<D3D12_RESOURCE_BARRIER>& out);
    void SetKnownStateDirect(ID3D12Resource* res, D3D12_RESOURCE_STATES state);
    // Invalidates all per-CL entries for the next frame (bumps lane epochs).
    void ResetLanesForFrame();

private:
    struct CLStateEntry {
        ID3D12CommandList* cmd = nullptr;
        CLState st;
        uint64_t epoch = 0;
    };

    static constexpr uint32_t kCLStateLanes = 64;
    struct CLStateLane {
        robin_hood::unordered_flat_map<ID3D12CommandList*, CLStateEntry> entries;
        uint64_t epoch = 0;
    };

    std::mutex knownStatesMtx_;
    robin_hood::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> knownStates_;

    std::atomic<uint32_t> clLaneCount_{ 0 };
    CLStateLane           clLanes_[kCLStateLanes];

    // TLS: which lane the thread uses and which CL is currently active
    static thread_local uint32_t      tlLaneIndex_;
    static thread_local CLStateEntry* tlCurrentEntry_;
};
