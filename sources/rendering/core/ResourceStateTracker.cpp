#include "rendering/core/ResourceStateTracker.h"

#include "rendering/core/RendererInvariantFailure.h"

#include <cassert>

thread_local ResourceStateTracker::CLStateLane* ResourceStateTracker::tlLane_ = nullptr;
thread_local uint64_t ResourceStateTracker::tlLaneTrackerId_ = 0;
thread_local ID3D12CommandList* ResourceStateTracker::tlCurrentKey_ = nullptr;
thread_local uint64_t ResourceStateTracker::tlObservedEpoch_ = 0;

namespace {
// Tracker ids start at 1 so the TLS default (0) never matches an instance.
std::atomic<uint64_t> gNextTrackerId{ 1 };

#ifdef _DEBUG
struct ScopedLaneRecording {
    explicit ScopedLaneRecording(std::atomic<int32_t>& counter) : counter_(counter)
    {
        counter_.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ScopedLaneRecording()
    {
        counter_.fetch_sub(1, std::memory_order_acq_rel);
    }
    std::atomic<int32_t>& counter_;
};
#endif
} // namespace

ResourceStateTracker::ResourceStateTracker()
    : trackerId_(gNextTrackerId.fetch_add(1, std::memory_order_relaxed))
{
}

void ResourceStateTracker::SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state)
{
    if (res == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(knownStatesMtx_);
    knownStates_[res] = state;
}

void ResourceStateTracker::ClearResourceState(ID3D12Resource* res)
{
    if (res == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(knownStatesMtx_);
    knownStates_.erase(res);
}

D3D12_RESOURCE_STATES ResourceStateTracker::GetGlobalKnownState(ID3D12Resource* res)
{
    std::lock_guard<std::mutex> lk(knownStatesMtx_);
    auto it = knownStates_.find(res);
    return (it == knownStates_.end()) ? D3D12_RESOURCE_STATE_COMMON : it->second;
}

void ResourceStateTracker::ClearAllKnownStates()
{
    std::lock_guard<std::mutex> lk(knownStatesMtx_);
    knownStates_.clear();
}

void ResourceStateTracker::ForgetResources(const std::vector<ID3D12Resource*>& resources)
{
    if (resources.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lk(knownStatesMtx_);
    for (auto* res : resources) {
        knownStates_.erase(res);
    }
}

void ResourceStateTracker::Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after)
{
    if (!cl || !res) { return; }
    ID3D12CommandList* key = static_cast<ID3D12CommandList*>(cl);

    // Fast path: TLS holds the thread's lane and the CL key it last recorded
    // into. The dereference of tlLane_ is safe only after the tracker-id check
    // (short-circuit order matters): a stale pointer from another or destroyed
    // tracker never gets this far.
    CLStateLane* lane = tlLane_;
    if (lane == nullptr || tlLaneTrackerId_ != trackerId_ ||
        tlObservedEpoch_ != lane->epoch || tlCurrentKey_ != key) {
        RegisterCurrentThreadCL(cl);
        lane = tlLane_;
    }

#ifdef _DEBUG
    ScopedLaneRecording recordingScope(lane->activeRecordings);
#endif

    // Resolve the entry by key — never via a cached pointer (the flat map
    // relocates entries on rehash, and the per-frame reset destroys them).
    auto found = lane->entries.find(key);
    if (found == lane->entries.end()) {
        // Unreachable unless the no-overlap contract broke; dropping the
        // transition would record wrong barriers.
        RendererInvariantFailure("ResourceStateTracker::Transition: registered CL entry missing from its lane");
    }
    CLState& st = found->second;

    auto itCur = st.current.find(res);
    if (itCur == st.current.end()) {
        // First use in this command list — no intra-CL barrier required
        st.firstUse.emplace(res, after);
        st.current.emplace(res, after);
        return;
    }

    const D3D12_RESOURCE_STATES before = itCur->second;
    if (before == after) { return; }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);

    itCur->second = after;
}

ResourceStateTracker::CLStateLane* ResourceStateTracker::LaneForThisThread_()
{
    if (tlLane_ != nullptr && tlLaneTrackerId_ == trackerId_) {
        return tlLane_;
    }

    std::lock_guard<std::mutex> lk(lanesMtx_);
    CLStateLane* lane = nullptr;
    try {
        clLanes_.push_back(std::make_unique<CLStateLane>());
        lane = clLanes_.back().get();
    }
    catch (...) {
        // Never share a lane and never throw across a worker task — a thread
        // without its own lane cannot track states safely.
        RendererInvariantFailure("ResourceStateTracker: command-list state lane allocation failed");
    }
    tlLane_ = lane;
    tlLaneTrackerId_ = trackerId_;
    return lane;
}

void ResourceStateTracker::RegisterCurrentThreadCL(ID3D12GraphicsCommandList* cl)
{
    if (!cl) {
        return;
    }
    CLStateLane& lane = *LaneForThisThread_();
    ID3D12CommandList* key = static_cast<ID3D12CommandList*>(cl);

#ifdef _DEBUG
    ScopedLaneRecording recordingScope(lane.activeRecordings);
#endif

    if (lane.entries.empty()) {
        lane.entries.reserve(32);
    }
    auto found = lane.entries.find(key);
    if (found == lane.entries.end()) {
        CLState& st = lane.entries[key];
        st.firstUse.reserve(8);
        st.current.reserve(16);
    }
    // An existing entry keeps its accumulated state: re-registering the same
    // CL on this thread resumes recording where it left off within the frame.

    tlCurrentKey_ = key;
    tlObservedEpoch_ = lane.epoch;
}

void ResourceStateTracker::UnregisterCurrentThreadCL()
{
    tlCurrentKey_ = nullptr;
}

const ResourceStateTracker::CLState* ResourceStateTracker::FindCLStateForCmd(ID3D12CommandList* cmd) const
{
    const CLState* result = nullptr;
    for (const auto& lane : clLanes_) {
        auto found = lane->entries.find(cmd);
        if (found != lane->entries.end()) {
#ifdef _DEBUG
            // A CL recorded through two lanes means two threads recorded it —
            // its state would be split between them. Keep scanning to detect.
            assert(result == nullptr && "command list recorded through more than one thread lane");
#endif
            if (result == nullptr) {
                result = &found->second;
#ifndef _DEBUG
                break;
#endif
            }
        }
    }
    return result;
}

void ResourceStateTracker::ApplyFinalStates(const CLState& st)
{
    for (auto& kv : st.current) {
        knownStates_[kv.first] = kv.second;
    }
}

void ResourceStateTracker::AppendAcquireBarriers(const CLState& st, std::vector<D3D12_RESOURCE_BARRIER>& out)
{
    if (st.firstUse.empty()) {
        return;
    }
    out.reserve(out.size() + st.firstUse.size());
    for (auto& kv : st.firstUse) {
        ID3D12Resource* res = kv.first;
        const D3D12_RESOURCE_STATES want = kv.second;

        D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON;
        if (auto ig = knownStates_.find(res); ig != knownStates_.end()) {
            before = ig->second;
        }

        if (before != want) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = res;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = want;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            out.push_back(b);
        }

        knownStates_[res] = want;
    }
}

void ResourceStateTracker::SetKnownStateDirect(ID3D12Resource* res, D3D12_RESOURCE_STATES state)
{
    if (res) {
        knownStates_[res] = state;
    }
}

void ResourceStateTracker::ResetLanesForFrame()
{
    for (auto& lane : clLanes_) {
#ifdef _DEBUG
        assert(lane->activeRecordings.load(std::memory_order_acquire) == 0 &&
            "ResetLanesForFrame while a thread is still recording");
#endif
        lane->entries.clear();
        lane->epoch++;
    }
}
