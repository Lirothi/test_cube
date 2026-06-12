#pragma once

#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "third_party/robin_hood.h"

// Tracks D3D12 resource states across parallel command-list recording.
//
// Two layers:
//  - a global "known states" map (locked; written from any thread),
//  - per-command-list first-use/current maps kept in per-thread lanes,
//    filled by Transition() while worker threads record.
//
// Lane ownership: a recording thread acquires its lane once (creation is
// guarded by one mutex); afterwards the lane is owned by exactly that thread
// and the record path takes no lock. Lane objects are heap-stable for the
// tracker's lifetime. TLS caches only stable values — the lane pointer, the
// owning tracker's id, the current CL key, and the lane epoch observed at
// registration. Per-CL entries are always re-resolved by key lookup: pointers
// into the flat map are invalidated by rehash and by the per-frame reset, and
// must never be cached.
//
// CONTRACT: the submit-phase methods (FindCLStateForCmd, ApplyFinalStates,
// AppendAcquireBarriers, SetKnownStateDirect, ResetLanesForFrame) are
// single-threaded and must not overlap recording (Transition) on any thread.
// They are intentionally unlocked. Debug builds validate the reset half of
// this contract (zero active recording scopes during ResetLanesForFrame) and
// that each command list is recorded through exactly one thread's lane.
class ResourceStateTracker
{
public:
    struct CLState {
        // First required resource state in this command list (not barriered inside the CL)
        robin_hood::unordered_flat_map<ID3D12Resource*, D3D12_RESOURCE_STATES> firstUse;
        // Current (latest) resource state within THIS command list (for transitions and final state)
        robin_hood::unordered_flat_map<ID3D12Resource*, D3D12_RESOURCE_STATES> current;
    };

    ResourceStateTracker();

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

    // --- Submit-phase helpers (single-threaded, unlocked by design; see contract above) ---
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
    struct CLStateLane {
        robin_hood::unordered_flat_map<ID3D12CommandList*, CLState> entries;
        uint64_t epoch = 0;
#ifdef _DEBUG
        // Transition() calls in flight on this lane; must be zero whenever
        // ResetLanesForFrame runs (validates the no-overlap contract).
        std::atomic<int32_t> activeRecordings{ 0 };
#endif
    };

    // Returns this thread's lane for THIS tracker instance, creating it on
    // first use (the only locked step on the record path).
    CLStateLane* LaneForThisThread_();

    std::mutex knownStatesMtx_;
    robin_hood::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> knownStates_;

    // Lane storage: grows only (guarded by lanesMtx_), freed only with the
    // tracker, so CLStateLane pointers handed to threads stay valid.
    std::mutex lanesMtx_;
    std::vector<std::unique_ptr<CLStateLane>> clLanes_;

    // Identifies this tracker instance in the TLS validation. Ids are never
    // reused (unlike addresses), so a thread that recorded through a different
    // tracker — or through a destroyed one — re-acquires a lane here instead
    // of dereferencing a foreign/dangling lane pointer.
    const uint64_t trackerId_;

    static thread_local CLStateLane*       tlLane_;
    static thread_local uint64_t           tlLaneTrackerId_;
    static thread_local ID3D12CommandList* tlCurrentKey_;
    static thread_local uint64_t           tlObservedEpoch_;
};
