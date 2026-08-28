#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "third_party/robin_hood.h"
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <optional>
#include <string_view>
#include <cstring>

#include "core/task/TaskSystem.h"

#ifndef PROF_ENABLED
#define PROF_ENABLED 1
#endif

#ifndef PROF_GPU_ENABLED
#define PROF_GPU_ENABLED 1
#endif

#if PROF_GPU_ENABLED
#include <d3d12.h>
#include <wrl/client.h>
#endif

class TextManager; // forward
// CPU profiler: scopes, EMA averaging, cooldown for resetting maxima,
// overlay preparation in EndFrame (double-buffered) and infrequent sorting by average.
class Profiler {
public:
    using CpuClock = std::chrono::steady_clock;

    struct ScopeNameKey {
        const std::wstring* name = nullptr;

        const wchar_t* c_str() const noexcept {
            return name ? name->c_str() : nullptr;
        }
    };

    struct ScopeSample {
        ScopeNameKey key{};
        double       ms = 0.0;
    };

    struct ScopeStats {
        // Inclusive accumulators per frame (sum ms of every scope invocation this frame)
        double   frameMsSum = 0.0;
        uint32_t frameCount = 0;

        // Metrics
        double   avgMs = 0.0;   // EMA
        double   maxMs = 0.0;   // reset by cooldown
        uint32_t lastCount = 0; // how many times the scope appeared last frame

        void Accumulate(double ms) {
            frameMsSum += ms;
            frameCount += 1u;
            if (ms > maxMs) { maxMs = ms; }
        }
        void CommitFrame(double emaAlpha) {
            if (frameCount == 0) { lastCount = 0; return; }
            const double cur = frameMsSum;
            if (avgMs <= 0.0) { avgMs = cur; }
            else { avgMs = avgMs * emaAlpha + cur * (1.0 - emaAlpha); }
            lastCount = frameCount;
            frameMsSum = 0.0;
            frameCount = 0;
        }
    };

    struct StatsEntry {
        ScopeStats   stats;
        std::wstring wideName;
        uint32_t     overlayId = 0;
    };

    struct ScopeNameKeyHash {
        using is_avalanching = void;
        size_t operator()(const ScopeNameKey& key) const noexcept {
            return robin_hood::hash_bytes(&key.name, sizeof(key.name));
        }
    };

    struct ScopeNameKeyEqual {
        bool operator()(const ScopeNameKey& a, const ScopeNameKey& b) const noexcept {
            return a.name == b.name;
        }
    };

    struct TraceEvent {
        enum class Category : uint8_t {
            Cpu,
            Gpu,
        };

        ScopeNameKey key{};
        uint64_t     tsUs = 0;
        uint64_t     durUs = 0;
        uint32_t     threadIndex = 0;
        Category     category = Category::Cpu;
        uint64_t     frameNumber = 0;
        bool         hasFrameNumber = false;
    };

    struct TraceNameEntry {
        std::wstring displayName;
        ScopeNameKey key{};
        bool         isDynamic = false;
        bool         dynamicInUse = false;
    };

    struct TraceDumpData {
        std::vector<TraceEvent> events;
        std::vector<ScopeNameKey> dynamicKeys;
    };

    // --- overlay data (snapshot) ---
    struct OverlayRow {
        uint32_t entryId = 0;
        double   avgMs = 0.0;
        double   maxMs = 0.0;
        uint32_t usages = 0;
        std::wstring formatted; // preformatted string
    };

public:
    static Profiler& Get();

    static ScopeNameKey RegisterTraceLiteral(const wchar_t* name);
    static ScopeNameKey RegisterTraceDynamic(std::wstring name, uint32_t* outIndex = nullptr);

    // Frame boundaries
    void BeginFrame(uint64_t frameNo);
    void EndFrame();

#if PROF_GPU_ENABLED
    // GPU initialization and result collection.
    //
    // Async-compute plan step 3: `computeQueue` may be null (device refused one, or the caller does
    // not use it). Everything the GPU side touches is PER QUEUE — query heap, readback buffer,
    // timestamp frequency, clock calibration and drain fence. The query COUNTER stays shared, which
    // only means slot indices are unique across both heaps. See the members: sharing the readback
    // buffer was the first cut and the debug layer rejected it outright.
    void InitGpu(ID3D12Device* device, ID3D12CommandQueue* queue,
                 ID3D12CommandQueue* computeQueue = nullptr, UINT maxQueries = 1024);
    void CollectGpuResults();
    void BeginGpuFrame(ID3D12GraphicsCommandList* cl);
    void EndGpuFrame(ID3D12GraphicsCommandList* cl);
    void ShutdownGpu();
#endif

    // Scoped marker (CPU)
    class ScopedCpu {
    public:
        ScopedCpu(ScopeNameKey key) {
#if PROF_ENABLED
            key_ = key;
            start_ = Clock::now();
#endif
        }
        ~ScopedCpu() {
#if PROF_ENABLED
            const auto end = Clock::now();
            Profiler::Get().PushSample(key_, start_, end);
#endif
        }
    private:
#if PROF_ENABLED
        using Clock = CpuClock;
        Clock::time_point start_{};
#endif
        ScopeNameKey key_{};
    };

#if PROF_ENABLED
#define CPU_SCOPE(keyExpr)       Profiler::ScopedCpu _prof_scope_##__LINE__(keyExpr)
#define CPU_SCOPE_N(keyExpr, id) Profiler::ScopedCpu _prof_scope_##__LINE__(keyExpr)
#else
#define CPU_SCOPE(keyExpr)       do { } while (0)
#define CPU_SCOPE_N(keyExpr, id) do { } while (0)
#endif

#if PROF_GPU_ENABLED
    // Scoped marker (GPU)
    class ScopedGpu {
    public:
        ScopedGpu(ID3D12GraphicsCommandList* cl, ScopeNameKey key);
        ~ScopedGpu();
    private:
#if PROF_ENABLED
        ID3D12GraphicsCommandList* cl_ = nullptr;
        size_t idx_ = SIZE_MAX;
#endif
    };

#if PROF_ENABLED
#define GPU_SCOPE(cmdList, keyExpr)       Profiler::ScopedGpu _prof_gpu_scope_##__LINE__(cmdList, keyExpr)
#define GPU_SCOPE_N(cmdList, keyExpr, id) Profiler::ScopedGpu _prof_gpu_scope_##__LINE__(cmdList, keyExpr)
#else
#define GPU_SCOPE(cmdList, keyExpr)       do { } while (0)
#define GPU_SCOPE_N(cmdList, keyExpr, id) do { } while (0)
#endif
#else
    class ScopedGpu {
    public:
        ScopedGpu(... ) {}
    };
#define GPU_SCOPE(cmdList, keyExpr)       do { } while (0)
#define GPU_SCOPE_N(cmdList, keyExpr, id) do { } while (0)
#endif

    // Overlay table (reads the double buffer without locks)
    void EmitOverlay(TextManager* tm, int x = 8, int y = 48, int maxLines = 16);

    // Temporary perf harness: write the current CPU+GPU overlay rows (the same data the HUD shows)
    // to a UTF-8 text file. Returns false on file-open failure. Reads the lock-free double buffer.
    bool DumpOverlay(const std::string& path);

    // Controls
#if PROF_ENABLED
    void SetEnabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    bool GetEnabled() const { return enabled_.load(std::memory_order_relaxed); }
    // Requests a trace capture for the given number of frames; calling again
    // while a capture is pending or active will stop/cancel it.
#endif
    void RequestTraceCapture(uint32_t frameCount);

    // Open-ended capture, for the trace window's Start/Stop. RequestTraceCapture takes a frame
    // COUNT, which forces the caller to guess how long the thing being investigated will last —
    // and the interesting cases (dragging a slider, walking into a stall) have no known length.
    void BeginTraceCapture();
    void StopTraceCapture();

    struct TraceCaptureStatus
    {
        bool     active = false;      // recording right now
        bool     pending = false;     // requested, starts next frame
        bool     openEnded = false;   // runs until StopTraceCapture
        uint32_t framesRemaining = 0; // meaningless when openEnded
        uint32_t framesRecorded = 0;
        size_t   events = 0;
        std::string lastPath;         // where the previous capture was written
    };
    TraceCaptureStatus GetTraceCaptureStatus() const;
    void Tick();
    void SetThreadName(const std::string& name);

    // Cooldown for resetting maxima
    void   SetMaxCooldownSeconds(double sec);
    double GetMaxCooldownSeconds() const;
    void   ResetMaxNow();

    // Interval for resorting rows in the overlay (seconds)
    void   SetOverlayResortIntervalSeconds(double sec) {
#if PROF_ENABLED
        if (sec < 0.05) { sec = 0.05; }
        std::lock_guard<std::mutex> lk(mtx_);
        overlayResortIntervalSec_ = sec;
#else
        (void)sec;
#endif
    }
    double GetOverlayResortIntervalSeconds() const {
#if PROF_ENABLED
        return overlayResortIntervalSec_;
#else
        return 0.0;
#endif
    }

private:
    Profiler();
    ~Profiler();
    void PushSample(const ScopeNameKey& key, CpuClock::time_point start, CpuClock::time_point end);
#if PROF_GPU_ENABLED
    void PushGpuSample(const ScopeNameKey& key, double ms);
    // Step 3: ONE calibration per queue. Two queues' timestamp counters are not the same clock and
    // are not required to tick at the same rate, so mapping a compute timestamp through the direct
    // queue's calibration produces a plausible-looking number in the wrong place — which is exactly
    // the failure that would make an overlap look real when it is not, or hide one that is.
    struct GpuTraceCalibration {
        UINT64 gpuTimestamp = 0;
        UINT64 cpuQpc = 0;
    };
    bool CaptureGpuTraceCalibration(UINT queueIndex, GpuTraceCalibration& calibration) const;
    uint64_t GpuTimestampToCpuUs(UINT64 gpuTimestamp, const GpuTraceCalibration& calibration,
                                 UINT queueIndex) const;
    void CollectGpuResolvedSamples(std::vector<ScopeSample>* sampleOut,
        std::vector<TraceEvent>* traceOut,
        uint64_t traceStartUs);
    bool WaitForGpuProfilerIdle();
    size_t BeginGpuSample(ID3D12GraphicsCommandList* cl, const ScopeNameKey& key);
    void EndGpuSample(ID3D12GraphicsCommandList* cl, size_t idx);
#endif

#if PROF_ENABLED
    void ResetMax_Unsafe(); // call while holding mtx_
    uint32_t GetThreadIndex_Locked(std::thread::id id);
    void WriteTraceJson(const std::vector<TraceEvent>& events);
    void ReleaseTraceNameKeys(const std::vector<ScopeNameKey>& keys);
#endif

private:
#if PROF_ENABLED
    // statistics collection
    mutable std::mutex mtx_; // mutable: GetTraceCaptureStatus is a const query
    std::mutex traceMtx_;
    robin_hood::unordered_flat_map<ScopeNameKey, StatsEntry, ScopeNameKeyHash, ScopeNameKeyEqual> stats_;
#if PROF_GPU_ENABLED
    robin_hood::unordered_flat_map<ScopeNameKey, StatsEntry, ScopeNameKeyHash, ScopeNameKeyEqual> gpuStats_;
#endif
    robin_hood::unordered_flat_map<std::thread::id, std::string> threadNames_;
    robin_hood::unordered_flat_map<std::thread::id, uint32_t> threadIndices_;
    std::vector<std::string> threadIndexToName_;

    struct SampleNode {
        ScopeSample sample;
        SampleNode* next = nullptr;
    };

    static SampleNode* const kSampleListClosed;

    SampleNode* AcquireSampleNode();
    void        ReleaseSampleNode(SampleNode* node);
    void        ReleaseSampleList(SampleNode* head);
    bool        PushSampleNode(std::atomic<SampleNode*>& head, SampleNode* node);
    void        DrainSampleNodes(std::atomic<SampleNode*>& head, std::vector<ScopeSample>& out);
    uint32_t    GetThreadIndexForCurrentThread();

    struct TraceSampleNode {
        TraceEvent sample;
        TraceSampleNode* next = nullptr;
    };

    static TraceSampleNode* const kTraceListClosed;

    TraceSampleNode* AcquireTraceSampleNode();
    void             ReleaseTraceSampleNode(TraceSampleNode* node);
    void             ReleaseTraceSampleList(TraceSampleNode* head);
    bool             PushTraceSampleNode(TraceSampleNode* node);
    void             DrainTraceSampleNodes(uint64_t traceStartUs);

    std::atomic<SampleNode*> frameSampleHead_{ nullptr };
#if PROF_GPU_ENABLED
    std::atomic<SampleNode*> gpuFrameSampleHead_{ nullptr };
#endif
    std::atomic<SampleNode*> sampleNodePool_{ nullptr };
    std::atomic<bool>        frameOpenFlag_{ false };

    std::atomic<TraceSampleNode*> traceSampleHead_{ nullptr };
    std::atomic<TraceSampleNode*> traceSamplePool_{ nullptr };

    std::vector<ScopeSample> frameSamples_;
#if PROF_GPU_ENABLED
    std::vector<ScopeSample> gpuFrameSamples_;
#endif
    uint64_t  frameNo_ = 0;
    bool      frameOpen_ = false;
    CpuClock::time_point frameCpuStart_{};

    // EMA for avgMs
    double emaAlpha_ = 0.997;

    std::atomic<bool> enabled_{ true };

    // Cooldown for resetting maxima
    using CoolClock = std::chrono::steady_clock;
    CoolClock::time_point lastMaxReset_{};
    double maxResetIntervalSec_ = 5.0;

    double endFrameAsyncLastMs_ = 0.0;
    double endFrameAsyncAvgMs_ = 0.0;
    double endFrameAsyncMaxMs_ = 0.0;

    // Overlay: double-buffered strings (EndFrame writes, EmitOverlay reads)
    std::vector<OverlayRow> overlayRows_[2];
    std::atomic<int>        overlayReadBuf_{ 0 }; // 0 or 1
#if PROF_GPU_ENABLED
    std::vector<OverlayRow> gpuOverlayRows_[2];
    std::atomic<int>        gpuOverlayReadBuf_{ 0 };
#endif

    // Scratch buffers for building the overlay (reallocated rarely, reused between frames)
    std::vector<OverlayRow> overlayScratchRows_;
    robin_hood::unordered_flat_map<uint32_t, size_t> overlayScratchLookup_;
    std::vector<uint8_t> overlayScratchUsed_;
    std::vector<const StatsEntry*> overlayEntryPtrs_;
    std::vector<size_t> overlayScratchOrder_;
#if PROF_GPU_ENABLED
    std::vector<OverlayRow> gpuOverlayScratchRows_;
    robin_hood::unordered_flat_map<uint32_t, size_t> gpuOverlayScratchLookup_;
    std::vector<uint8_t> gpuOverlayScratchUsed_;
    std::vector<const StatsEntry*> gpuOverlayEntryPtrs_;
    std::vector<size_t> gpuOverlayScratchOrder_;
#endif

    // Resort by avgMs every N seconds
    CoolClock::time_point lastOverlaySort_{};
    double overlayResortIntervalSec_ = 4.0; // defaults to once per second

    // Overlay region width (fixed)—a smoothed estimate to avoid measuring strings
    double overlayWidthPx_ = 640.0;
#if PROF_GPU_ENABLED
    double gpuOverlayWidthPx_ = 640.0;
#endif

    uint32_t nextOverlayId_ = 1;
#if PROF_GPU_ENABLED
    uint32_t nextGpuOverlayId_ = 1;
#endif

    TaskSystem::TaskHandle overlayTask_ = nullptr;

    bool traceCaptureRequested_ = false;
    uint32_t traceRequestFrameCount_ = 0;
    bool traceCapturing_ = false;
    std::atomic<bool> traceCapturingAtomic_{ false };
    bool traceStopRequested_ = false;
    bool traceOpenEnded_ = false;
    uint32_t traceFramesRemaining_ = 0;
    uint32_t traceFramesRecorded_ = 0;
    std::string traceLastPath_;
    uint64_t traceStartUs_ = 0;
    bool traceStartSet_ = false;
    std::vector<TraceEvent> traceEvents_;
    std::atomic<uint32_t> traceFileCounter_{ 0 };
    std::vector<ScopeSample> asyncCpuSamples_;
#if PROF_GPU_ENABLED
    std::vector<ScopeSample> asyncGpuSamples_;
#endif
    TraceDumpData traceDumpData_;

#if PROF_GPU_ENABLED
    // GPU timestamp queries
    static constexpr UINT kGpuQueueCount = 2; // 0 = direct, 1 = async compute (step 3)
    std::mutex gpuMtx_;
    // Step 3, CORRECTED BY THE DEBUG LAYER: the query heap and the readback buffer are PER QUEUE.
    //
    // The first cut shared them, reasoning that a TIMESTAMP heap is valid on both queue types and
    // that the counter only has to hand out unique slots. Both true, and both beside the point: a
    // GPU scope on a compute list ends in `ResolveQueryData`, which WRITES the readback buffer — so
    // two queues were writing one resource with no synchronisation between them. D3D12 says so
    // plainly (id=1047, "still referenced by write GPU operations in-flight on another Command
    // Queue"), and it says it at ExecuteCommandLists, not at the write.
    //
    // Disjoint resources are the fix rather than cross-queue fences around the profiler's own
    // writes: the profiler must not add synchronisation between the queues it is measuring.
    // Costs one extra 8 KB heap and one extra readback buffer.
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> gpuQueryHeap_[kGpuQueueCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> gpuReadback_[kGpuQueueCount];
    // Step 3 — everything below that is [kGpuQueueCount] is per-queue BECAUSE IT HAS TO BE:
    //   * gpuDrainFence_ decides when a resolved batch may be READ. It used to be signalled only on
    //     the direct queue, so a ResolveQueryData recorded on the compute queue would be declared
    //     readable before it had run, and the collector would map a readback range nobody had
    //     written yet — fictitious numbers, silently, from the tool this whole plan is judged by.
    //   * gpuQueue_ is what GetClockCalibration is asked of, and each queue has its own clock.
    //   * gpuFreq_ is per queue by API contract (GetTimestampFrequency is a queue method).
    // The query HEAP and nextGpuQuery_ deliberately stay SHARED: a TIMESTAMP heap is valid on both
    // queue types and the counter only has to hand out unique slots. "Shared heap, split fence" is
    // an asymmetry a later reader will assume is a bug, so it is stated here.
    Microsoft::WRL::ComPtr<ID3D12Fence> gpuDrainFence_[kGpuQueueCount];
    ID3D12CommandQueue* gpuQueue_[kGpuQueueCount] = {};
    HANDLE gpuDrainFenceEvent_[kGpuQueueCount] = {};
    UINT64 gpuFreq_[kGpuQueueCount] = {};
    UINT64 gpuTraceQpcFreq_ = 0;
    UINT64 gpuTraceQpcOrigin_ = 0;
    uint64_t gpuTraceCpuOriginUs_ = 0;
    UINT64 gpuDrainFenceValue_ = 1;
    UINT maxGpuQueries_ = 0;
    UINT nextGpuQuery_ = 0;
    UINT gpuRecordingReadbackSlot_ = 0;
    // Step 3: `queueIndex` is taken from the recording command list's OWN type
    // (ID3D12GraphicsCommandList::GetType()) rather than plumbed through the ~100 GPU_SCOPE call
    // sites. That is both zero-churn and impossible to get wrong — a scope cannot be mislabelled,
    // because the label is read off the thing doing the recording.
    struct GpuSampleRange { ScopeNameKey key; UINT start; UINT end; bool completed; uint64_t frameNo; UINT queueIndex; };
    std::vector<GpuSampleRange> gpuPending_;
    struct GpuResolvedBatch {
        std::vector<GpuSampleRange> ranges;
        UINT queryCount = 0;
        UINT readbackSlot = 0;
        // Step 3: ONE value signalled on BOTH drain fences (same trick as the frame fences). The
        // batch may hold ranges resolved on either queue, so it is readable only once BOTH have
        // passed — one value, two fences, no per-range bookkeeping.
        UINT64 fenceValue = 0;
    };
    std::vector<GpuResolvedBatch> gpuResolvedBatches_;
    bool gpuInitialized_ = false;
    std::atomic<size_t> gpuFrameSampleIdx_{ SIZE_MAX };
#endif
#endif
};

#if !PROF_ENABLED
inline Profiler::Profiler() = default;
inline Profiler::~Profiler() = default;
inline Profiler& Profiler::Get() { static Profiler p; return p; }
inline void Profiler::BeginFrame(uint64_t) {}
inline void Profiler::EndFrame() {}
inline void Profiler::EmitOverlay(TextManager*, int, int, int) {}
inline void Profiler::SetMaxCooldownSeconds(double) {}
inline double Profiler::GetMaxCooldownSeconds() const { return 0.0; }
inline void Profiler::ResetMaxNow() {}
inline void Profiler::RequestTraceCapture(uint32_t) {}
inline void Profiler::Tick() {}
inline void Profiler::SetThreadName(const std::string&) {}
inline Profiler::ScopeNameKey Profiler::RegisterTraceLiteral(const wchar_t*) { return {}; }
inline Profiler::ScopeNameKey Profiler::RegisterTraceDynamic(std::wstring, uint32_t* outIndex) {
    if (outIndex) { *outIndex = 0; }
    return {};
}
#if PROF_GPU_ENABLED
inline void Profiler::InitGpu(ID3D12Device*, ID3D12CommandQueue*, ID3D12CommandQueue*, UINT) {}
inline void Profiler::CollectGpuResults() {}
inline void Profiler::BeginGpuFrame(ID3D12GraphicsCommandList*) {}
inline void Profiler::EndGpuFrame(ID3D12GraphicsCommandList*) {}
inline void Profiler::ShutdownGpu() {}
#endif
#endif
