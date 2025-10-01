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

#include "TaskSystem.h"

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

// Профайлер CPU: скоупы, EMA-среднее, кулдаун сброса максимумов,
// подготовка оверлея в EndFrame (дабл-буфер), редкая сортировка по среднему.
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
        // Инклюзивные накопители за кадр (складывают ms всех обращений скоупа в текущем кадре)
        double   frameMsSum = 0.0;
        uint32_t frameCount = 0;

        // Показатели
        double   avgMs = 0.0;   // EMA
        double   maxMs = 0.0;   // сбрасывается по кулдауну
        uint32_t lastCount = 0; // сколько раз встретился скоуп в прошлом кадре

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
        ScopeNameKey key{};
        uint64_t     tsUs = 0;
        uint64_t     durUs = 0;
        uint32_t     threadIndex = 0;
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

    // --- данные оверлея (снэпшот) ---
    struct OverlayRow {
        uint32_t entryId = 0;
        double   avgMs = 0.0;
        double   maxMs = 0.0;
        uint32_t usages = 0;
        std::wstring formatted; // заранее отформатированная строка
    };

public:
    static Profiler& Get();

    static ScopeNameKey RegisterTraceLiteral(const wchar_t* name);
    static ScopeNameKey RegisterTraceDynamic(std::wstring name, uint32_t* outIndex = nullptr);

    // Границы кадра
    void BeginFrame(uint64_t frameNo);
    void EndFrame();

#if PROF_GPU_ENABLED
    // Инициализация и сбор результатов GPU
    void InitGpu(ID3D12Device* device, ID3D12CommandQueue* queue, UINT maxQueries = 1024);
    void CollectGpuResults();
    void BeginGpuFrame(ID3D12GraphicsCommandList* cl);
    void EndGpuFrame(ID3D12GraphicsCommandList* cl);
#endif

    // Скоповая отметка (CPU)
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
    // Скоповая отметка (GPU)
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

    // Оверлей с табличкой (читает дабл-буфер без локов)
    void EmitOverlay(TextManager* tm, int x = 8, int y = 48, int maxLines = 16);

    // Управление
#if PROF_ENABLED
    void SetEnabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    bool GetEnabled() const { return enabled_.load(std::memory_order_relaxed); }
    // Requests a trace capture for the given number of frames; calling again
    // while a capture is pending or active will stop/cancel it.
#endif
    void RequestTraceCapture(uint32_t frameCount);
    void SetThreadName(const std::string& name);

    // Кулдаун сброса максимумов
    void   SetMaxCooldownSeconds(double sec);
    double GetMaxCooldownSeconds() const;
    void   ResetMaxNow();

    // Интервал пересортировки строк в оверлее (сек)
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
    size_t BeginGpuSample(ID3D12GraphicsCommandList* cl, const ScopeNameKey& key);
    void EndGpuSample(ID3D12GraphicsCommandList* cl, size_t idx);
    void ShutdownGpu();
#endif

#if PROF_ENABLED
    void ResetMax_Unsafe(); // вызывать под mtx_
    uint32_t GetThreadIndex_Locked(std::thread::id id);
    void WriteTraceJson(const std::vector<TraceEvent>& events);
    void ReleaseTraceNameKeys(const std::vector<ScopeNameKey>& keys);
#endif

private:
#if PROF_ENABLED
    // сбор статистики
    std::mutex mtx_;
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

    // EMA для avgMs
    double emaAlpha_ = 0.995;

    std::atomic<bool> enabled_{ true };

    // Кулдаун сброса максимумов
    using CoolClock = std::chrono::steady_clock;
    CoolClock::time_point lastMaxReset_{};
    double maxResetIntervalSec_ = 3.0;

    double endFrameAsyncLastMs_ = 0.0;
    double endFrameAsyncAvgMs_ = 0.0;
    double endFrameAsyncMaxMs_ = 0.0;

    // Оверлей: дабл-буфер строк (EndFrame пишет, EmitOverlay читает)
    std::vector<OverlayRow> overlayRows_[2];
    std::atomic<int>        overlayReadBuf_{ 0 }; // 0 или 1
#if PROF_GPU_ENABLED
    std::vector<OverlayRow> gpuOverlayRows_[2];
    std::atomic<int>        gpuOverlayReadBuf_{ 0 };
#endif

    // scratch буферы для построения оверлея (перевыделяются редко, переиспользуются между кадрами)
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

    // Переcортировка по avgMs раз в N сек
    CoolClock::time_point lastOverlaySort_{};
    double overlayResortIntervalSec_ = 2.0; // по умолчанию раз в секунду

    // Ширина оверлей-региона (фикс) — сглаженная оценка, чтобы не мерить строки
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
    uint32_t traceFramesRemaining_ = 0;
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
    std::mutex gpuMtx_;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> gpuQueryHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> gpuReadback_;
    ID3D12CommandQueue* gpuQueue_ = nullptr;
    UINT64 gpuFreq_ = 0;
    UINT maxGpuQueries_ = 0;
    UINT nextGpuQuery_ = 0;
    UINT lastGpuQueryCount_ = 0;
    struct GpuSampleRange { ScopeNameKey key; UINT start; UINT end; bool completed; };
    std::vector<GpuSampleRange> gpuPending_;
    std::vector<GpuSampleRange> gpuResolved_;
    bool gpuInitialized_ = false;
    std::atomic<size_t> gpuFrameSampleIdx_{ SIZE_MAX };
#endif
#endif
};

#if !PROF_ENABLED
inline Profiler& Profiler::Get() { static Profiler p; return p; }
inline void Profiler::BeginFrame(uint64_t) {}
inline void Profiler::EndFrame() {}
inline void Profiler::EmitOverlay(TextManager*, int, int, int) {}
inline void Profiler::SetMaxCooldownSeconds(double) {}
inline double Profiler::GetMaxCooldownSeconds() const { return 0.0; }
inline void Profiler::ResetMaxNow() {}
inline void Profiler::RequestTraceCapture(uint32_t) {}
inline void Profiler::SetThreadName(const std::string&) {}
#if PROF_GPU_ENABLED
inline void Profiler::BeginGpuFrame(ID3D12GraphicsCommandList*) {}
inline void Profiler::EndGpuFrame(ID3D12GraphicsCommandList*) {}
#endif
#endif
