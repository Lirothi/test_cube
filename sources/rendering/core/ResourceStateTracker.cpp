#include "rendering/core/ResourceStateTracker.h"

#include <algorithm>

thread_local uint32_t ResourceStateTracker::tlLaneIndex_ = UINT32_MAX;
thread_local ResourceStateTracker::CLStateEntry* ResourceStateTracker::tlCurrentEntry_ = nullptr;

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
    ID3D12CommandList* base = static_cast<ID3D12CommandList*>(cl);

    // Fast path — the active command list is stored in TLS
    CLStateEntry* entry = tlCurrentEntry_;
    const uint32_t lane = tlLaneIndex_;
    if (entry == nullptr || lane == UINT32_MAX ||
        entry->epoch != clLanes_[lane].epoch || entry->cmd != base) {
        if (lane != UINT32_MAX) {
            CLStateLane& ln = clLanes_[lane];
            auto found = ln.entries.find(base);
            if (found != ln.entries.end()) {
                entry = &found->second;
                entry->epoch = ln.epoch;
                tlCurrentEntry_ = entry;
            } else {
                RegisterCurrentThreadCL(cl);
                entry = tlCurrentEntry_;
            }
        } else {
            // Command list not yet registered on this thread — register it on the fly
            RegisterCurrentThreadCL(cl);
            entry = tlCurrentEntry_;
        }
    }

    if (!entry) {
        return;
    }
    auto& st = entry->st;

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

void ResourceStateTracker::RegisterCurrentThreadCL(ID3D12GraphicsCommandList* cl)
{
    uint32_t lane = tlLaneIndex_;
    if (lane == UINT32_MAX) {
        lane = clLaneCount_.fetch_add(1, std::memory_order_relaxed);
        if (lane >= kCLStateLanes) { lane = kCLStateLanes - 1; }
        tlLaneIndex_ = lane;
    }
    CLStateLane& ln = clLanes_[lane];
    if (ln.entries.size() == 0)
    {
        ln.entries.reserve(32);
    }
    CLStateEntry& e = ln.entries[static_cast<ID3D12CommandList*>(cl)];
    e.cmd = static_cast<ID3D12CommandList*>(cl);
    e.st.firstUse.clear(); e.st.firstUse.reserve(8);
    e.st.current.clear(); e.st.current.reserve(16);
    e.epoch = ln.epoch;
    tlCurrentEntry_ = &e;
}

void ResourceStateTracker::UnregisterCurrentThreadCL()
{
    tlCurrentEntry_ = nullptr;
}

const ResourceStateTracker::CLState* ResourceStateTracker::FindCLStateForCmd(ID3D12CommandList* cmd) const
{
    const uint32_t lanes = clLaneCount_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < std::min<uint32_t>(lanes, kCLStateLanes); ++i) {
        auto found = clLanes_[i].entries.find(cmd);
        if (found != clLanes_[i].entries.end()) {
            return &found->second.st;
        }
    }
    return nullptr;
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
    const uint32_t lanes = clLaneCount_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < std::min<uint32_t>(lanes, kCLStateLanes); ++i) {
        clLanes_[i].entries.clear();
        clLanes_[i].epoch++;
    }
}
