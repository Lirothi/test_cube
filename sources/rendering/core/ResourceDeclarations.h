#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include "third_party/robin_hood.h"

// Barrier plan step 6 (design D2).
//
// Every graph resource has a CANONICAL (resting) state: the state it is created in, and the
// state every frame must both begin and end with it in. That is exactly what the engine's
// ~45 creation-time `SetResourceState` calls always passed — R2 established that those calls
// were never tracking, only seeding.
//
// Once the invariant holds, the compiled barriers can seed from this static table and the
// live cross-frame map inside ResourceStateTracker became dead weight — and at Step 7 that class
// was deleted outright. This table outlived it, exactly as keeping it out of that class intended.
class CanonicalStateRegistry
{
public:
    struct Entry {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        // Step 7: where the LAST barrier compile left this resource. The compile seeds a frame
        // from here, not from `state`.
        //
        // Seeding from the canonical state assumed every frame ENDS at canonical. That holds only
        // while the graph keeps its shape: Pass_Tonemap returns the depth and velocity buffers to
        // their resting state as part of the DLSS branch, so the frames where DLSS is off leave
        // them at DEPTH_WRITE / RENDER_TARGET and the next frame's GBuffer barrier claimed a
        // before-state of NON_PIXEL_SHADER_RESOURCE (D3D12 error 527, measured across every
        // render-resolution change the stress harness makes).
        //
        // This is NOT the tracker coming back: one value per resource, written once per frame by
        // the single-threaded compile, with no per-command-list lanes, no first-use bookkeeping
        // and no submit-time stitching. Declare resets it, so a resource created at a recycled
        // address starts from its own resting state rather than the previous owner's.
        D3D12_RESOURCE_STATES predicted = D3D12_RESOURCE_STATE_COMMON;
        // Captured HERE, at declaration, because that is the only moment the pointer is
        // guaranteed live. Not every release path unregisters (measured — see the plan's
        // Step 6 result), so the table can hold dangling keys; reading a name off one later
        // is an access violation, which is exactly how this was found.
        char name[96] = {};
        // Step 7: this resource's state is driven from OUTSIDE the render graph (the swapchain
        // backbuffer: RecordBindAndClear and the present epilogue write it with hand-rolled
        // barriers, and uploads/ImGui touch others). The compile cannot model those, so it must
        // not compile barriers for them and Transition must keep routing them to the tracker —
        // otherwise the compiled before-state is guesswork against code that never told us.
        bool unmanaged = false;
    };

    // Last write wins rather than first: a resource recreated at a recycled address must not
    // inherit the previous owner's resting state. That bug class has bitten this engine before.
    void Declare(ID3D12Resource* res, D3D12_RESOURCE_STATES state)
    {
        if (res == nullptr) { return; }
        Entry e;
        e.state = state;
        e.predicted = state; // a freshly created resource IS in its resting state
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

    // Mark a declared resource as driven from outside the graph (see Entry::unmanaged).
    void SetUnmanaged(ID3D12Resource* res)
    {
        if (res == nullptr) { return; }
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        if (it != states_.end()) { it->second.unmanaged = true; }
    }

    // Is this resource the barrier compile's business at all? It must be DECLARED (so the
    // compile seeded a canonical state for it) and not explicitly excluded. An UNDECLARED
    // resource — an editor/ImGui texture, an upload buffer — was never modelled, so its
    // transitions must keep going to the tracker.
    bool IsCompileManaged(ID3D12Resource* res) const
    {
        if (res == nullptr) { return false; }
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        return it != states_.end() && !it->second.unmanaged;
    }

    bool IsUnmanaged(ID3D12Resource* res) const
    {
        if (res == nullptr) { return false; }
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        return it != states_.end() && it->second.unmanaged;
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

    // The name captured at Declare time, copied out. Used by --barrier-flip-trace, which runs
    // while command lists are recording: reading the name off the resource there would mean a
    // GetPrivateData call per miss, and the pointer may already be dangling.
    void NameOf(ID3D12Resource* res, char* out, size_t cap) const
    {
        if (out == nullptr || cap == 0) { return; }
        out[0] = '\0';
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        if (it == states_.end()) { std::snprintf(out, cap, "%p", static_cast<const void*>(res)); return; }
        std::snprintf(out, cap, "%s", it->second.name);
    }

    // Where the last compile left this resource (see Entry::predicted). Falls back to the
    // canonical state, which is also what a never-declared resource reports.
    D3D12_RESOURCE_STATES GetPredicted(ID3D12Resource* res) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        return (it == states_.end()) ? D3D12_RESOURCE_STATE_COMMON : it->second.predicted;
    }

    void SetPredicted(ID3D12Resource* res, D3D12_RESOURCE_STATES state)
    {
        if (res == nullptr) { return; }
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = states_.find(res);
        if (it != states_.end()) { it->second.predicted = state; }
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

// The sink a creation site declares through. It used to feed BOTH the canonical registry and the
// tracker's live map; at Step 7 the `tracker` member went away and — as that split was designed to
// guarantee — not one of the ~60 call sites changed. Passed by value.
struct ResourceDeclarations
{
    CanonicalStateRegistry* canonical = nullptr;

    // Creation state doubles as the resting state. Correct for the ~75 resources measured to
    // already end the frame where they started.
    void Declare(ID3D12Resource* res, D3D12_RESOURCE_STATES creationState) const
    {
        Declare(res, creationState, creationState);
    }

    // Step 6b kept these as two separate facts because the tracker had to be seeded with where the
    // resource actually IS. Step 7 aligned them (resources are now created directly in their
    // resting state) and then deleted the tracker, so `creationState` has no consumer left — the
    // overload survives for the upload-initialised resources whose sites still state both, and
    // where the two genuinely differ (COPY_DEST at creation, canonical after the upload's own
    // final barrier).
    void Declare(ID3D12Resource* res, D3D12_RESOURCE_STATES creationState,
                 D3D12_RESOURCE_STATES canonicalState) const
    {
        (void)creationState;
        if (canonical) { canonical->Declare(res, canonicalState); }
    }

    void Forget(ID3D12Resource* res) const
    {
        if (canonical) { canonical->Forget(res); }
    }

    void ForgetMany(const std::vector<ID3D12Resource*>& resources) const
    {
        if (canonical) { canonical->ForgetMany(resources); }
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
