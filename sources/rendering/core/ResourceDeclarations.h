#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include "rendering/core/ResourceStateTracker.h"
#include "third_party/robin_hood.h"

// Barrier plan step 6 (design D2).
//
// Every graph resource has a CANONICAL (resting) state: the state it is created in, and the
// state every frame must both begin and end with it in. That is exactly what the engine's
// ~45 creation-time `SetResourceState` calls always passed — R2 established that those calls
// were never tracking, only seeding.
//
// Once the invariant holds, the compiled barriers can seed from this static table and the
// live cross-frame map inside ResourceStateTracker becomes dead weight. So this table is
// deliberately NOT a member of that class: it has to outlive it.
class CanonicalStateRegistry
{
public:
    struct Entry {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        // Captured HERE, at declaration, because that is the only moment the pointer is
        // guaranteed live. Not every release path unregisters (measured — see the plan's
        // Step 6 result), so the table can hold dangling keys; reading a name off one later
        // is an access violation, which is exactly how this was found.
        char name[96] = {};
    };

    // Last write wins rather than first: a resource recreated at a recycled address must not
    // inherit the previous owner's resting state. That bug class has bitten this engine before.
    void Declare(ID3D12Resource* res, D3D12_RESOURCE_STATES state)
    {
        if (res == nullptr) { return; }
        Entry e;
        e.state = state;
        // 256, not 64: GetPrivateData returns DXGI_ERROR_MORE_DATA when the name does not fit and
        // the D3D12 debug layer RAISES on that — a Debug-only crash Release runs can never show.
        // Texture names carry a full asset path, which is what overflowed the old buffer.
        wchar_t wide[256] = {};
        UINT bytes = sizeof(wide) - sizeof(wchar_t);
        if (SUCCEEDED(res->GetPrivateData(WKPDID_D3DDebugObjectNameW, &bytes, wide)) && wide[0]) {
            std::snprintf(e.name, sizeof(e.name), "%ls", wide);
        }
        else {
            std::snprintf(e.name, sizeof(e.name), "%p", static_cast<const void*>(res));
        }
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        if (it != states_.end()) { --net_[it->second.name]; } // overwrite = the old one is gone
        ++net_[e.name];
        states_[res] = e;
    }

    void Forget(ID3D12Resource* res)
    {
        if (res == nullptr) { return; }
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        if (it == states_.end()) { return; }
        --net_[it->second.name];
        states_.erase(it);
    }

    void ForgetMany(const std::vector<ID3D12Resource*>& resources)
    {
        if (resources.empty()) { return; }
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto* res : resources) { states_.erase(res); }
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        states_.clear();
        net_.clear();
    }

    // Re-read the debug name of an ALREADY-DECLARED resource. Owners that name their resources
    // after declaring them (RenderTargetManager names the whole deferred set in one block at the
    // end of Create) would otherwise be logged as raw pointers, which makes the off-canonical
    // report unactionable. THE CALLER GUARANTEES `res` IS ALIVE — that is the whole reason the
    // name is not simply read at report time.
    void RefreshName(ID3D12Resource* res)
    {
        if (res == nullptr) { return; }
        // 256, not 64: GetPrivateData returns DXGI_ERROR_MORE_DATA when the name does not fit and
        // the D3D12 debug layer RAISES on that — a Debug-only crash Release runs can never show.
        // Texture names carry a full asset path, which is what overflowed the old buffer.
        wchar_t wide[256] = {};
        UINT bytes = sizeof(wide) - sizeof(wchar_t);
        if (FAILED(res->GetPrivateData(WKPDID_D3DDebugObjectNameW, &bytes, wide)) || !wide[0]) { return; }
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        if (it == states_.end()) { return; }
        std::snprintf(it->second.name, sizeof(it->second.name), "%ls", wide);
    }

    // COMMON for a resource that never declared — the same answer the tracker gives, so an
    // undeclared resource reads as "matches" rather than as a spurious drift report.
    D3D12_RESOURCE_STATES Get(ID3D12Resource* res) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        return (it == states_.end()) ? D3D12_RESOURCE_STATE_COMMON : it->second.state;
    }

    // Copied out rather than exposing the map, so callers cannot hold the lock across the
    // tracker's own lock (the frame-end check needs both).
    void Snapshot(std::vector<std::pair<ID3D12Resource*, Entry>>& out) const
    {
        out.clear();
        std::lock_guard<std::mutex> lk(mtx_);
        out.reserve(states_.size());
        for (const auto& kv : states_) { out.emplace_back(kv.first, kv.second); }
    }

    // Step 6b: how many live entries each NAME currently accounts for (declares minus forgets).
    //
    // This is what distinguishes a leak from legitimate duplicates, without ever needing to know
    // whether a resource is still alive — which the registry cannot know. Two objects that share a
    // name (two GPU-instanced clouds, a texture loaded twice) hold their net at a STEADY value; an
    // owner that never unregisters makes it climb with every churn cycle. Counting bare duplicates
    // cannot tell those apart, and twice led me to call a steady ×2 a leak.
    void NetByName(std::vector<std::pair<std::string, int>>& out) const
    {
        out.clear();
        std::lock_guard<std::mutex> lk(mtx_);
        out.reserve(net_.size());
        for (const auto& kv : net_) { if (kv.second != 0) { out.emplace_back(kv.first, kv.second); } }
    }

private:
    // Touched only at resource creation and destruction, so the mutex is uncontended; it
    // exists because texture loads can declare off the main thread.
    mutable std::mutex mtx_;
    robin_hood::unordered_map<ID3D12Resource*, Entry> states_;
    robin_hood::unordered_map<std::string, int> net_;
};

// The sink a creation site declares through. Today it feeds BOTH the canonical registry and
// the tracker's live map; at Step 7 the `tracker` member goes away and every call site written
// against this interface stays exactly as it is. Passed by value — two references.
struct ResourceDeclarations
{
    ResourceStateTracker*   tracker = nullptr;
    CanonicalStateRegistry* canonical = nullptr;

    // Creation state doubles as the resting state. Correct for the ~75 resources measured to
    // already end the frame where they started.
    void Declare(ID3D12Resource* res, D3D12_RESOURCE_STATES creationState) const
    {
        Declare(res, creationState, creationState);
    }

    // Step 6b: the two states are NOT the same thing, and for most resources they differ.
    // `creationState` seeds the tracker, because that is where the resource actually IS and
    // the first barrier's before-state depends on it. `canonicalState` is where the frame must
    // LEAVE it — the measured resting state, which is what Step 7's compile will seed from.
    // Aligning the two (creating resources directly in canonical) belongs to Step 7, where the
    // compile starts reading this table; doing it here would change real frame-1 GPU behaviour
    // for no gain.
    void Declare(ID3D12Resource* res, D3D12_RESOURCE_STATES creationState,
                 D3D12_RESOURCE_STATES canonicalState) const
    {
        if (canonical) { canonical->Declare(res, canonicalState); }
        if (tracker) { tracker->SetResourceState(res, creationState); }
    }

    void Forget(ID3D12Resource* res) const
    {
        if (canonical) { canonical->Forget(res); }
        if (tracker) { tracker->ClearResourceState(res); }
    }

    void ForgetMany(const std::vector<ID3D12Resource*>& resources) const
    {
        if (canonical) { canonical->ForgetMany(resources); }
        if (tracker) { tracker->ForgetResources(resources); }
    }

    // See CanonicalStateRegistry::RefreshName — caller guarantees the resource is alive.
    void RefreshName(ID3D12Resource* res) const
    {
        if (canonical) { canonical->RefreshName(res); }
    }

    // Override the resting state of an already-declared resource WITHOUT re-seeding the tracker.
    // For owners whose creation helper is shared across targets that rest in different states
    // (the deferred targets are all created by four generic helpers but rest in five different
    // states), so the resting state is stated per target rather than per helper.
    void SetCanonical(ID3D12Resource* res, D3D12_RESOURCE_STATES canonicalState) const
    {
        if (canonical) { canonical->Declare(res, canonicalState); }
    }
};

// Barrier plan step 6b part 2 — the resource wrapper.
//
// Owns the resource AND its registration. Declaring on attach and forgetting in the destructor
// is the whole point: the hand-maintained unregister lists were silently incomplete (measured —
// `OceanSimulation::RetireGpuResources` clears five of its resources and misses `shoreDepth_`),
// and a stale entry lets a recycled `ID3D12Resource*` inherit someone else's canonical state.
//
// Deliberately holds a ResourceDeclarations (two pointers), not a Renderer* — Renderer.h includes
// this header, so depending on Renderer here would be circular.
//
// Shaped for minimal disruption at the ~60 existing sites: create into a local ComPtr exactly as
// today, then Attach. Everything else keeps calling Get().
class GpuResource
{
public:
    GpuResource() = default;
    ~GpuResource() { Reset(); }

    GpuResource(const GpuResource&) = delete;
    GpuResource& operator=(const GpuResource&) = delete;

    GpuResource(GpuResource&& other) noexcept { *this = std::move(other); }
    GpuResource& operator=(GpuResource&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            res_ = std::move(other.res_);
            decls_ = other.decls_;
            other.res_.Reset();
            other.decls_ = ResourceDeclarations{};
        }
        return *this;
    }

    // `name` is set BEFORE declaring so the canonical registry captures a readable name rather
    // than an address — the registry cannot re-read it later, because by report time the resource
    // may already be gone.
    void Attach(ResourceDeclarations decls, Microsoft::WRL::ComPtr<ID3D12Resource> res,
                D3D12_RESOURCE_STATES creationState, D3D12_RESOURCE_STATES canonicalState,
                const wchar_t* name)
    {
        Reset();
        res_ = std::move(res);
        decls_ = decls;
        if (!res_) { return; }
        if (name) { res_->SetName(name); }
        decls_.Declare(res_.Get(), creationState, canonicalState);
    }

    // Creation state doubles as the resting state.
    void Attach(ResourceDeclarations decls, Microsoft::WRL::ComPtr<ID3D12Resource> res,
                D3D12_RESOURCE_STATES state, const wchar_t* name)
    {
        Attach(decls, std::move(res), state, state, name);
    }

    void Reset()
    {
        if (res_) { decls_.Forget(res_.Get()); }
        res_.Reset();
        decls_ = ResourceDeclarations{};
    }

    // For the `CreateCommittedResource(..., IID_PPV_ARGS(x.GetAddressOf()))` shape used all over
    // the engine: hand out the raw address (unregistering whatever was there first), then call
    // DeclareCreated once the caller has checked the HRESULT. Two single-line edits per site,
    // versus rewriting each creation block around a temporary.
    ID3D12Resource** GetAddressOfForCreate()
    {
        Reset();
        return res_.GetAddressOf();
    }

    void DeclareCreated(ResourceDeclarations decls, D3D12_RESOURCE_STATES creationState,
                        D3D12_RESOURCE_STATES canonicalState, const wchar_t* name)
    {
        if (!res_) { return; }
        decls_ = decls;
        if (name) { res_->SetName(name); }
        decls_.Declare(res_.Get(), creationState, canonicalState);
    }

    void DeclareCreated(ResourceDeclarations decls, D3D12_RESOURCE_STATES state, const wchar_t* name)
    {
        DeclareCreated(decls, state, state, name);
    }

    ID3D12Resource* Get() const { return res_.Get(); }
    ID3D12Resource* operator->() const { return res_.Get(); }
    explicit operator bool() const { return res_ != nullptr; }
    // `x != nullptr` is how the engine spells this everywhere; without these the explicit bool
    // conversion turns every such predicate into a compile error, which cost three separate
    // rounds of chasing call sites.
    friend bool operator==(const GpuResource& r, std::nullptr_t) { return r.res_ == nullptr; }
    friend bool operator!=(const GpuResource& r, std::nullptr_t) { return r.res_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> res_;
    ResourceDeclarations decls_{};
};
