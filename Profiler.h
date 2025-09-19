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

#include "TaskSystem.h"

#ifndef PROF_ENABLED
#define PROF_ENABLED 1
#endif

#ifndef PROF_GPU_ENABLED
#define PROF_GPU_ENABLED 0
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
    struct ScopeSample {
        const char* name = nullptr; // ожидается литерал/статическая строка
        uint64_t    nameId = 0;     // зарезервировано под быстрый id
        double      ms = 0.0;
    };

    struct ScopeStats {
        // Инклюзивные накопители за кадр
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

public:
    static Profiler& Get();

    // Границы кадра
    void BeginFrame(uint64_t frameNo);
    void EndFrame();

#if PROF_GPU_ENABLED
    // Инициализация и сбор результатов GPU
    void InitGpu(ID3D12Device* device, ID3D12CommandQueue* queue, UINT maxQueries = 1024);
    void CollectGpuResults();
#endif

    // Скоповая отметка (CPU)
    class ScopedCpu {
    public:
        ScopedCpu(const char* name, uint64_t nameId = 0) : name_(name), id_(nameId) {
#if PROF_ENABLED
            start_ = Clock::now();
#endif
        }
        ~ScopedCpu() {
#if PROF_ENABLED
            const auto end = Clock::now();
            const double ms = std::chrono::duration<double, std::milli>(end - start_).count();
            Profiler::Get().PushSample(name_, id_, ms);
#endif
        }
    private:
#if PROF_ENABLED
        using Clock = std::chrono::high_resolution_clock;
        Clock::time_point start_{};
#endif
        const char* name_;
        uint64_t    id_;
    };

#if PROF_ENABLED
#define CPU_SCOPE(nameLiteral)       Profiler::ScopedCpu _prof_scope_##__LINE__(nameLiteral, 0)
#define CPU_SCOPE_N(nameLiteral, id) Profiler::ScopedCpu _prof_scope_##__LINE__(nameLiteral, (id))
#else
#define CPU_SCOPE(nameLiteral)       do { } while (0)
#define CPU_SCOPE_N(nameLiteral, id) do { } while (0)
#endif

#if PROF_GPU_ENABLED
    // Скоповая отметка (GPU)
    class ScopedGpu {
    public:
        ScopedGpu(ID3D12GraphicsCommandList* cl, const char* name, uint64_t nameId = 0);
        ~ScopedGpu();
    private:
#if PROF_ENABLED
        ID3D12GraphicsCommandList* cl_ = nullptr;
        size_t idx_ = SIZE_MAX;
#endif
    };

#if PROF_ENABLED
#define GPU_SCOPE(cmdList, nameLiteral)       Profiler::ScopedGpu _prof_gpu_scope_##__LINE__(cmdList, nameLiteral, 0)
#define GPU_SCOPE_N(cmdList, nameLiteral, id) Profiler::ScopedGpu _prof_gpu_scope_##__LINE__(cmdList, nameLiteral, (id))
#else
#define GPU_SCOPE(cmdList, nameLiteral)       do { } while (0)
#define GPU_SCOPE_N(cmdList, nameLiteral, id) do { } while (0)
#endif
#else
    class ScopedGpu {
    public:
        ScopedGpu(... ) {}
    };
#define GPU_SCOPE(cmdList, nameLiteral)       do { } while (0)
#define GPU_SCOPE_N(cmdList, nameLiteral, id) do { } while (0)
#endif

    // Оверлей с табличкой (читает дабл-буфер без локов)
    void EmitOverlay(TextManager* tm, int x = 8, int y = 48, int maxLines = 16);

    // Управление
#if PROF_ENABLED
    void SetEnabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    bool GetEnabled() const { return enabled_.load(std::memory_order_relaxed); }
#endif

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
    Profiler() = default;
    void PushSample(const char* name, uint64_t id, double ms);
#if PROF_GPU_ENABLED
    void PushGpuSample(const char* name, uint64_t id, double ms);
    size_t BeginGpuSample(ID3D12GraphicsCommandList* cl, const char* name, uint64_t id);
    void EndGpuSample(ID3D12GraphicsCommandList* cl, size_t idx);
#endif

#if PROF_ENABLED
    void ResetMax_Unsafe(); // вызывать под mtx_
#endif

    // --- данные оверлея (снэпшот) ---
    struct OverlayRow {
        std::wstring name; // уже wide, чтобы не конвертировать при отрисовке
        double   avgMs = 0.0;
        double   maxMs = 0.0;
        uint32_t usages = 0;
        std::wstring formatted; // заранее отформатированная строка
    };

private:
#if PROF_ENABLED
    // сбор статистики
    std::mutex mtx_;
    robin_hood::unordered_map<std::string, ScopeStats> stats_;
#if PROF_GPU_ENABLED
    robin_hood::unordered_map<std::string, ScopeStats> gpuStats_;
#endif
    robin_hood::unordered_flat_map<std::thread::id, std::string> threadNames_;

    std::vector<ScopeSample> frameSamples_;
#if PROF_GPU_ENABLED
    std::vector<ScopeSample> gpuFrameSamples_;
#endif
    uint64_t  frameNo_ = 0;
    bool      frameOpen_ = false;

    // EMA для avgMs
    double emaAlpha_ = 0.99;

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

    // Переcортировка по avgMs раз в N сек
    CoolClock::time_point lastOverlaySort_{};
    double overlayResortIntervalSec_ = 1.0; // по умолчанию раз в секунду

    // Ширина оверлей-региона (фикс) — сглаженная оценка, чтобы не мерить строки
    double overlayWidthPx_ = 640.0;
#if PROF_GPU_ENABLED
    double gpuOverlayWidthPx_ = 640.0;
#endif

    TaskSystem::TaskHandle overlayTask_ = nullptr;

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
    struct GpuSampleRange { const char* name; UINT start; UINT end; };
    std::vector<GpuSampleRange> gpuPending_;
    std::vector<GpuSampleRange> gpuResolved_;
    bool gpuInitialized_ = false;
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
#endif
