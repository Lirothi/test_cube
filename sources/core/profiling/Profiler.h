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
#include <array>
#include <condition_variable>
#include <deque>
#include <memory>

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
// Numeric snapshots are independent of the demand-driven HUD cache.
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
        uint64_t     frameNumber = 0; // original GPU frame, not its CPU readback frame
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
        }
        void CommitFrame(double emaAlpha) {
            if (frameCount == 0) { lastCount = 0; return; }
            const double cur = frameMsSum;
            maxMs = (std::max)(maxMs, cur);
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

    // A counter sample ("ph":"C"): one track, up to kMaxTraceCounterSeries named values at one time.
    // Names are static strings (the memory tags' table, literals). Memory profiling plan M2: the GPU
    // census and the process numbers as tracks beside the CPU/GPU scopes.
    static constexpr int kMaxTraceCounterSeries = 32;
    struct TraceCounter {
        const char* track = nullptr;
        uint64_t    tsUs = 0;
        int         count = 0;
        const char* names[kMaxTraceCounterSeries] = {};
        double      values[kMaxTraceCounterSeries] = {};
    };

    struct TraceDumpData {
        std::vector<TraceEvent> events;
        std::vector<ScopeNameKey> dynamicKeys;
        std::vector<std::string> threadNames;
        std::vector<TraceCounter> counters;
    };

    // --- overlay data (snapshot) ---
    struct OverlayRow {
        uint32_t entryId = 0;
        double   avgMs = 0.0;
        double   maxMs = 0.0;
        uint32_t usages = 0;
        std::wstring name;
        std::wstring formatted; // HUD cache only; numeric snapshots never format text
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
            auto& profiler = Profiler::Get();
            epoch_ = profiler.GetEnabled() ? profiler.sampleEpoch_.load(std::memory_order_acquire) : 0;
            if (epoch_) { start_ = Clock::now(); }
#endif
        }
        ~ScopedCpu() {
#if PROF_ENABLED
            if (epoch_) { Profiler::Get().PushSample(key_, start_, Clock::now(), epoch_); }
#endif
        }
    private:
#if PROF_ENABLED
        using Clock = CpuClock;
        Clock::time_point start_{};
        uint64_t epoch_ = 0;
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
        ScopedGpu(const ScopedGpu&) = delete;
        ScopedGpu& operator=(const ScopedGpu&) = delete;
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

    // Overlay table (formats only visible rows, at most 10 Hz)
    void EmitOverlay(TextManager* tm, int x = 8, int y = 48, int maxLines = 16);

    // Temporary perf harness: write the current CPU+GPU overlay rows (the same data the HUD shows)
    // to a UTF-8 text file. Returns false on file-open failure. Reads the numeric snapshot.
    bool DumpOverlay(const std::string& path);
    // The same rows as data -- the editor's MCP `get_profile` reads them from a running frame.
    struct OverlaySample
    {
        std::string name;
        double avgMs = 0.0; // EMA
        double maxMs = 0.0; // since the last ResetMaxNow / cooldown
        uint32_t usages = 0;
        bool gpu = false;
    };
    std::vector<OverlaySample> OverlaySnapshot() const;

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
    // True while a capture records (a relaxed read; no lock). A counter source checks it before
    // gathering anything, so it costs nothing outside a capture.
    bool IsTraceCapturing() const;
    // One counter sample on `track` at the current time, kept only while a capture records; at most
    // kMaxTraceCounterSeries values. `track` and `names` must outlive the capture (static strings).
    void RecordTraceCounters(const char* track, const char* const* names, const double* values, int count);
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
        overlayResortIntervalSec_.store(sec, std::memory_order_relaxed);
#else
        (void)sec;
#endif
    }
    double GetOverlayResortIntervalSeconds() const {
#if PROF_ENABLED
        return overlayResortIntervalSec_.load(std::memory_order_relaxed);
#else
        return 0.0;
#endif
    }

private:
    Profiler();
    ~Profiler();
    void PushSample(const ScopeNameKey& key, CpuClock::time_point start, CpuClock::time_point end, uint64_t epoch);
#if PROF_GPU_ENABLED
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
    void CollectGpuResolvedSamples(std::vector<ScopeSample>& sampleOut,
        std::vector<TraceEvent>* traceOut,
        uint64_t traceStartUs);
    size_t BeginGpuSample(ID3D12GraphicsCommandList* cl, const ScopeNameKey& key);
    void EndGpuSample(ID3D12GraphicsCommandList* cl, size_t idx);
#endif

#if PROF_ENABLED
    void ResetMax_Unsafe(); // aggregation task only
    uint32_t GetThreadIndex_Locked(std::thread::id id);
    void WriteTraceJson(const TraceDumpData& dump);
    void FinishTraceIfReady();
    void QueueTraceWrite(TraceDumpData dump);
    void StopTraceWriter();
    void ReleaseTraceNameKeys(const std::vector<ScopeNameKey>& keys);
#endif

private:
    friend struct ProfilerTestAccess;
#if PROF_ENABLED
    // Capture controls / thread names. Statistics have one writer: overlayTask_.
    mutable std::mutex mtx_;
    using StatsMap = robin_hood::unordered_flat_map<ScopeNameKey, StatsEntry, ScopeNameKeyHash, ScopeNameKeyEqual>;
    StatsMap stats_;
#if PROF_GPU_ENABLED
    StatsMap gpuStats_;
    struct GpuResolveGroup {
        ID3D12GraphicsCommandList* cl = nullptr;
        std::vector<UINT> starts;
        uint32_t depth = 0;
    };
#endif
    robin_hood::unordered_flat_map<std::thread::id, std::string> threadNames_;
    robin_hood::unordered_flat_map<std::thread::id, uint32_t> threadIndices_;
    std::vector<std::string> threadIndexToName_;

    // One producer per buffer. The consumer swaps under this buffer's own lock;
    // no shared free list, speculative node reads, or per-sample heap allocation.
    struct alignas(64) ThreadSamples {
        std::mutex mutex;
        std::vector<ScopeSample> samples, drainedSamples;
        std::vector<TraceEvent> traces, drainedTraces;
        uint32_t threadIndex = 0;
#if PROF_GPU_ENABLED
        std::vector<std::unique_ptr<GpuResolveGroup>> gpuGroups;
#endif
    };
    ThreadSamples& GetThreadSamples();
    void DrainThreadSamples(bool capture, uint64_t startUs);
    std::mutex buffersMutex_;
    std::vector<std::unique_ptr<ThreadSamples>> threadSamples_;
    std::atomic<uint64_t> sampleEpoch_{ 0 }; // 0 = closed; prevents late producers crossing frames
    uint64_t nextSampleEpoch_ = 0;
    uint64_t frameNo_ = 0;
    bool frameOpen_ = false;
    CpuClock::time_point frameCpuStart_{};
    double emaAlpha_ = 0.997;
    std::atomic<bool> enabled_{ true };

    using CoolClock = std::chrono::steady_clock;
    CoolClock::time_point lastMaxReset_{}, lastOverlaySort_{};
    std::atomic<double> maxResetIntervalSec_{ 5.0 };
    std::atomic<double> overlayResortIntervalSec_{ 4.0 };
    std::atomic<bool> resetMaxRequested_{ false };
    double endFrameAsyncAvgMs_ = 0.0, endFrameAsyncMaxMs_ = 0.0;
    uint32_t nextOverlayId_ = 1, nextGpuOverlayId_ = 1;
    std::vector<ScopeNameKey> overlayOrder_, gpuOverlayOrder_;

    // Readers copy numeric data under a short lock. Text is only generated by
    // EmitOverlay (at most 10 Hz) into persistent consumer-owned string buffers.
    mutable std::mutex snapshotMutex_;
    std::vector<OverlayRow> overlayRows_, gpuOverlayRows_;
    std::vector<OverlayRow> hudRows_, gpuHudRows_;
    CoolClock::time_point lastHudUpdate_{};
    double overlayWidthPx_ = 640.0, gpuOverlayWidthPx_ = 640.0;
    TaskSystem::TaskHandle overlayTask_ = nullptr;
    std::vector<ScopeSample> asyncCpuSamples_, asyncGpuSamples_, gpuFrameSamples_;

    bool traceCaptureRequested_ = false, traceCapturing_ = false, traceDraining_ = false;
    std::atomic<bool> traceCapturingAtomic_{ false };
    bool traceStopRequested_ = false, traceOpenEnded_ = false;
    uint32_t traceRequestFrameCount_ = 0, traceFramesRemaining_ = 0, traceFramesRecorded_ = 0;
    std::string traceLastPath_;
    uint64_t traceStartUs_ = 0, traceFirstFrame_ = 0, traceLastFrame_ = 0;
    std::vector<TraceEvent> traceEvents_;
    std::vector<TraceCounter> traceCounters_;
    // Mirrors traceCapturing_ across the whole capture (traceCapturingAtomic_ only covers the open
    // frame), for IsTraceCapturing's lock-free read from code running between frames.
    std::atomic<bool> traceRecording_{ false };
    std::atomic<uint32_t> traceFileCounter_{ 0 };
    // Export owns immutable events, names and thread metadata. It is never a
    // dependency of EndFrame / the render workers; shutdown alone joins it.
    std::mutex traceWriterMutex_;
    std::condition_variable traceWriterCv_;
    std::deque<TraceDumpData> traceWriteQueue_;
    std::thread traceWriter_;
    bool traceWriterStop_ = false;

#if PROF_GPU_ENABLED
    static constexpr UINT kGpuQueueCount = 2;
    static constexpr UINT kGpuReadbackFrameSlots = 8;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> gpuQueryHeap_[kGpuQueueCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> gpuReadback_[kGpuQueueCount];
    Microsoft::WRL::ComPtr<ID3D12Fence> gpuDrainFence_[kGpuQueueCount];
    ID3D12CommandQueue* gpuQueue_[kGpuQueueCount] = {};
    UINT64* gpuMappedReadback_[kGpuQueueCount] = {};
    UINT64 gpuFreq_[kGpuQueueCount] = {};
    UINT64 gpuTraceQpcFreq_ = 0, gpuTraceQpcOrigin_ = 0;
    uint64_t gpuTraceCpuOriginUs_ = 0;
    UINT64 gpuDrainFenceValue_ = 1;
    UINT maxGpuQueries_ = 0;
    std::atomic<UINT> nextGpuQuery_{ 0 };
    UINT gpuRecordingReadbackSlot_ = 0;
    bool gpuRecordingReady_ = false;
    // Unique preallocated slots: one atomic reservation replaces two global
    // mutex acquisitions per scope. Frame boundaries join recording workers.
    struct GpuSampleRange {
        ScopeNameKey key;
        UINT start = 0, end = 0;
        bool completed = false;
        uint64_t frameNo = 0;
        UINT queueIndex = 0;
        GpuResolveGroup* group = nullptr;
    };
    std::vector<GpuSampleRange> gpuPending_;
    struct GpuResolvedBatch {
        std::vector<GpuSampleRange> ranges;
        UINT queryCount = 0;
        UINT64 fenceValue = 0;
        uint64_t frameNo = 0;
    };
    std::array<GpuResolvedBatch, kGpuReadbackFrameSlots> gpuResolvedBatches_;
    bool gpuInitialized_ = false;
    size_t gpuFrameSampleIdx_ = SIZE_MAX;

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
inline void Profiler::BeginTraceCapture() {}
inline void Profiler::StopTraceCapture() {}
inline Profiler::TraceCaptureStatus Profiler::GetTraceCaptureStatus() const { return {}; }
inline bool Profiler::IsTraceCapturing() const { return false; }
inline void Profiler::RecordTraceCounters(const char*, const char* const*, const double*, int) {}
inline std::vector<Profiler::OverlaySample> Profiler::OverlaySnapshot() const { return {}; }
inline bool Profiler::DumpOverlay(const std::string&) { return false; }
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
