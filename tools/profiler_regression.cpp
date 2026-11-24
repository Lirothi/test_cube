#include "core/profiling/Profiler.h"
#include "core/logging/Log.h"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <atomic>
#include <barrier>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "third_party/json/json.hpp"

// ThreadRole.cpp's one scope name. ProfilerScopes.cpp, which defines it in the game, brings the text and input layers.
namespace ProfilerScopes {
extern const Profiler::ScopeNameKey kGameParkRender = Profiler::RegisterTraceLiteral(L"Game: Park Render");
}

struct ProfilerTestAccess {
    static void Join(Profiler& p) { TaskSystem::Get().Wait(p.overlayTask_); }
    static void Sample(Profiler& p, Profiler::ScopeNameKey key, double ms) {
        const auto start = Profiler::CpuClock::now();
        p.PushSample(key, start, start + std::chrono::duration_cast<Profiler::CpuClock::duration>(std::chrono::duration<double, std::milli>(ms)), p.sampleEpoch_.load());
    }
    static void GpuSample(Profiler& p, Profiler::ScopeNameKey key, double ms, uint64_t frame) {
        p.gpuFrameSamples_.push_back({ key, ms, frame });
    }
    static bool HasFormattedSnapshot(Profiler& p) {
        std::lock_guard<std::mutex> lock(p.snapshotMutex_);
        for (const auto& row : p.overlayRows_) { if (!row.formatted.empty()) { return true; } }
        for (const auto& row : p.gpuOverlayRows_) { if (!row.formatted.empty()) { return true; } }
        return false;
    }
    static void FlushWriter(Profiler& p) { p.StopTraceWriter(); }
    static UINT QueryCount(Profiler& p) { return p.nextGpuQuery_.load(); }
    static void SetTraceEventLimit(Profiler& p, size_t limit) { std::lock_guard<std::mutex> lock(p.mtx_); p.traceEventLimit_ = limit; }
    static size_t DefaultTraceEventLimit() { return Profiler::kTraceEventLimit; }
    static size_t TraceFilesKept() { return Profiler::kTraceFilesKept; }
};

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    if (ok) { LOG_INFO(logging::LogCategory::Profiling, "profiler regression PASS: {}", name); }
    else { ++failures; LOG_ERROR(logging::LogCategory::Profiling, "profiler regression FAIL: {}", name); }
}
Profiler::OverlaySample Find(Profiler& p, const std::string& name, bool gpu = false) {
    for (const auto& row : p.OverlaySnapshot()) { if (row.name == name && row.gpu == gpu) { return row; } }
    throw std::runtime_error("missing profiler row: " + name);
}
void End(Profiler& p) { p.EndFrame(); ProfilerTestAccess::Join(p); }
bool Near(double a, double b) { return std::abs(a - b) < 1e-6; }

void CpuTests(Profiler& p) {
    const auto key = Profiler::RegisterTraceLiteral(L"Regression.Concurrent");
    p.SetMaxCooldownSeconds(3600);
    std::atomic<bool> stop{ false };
    std::thread reader([&]() {
        while (!stop.load()) { p.ResetMaxNow(); (void)p.OverlaySnapshot(); std::this_thread::yield(); }
    });
    bool exact = true;
    for (uint64_t frame = 1; frame <= 80; ++frame) {
        p.BeginFrame(frame);
        TaskSystem::Get().DispatchWait(8, [&](size_t) {
            for (int i = 0; i < 2000; ++i) { ProfilerTestAccess::Sample(p, key, 0.001); }
        }, 1);
        End(p);
        const auto row = Find(p, "Regression.Concurrent");
        exact = exact && row.usages == 16000 && Near(row.avgMs, 16.0) && Near(row.maxMs, 16.0);
    }
    stop.store(true);
    reader.join();
    Check(exact, "parallel sample counts, frame totals, concurrent reset/snapshot (1.28M samples)");
    Check(!ProfilerTestAccess::HasFormattedSnapshot(p), "hidden numeric snapshots never format HUD strings");

    const auto peak = Profiler::RegisterTraceLiteral(L"Regression.Peak");
    p.BeginFrame(100);
    for (int i = 0; i < 100; ++i) { ProfilerTestAccess::Sample(p, peak, 0.01); }
    End(p);
    Check(Near(Find(p, "Regression.Peak").maxMs, 1.0), "max measures total frame cost, not one invocation");
    p.ResetMaxNow();
    p.BeginFrame(101);
    ProfilerTestAccess::Sample(p, peak, 0.02);
    End(p);
    Check(Near(Find(p, "Regression.Peak").maxMs, 0.02), "reset does not seed max from historical EMA");

    const auto late = Profiler::RegisterTraceLiteral(L"Regression.Late");
    p.BeginFrame(102);
    auto scope = std::make_unique<Profiler::ScopedCpu>(late);
    End(p);
    p.BeginFrame(103);
    scope.reset();
    ProfilerTestAccess::Sample(p, late, 0.25);
    End(p);
    Check(Find(p, "Regression.Late").usages == 1, "scope from previous epoch cannot leak into next frame");

    const auto gpu = Profiler::RegisterTraceLiteral(L"Regression.DelayedGpu");
    p.BeginFrame(104);
    ProfilerTestAccess::GpuSample(p, gpu, 2.0, 98);
    ProfilerTestAccess::GpuSample(p, gpu, 2.0, 99);
    End(p);
    const auto row = Find(p, "Regression.DelayedGpu", true);
    Check(Near(row.avgMs, 2.0) && Near(row.maxMs, 2.0) && row.usages == 1, "delayed GPU frames are committed individually");

    const auto longName = Profiler::RegisterTraceLiteral(L"Regression.Scope name longer than forty characters.\u041f\u0440\u043e\u0444\u0438\u043b\u044c");
    p.BeginFrame(105); ProfilerTestAccess::Sample(p, longName, 1.0); End(p);
    Check(Find(p, "Regression.Scope name longer than forty characters.Профиль").usages == 1, "snapshot preserves full UTF-8 names");
}

// Architecture review 10.16 p. 2: CPU_SCOPE / GPU_SCOPE pasted the token __LINE__ itself, so every scope was named
// _prof_scope___LINE__ and two in one block did not compile. This function IS the check: it builds or it does not.
void TwoScopesInOneBlock(Profiler& p) {
    const auto a = Profiler::RegisterTraceLiteral(L"Regression.Block.A");
    const auto b = Profiler::RegisterTraceLiteral(L"Regression.Block.B");
    p.BeginFrame(300);
    {
        CPU_SCOPE(a);
        CPU_SCOPE(b);
        CPU_SCOPE_N(a, first); CPU_SCOPE_N(b, second);
    }
    End(p);
    Check(Find(p, "Regression.Block.A").usages == 2 && Find(p, "Regression.Block.B").usages == 2,
        "two CPU_SCOPEs in one block, and two CPU_SCOPE_Ns on one line, compile and are both counted");
}
void TwoGpuScopesInOneBlock(ID3D12GraphicsCommandList* cl, Profiler::ScopeNameKey key) {
    GPU_SCOPE(cl, key);
    GPU_SCOPE(cl, key);
    GPU_SCOPE_N(cl, key, first); GPU_SCOPE_N(cl, key, second);
}

size_t CompleteEvents(const std::string& path) {
    std::ifstream input(path);
    if (!input) { return SIZE_MAX; }
    const auto trace = nlohmann::json::parse(input);
    size_t complete = 0;
    for (const auto& e : trace["traceEvents"]) { complete += e.value("ph", "") == "X" ? 1u : 0u; }
    return complete;
}

// Architecture review 10.16 p. 4: a recording grew without end. Past its event limit it stops as Stop would (a
// Warning says so), and the file is written with what it holds.
void TraceStopsAtItsLimit(Profiler& p) {
    const auto key = Profiler::RegisterTraceLiteral(L"Regression.Limit");
    constexpr size_t limit = 500;
    ProfilerTestAccess::SetTraceEventLimit(p, limit);
    p.BeginTraceCapture();
    for (uint64_t frame = 310; frame < 320; ++frame) {
        p.BeginFrame(frame);
        for (int i = 0; i < 200; ++i) { ProfilerTestAccess::Sample(p, key, 0.001); }
        End(p);
    }
    const auto during = p.GetTraceCaptureStatus();
    if (during.active) { p.StopTraceCapture(); p.BeginFrame(320); End(p); } // the code before the limit: end it here
    ProfilerTestAccess::FlushWriter(p);
    ProfilerTestAccess::SetTraceEventLimit(p, ProfilerTestAccess::DefaultTraceEventLimit());
    const auto status = p.GetTraceCaptureStatus();
    const size_t complete = status.lastPath.empty() ? SIZE_MAX : CompleteEvents(status.lastPath);
    LOG_INFO(logging::LogCategory::Profiling, "limit {}: after 10 frames active={} recorded={} dropped={} file={} ({} events)",
        limit, during.active, during.framesRecorded, status.droppedEvents, status.lastPath, complete);
    Check(!during.active && during.framesRecorded == 3 && status.droppedEvents > 0 && complete <= limit && complete > limit - 210,
        "a recording past its event limit stops at that frame and its file is written with what it holds");
}

// Architecture review 10.16 p. 4: traces/ kept every trace. A write keeps the newest kTraceFilesKept written traces;
// a trace the owner named is never pruned.
void TracesKeepTheNewestTen(Profiler& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all("traces", ec);
    fs::create_directory("traces");
    std::ofstream("traces/trace_named_by_hand.json") << "{}\n";
    const auto key = Profiler::RegisterTraceLiteral(L"Regression.Retention");
    const size_t kept = ProfilerTestAccess::TraceFilesKept();
    std::vector<std::string> written;
    for (uint64_t i = 0; i <= kept; ++i) {
        p.RequestTraceCapture(1);
        p.BeginFrame(330 + i);
        ProfilerTestAccess::Sample(p, key, 0.001);
        End(p);
        ProfilerTestAccess::FlushWriter(p);
        written.push_back(p.GetTraceCaptureStatus().lastPath);
    }
    bool distinct = true;
    for (size_t i = 1; i < written.size(); ++i) { distinct = distinct && written[i] != written[i - 1] && !written[i].empty(); }
    bool newestKept = true;
    for (size_t i = 1; i < written.size(); ++i) { newestKept = newestKept && fs::exists(written[i]); }
    size_t traces = 0;
    for (const auto& entry : fs::directory_iterator("traces")) { traces += entry.path().extension() == ".json" ? 1u : 0u; }
    LOG_INFO(logging::LogCategory::Profiling, "retention: {} traces written, the first {} {}, {} json files in traces/",
        written.size(), written.front(), fs::exists(written.front()) ? "is still there" : "is gone", traces);
    Check(distinct && !fs::exists(written.front()) && newestKept && fs::exists("traces/trace_named_by_hand.json")
        && traces == kept + 1, "the eleventh trace deletes the first; ten written and the hand-named one stay");
}

void Hr(HRESULT result) { if (FAILED(result)) { throw std::runtime_error("D3D12 harness operation failed"); } }
using Microsoft::WRL::ComPtr;
struct Commands {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Commands(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type) {
        Hr(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)));
        Hr(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    }
};
void WaitQueue(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence;
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    Hr(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    Hr(fence->SetEventOnCompletion(1, event));
    const DWORD waited = WaitForSingleObject(event, 10000);
    CloseHandle(event);
    if (waited != WAIT_OBJECT_0) { throw std::runtime_error("GPU queue drain timed out"); }
}
void GpuTests(Profiler& p) {
    ComPtr<ID3D12Debug1> debug;
    Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    debug->EnableDebugLayer();
    debug->SetEnableGPUBasedValidation(TRUE);
    ComPtr<IDXGIFactory4> factory;
    Hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device;
    Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages;
    Hr(device.As(&messages));
    ComPtr<ID3D12CommandQueue> direct, compute;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&direct)));
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&compute)));
    p.InitGpu(device.Get(), direct.Get(), compute.Get(), 16);
    const auto outer = Profiler::RegisterTraceLiteral(L"Regression.GpuOuter");
    const auto inner = Profiler::RegisterTraceLiteral(L"Regression.GpuInner");
    const auto async = Profiler::RegisterTraceLiteral(L"Regression.GpuCompute");
    ComPtr<ID3D12Fence> gate;
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    Hr(direct->Wait(gate.Get(), 1));
    Hr(compute->Wait(gate.Get(), 1));
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool release = false;
    // A watchdog releases the intentionally blocked GPU even if Stop regresses
    // to a synchronous drain, turning a deadlock into a bounded test failure.
    std::thread watchdog([&]() {
        std::unique_lock<std::mutex> lock(gateMutex);
        gateCv.wait_for(lock, std::chrono::seconds(2), [&]() { return release; });
        gate->Signal(1);
    });
    std::vector<std::unique_ptr<Commands>> commands;
    p.BeginTraceCapture();
    const auto started = Profiler::CpuClock::now();
    for (uint64_t frame = 200; frame < 210; ++frame) {
        p.BeginFrame(frame);
        p.CollectGpuResults();
        auto begin = std::make_unique<Commands>(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto work = std::make_unique<Commands>(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto comp = std::make_unique<Commands>(device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE);
        auto end = std::make_unique<Commands>(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        p.BeginGpuFrame(begin->list.Get());
        std::barrier rendezvous(2);
        std::thread a([&]() {
            Profiler::ScopedGpu parent(work->list.Get(), outer);
            rendezvous.arrive_and_wait();
            Profiler::ScopedGpu child(work->list.Get(), inner);
        });
        std::thread b([&]() {
            Profiler::ScopedGpu parent(comp->list.Get(), async);
            rendezvous.arrive_and_wait();
            Profiler::ScopedGpu child(comp->list.Get(), inner);
        });
        a.join(); b.join();
        p.EndGpuFrame(end->list.Get());
        Hr(begin->list->Close()); Hr(work->list->Close()); Hr(comp->list->Close()); Hr(end->list->Close());
        ID3D12CommandList* graphics[] = { begin->list.Get(), work->list.Get(), end->list.Get() };
        ID3D12CommandList* computing[] = { comp->list.Get() };
        direct->ExecuteCommandLists(3, graphics);
        compute->ExecuteCommandLists(1, computing);
        commands.push_back(std::move(begin)); commands.push_back(std::move(work));
        commands.push_back(std::move(comp)); commands.push_back(std::move(end));
        if (frame == 209) { p.StopTraceCapture(); }
        End(p);
    }
    const double stopMs = std::chrono::duration<double, std::milli>(Profiler::CpuClock::now() - started).count();
    Check(stopMs < 1000, "GPU lag and trace Stop never wait for queue completion");
    Check(p.GetTraceCaptureStatus().pending, "capture remains pending until final GPU batches arrive");
    { std::lock_guard<std::mutex> lock(gateMutex); release = true; }
    gateCv.notify_one(); watchdog.join();
    WaitQueue(device.Get(), direct.Get()); WaitQueue(device.Get(), compute.Get());
    p.BeginFrame(210); p.CollectGpuResults(); End(p);
    ProfilerTestAccess::FlushWriter(p);
    const auto status = p.GetTraceCaptureStatus();
    Check(!status.pending && !status.lastPath.empty(), "drained capture is exported");
    std::ifstream input(status.lastPath);
    const auto trace = nlohmann::json::parse(input);
    size_t frames = 0, gpuFrames = 0, computeEvents = 0;
    bool validFrames = true;
    for (const auto& e : trace["traceEvents"]) {
        if (e.value("ph", "") != "X") { continue; }
        const std::string name = e.value("name", "");
        if (name.starts_with("Frame ")) { ++frames; }
        if (name.starts_with("GPU.Frame ")) { ++gpuFrames; }
        if (e.value("cat", "") == "GPU") {
            const auto frame = e["args"]["frame"].get<uint64_t>();
            validFrames = validFrames && frame >= 200 && frame < 208;
            if (e.value("tid", 0) == 1) { ++computeEvents; }
        }
    }
    Check(frames == 10 && gpuFrames == 8 && computeEvents == 16 && validFrames,
        "ring exhaustion skips new samples without overwriting 8 unread frames; both GPU queues retained");
    Check(Find(p, "Regression.GpuOuter", true).usages == 1, "8 ready GPU batches do not multiply per-frame usages");

    p.BeginFrame(211);
    Commands overflow(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    p.SetEnabled(false);
    { Profiler::ScopedGpu disabled(overflow.list.Get(), inner); }
    TwoGpuScopesInOneBlock(overflow.list.Get(), inner);
    Check(ProfilerTestAccess::QueryCount(p) == 0, "disabled GPU profiling reserves no queries");
    p.SetEnabled(true);
    {
        Profiler::ScopedGpu root(overflow.list.Get(), outer);
        for (int i = 0; i < 24; ++i) { Profiler::ScopedGpu nested(overflow.list.Get(), inner); }
    }
    Hr(overflow.list->Close());
    ID3D12CommandList* lists[] = { overflow.list.Get() };
    direct->ExecuteCommandLists(1, lists);
    End(p); WaitQueue(device.Get(), direct.Get()); WaitQueue(device.Get(), compute.Get());
    p.BeginFrame(212); p.CollectGpuResults(); End(p);
    Check(Find(p, "Regression.GpuInner", true).usages == 7, "query exhaustion drops excess nested scopes safely");
    p.ShutdownGpu();
    WaitQueue(device.Get(), direct.Get()); WaitQueue(device.Get(), compute.Get());
    size_t findings = 0;
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0; messages->GetMessage(i, nullptr, &bytes);
        std::vector<unsigned char> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Hr(messages->GetMessage(i, message, &bytes));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            ++findings;
            LOG_ERROR(logging::LogCategory::RenderValidation, "profiler GPU validation id={}: {}", message->ID, message->pDescription);
        }
    }
    Check(findings == 0, "D3D12 debug layer + GBV: no warnings or errors");
}
}

int main() {
    logging::LogConfig config;
    config.debuggerSink = false;
    logging::Initialize(config);
    // The traces go to a folder of this run's own: a write deletes older traces, and the repository's traces/ are the
    // owner's.
    namespace fs = std::filesystem;
    const fs::path home = fs::current_path();
    const fs::path scratch = fs::temp_directory_path() / ("profiler_regression_" + std::to_string(GetCurrentProcessId()));
    try {
        fs::create_directories(scratch / "traces");
        fs::current_path(scratch);
        TaskSystem::Get().Start(8);
        auto& profiler = Profiler::Get();
        CpuTests(profiler);
        TwoScopesInOneBlock(profiler);
        TraceStopsAtItsLimit(profiler);
        TracesKeepTheNewestTen(profiler);
        GpuTests(profiler);
        ProfilerTestAccess::FlushWriter(profiler);
        TaskSystem::Get().Stop();
    } catch (const std::exception& e) {
        ++failures;
        LOG_ERROR(logging::LogCategory::Profiling, "profiler regression exception: {}", e.what());
        TaskSystem::Get().Stop();
    }
    std::error_code ec;
    fs::current_path(home, ec);
    fs::remove_all(scratch, ec);
    LOG_INFO(logging::LogCategory::Profiling, "profiler regression: {} ({} failures)", failures ? "FAIL" : "PASS", failures);
    logging::Shutdown();
    return failures ? 1 : 0;
}
