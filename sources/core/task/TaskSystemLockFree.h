#pragma once

#ifdef TASKSYSTEM_USE_LOCKFREE

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <core/containers/inl_vector.h>

#ifndef TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
#define TASKSYSTEM_ENABLE_PARALLEL_EXECUTION 1
#endif

class TaskSystem {
public:
    struct TaskWithDeps;
    using TaskHandle = TaskWithDeps*;

    enum class TaskKind { Lambda, Range };

    struct TaskWithDeps {
        TaskWithDeps(TaskSystem& owner, TaskKind kind, std::size_t depCapacity);
        virtual ~TaskWithDeps() = default;

        // Returns false when the fixed dependents_ capacity is exhausted. The
        // caller must treat that as fatal: the dependent's pendingDeps_ is
        // already counted, so silently dropping the edge would hang it forever,
        // and a raw push would write out of bounds in Release.
        [[nodiscard]] bool AddDependent(TaskHandle dependent);
        void IncrementDependency();
        void DependencySatisfied();
        void NotifyDependents();

        virtual void Execute() = 0;

        TaskSystem& owner_;
        TaskKind kind_;
        TaskWithDeps* nextFree_{nullptr};
        std::atomic<std::size_t> pendingDeps_{0};
        //std::vector<TaskHandle> dependents_;
        tc::inl_vector<TaskHandle, 4> dependents_;
        // Completion flag, waited on via C++20 atomic wait. Waiters must hold a
        // reference to the handle so the task cannot be recycled under them.
        std::atomic<std::uint32_t> completed_{0};
        std::atomic<bool> submitted_{false};
        std::atomic<bool> scheduled_{false};
        std::atomic<int> refCount_{1};

    protected:
        void Prepare(std::size_t depCapacity);
    };

    using Task = std::function<void()>;

    static TaskSystem& Get();

    void Start(unsigned threadCount = 0);
    void Stop();

    TaskHandle Submit(Task t);
    TaskHandle CreateTask(Task t, std::size_t depCount = 0);
    TaskHandle CreateRangeTask(std::size_t jobCount,
                               std::function<void(std::size_t)> fn,
                               std::size_t batchSize = 1,
                               std::size_t depCount = 0);
    void SetDependencies(TaskHandle handle, const TaskHandle* deps, std::size_t depCount);
    void Submit(TaskHandle handle);
    TaskHandle Dispatch(std::size_t jobCount,
                        std::function<void(std::size_t)> fn,
                        std::size_t batchSize = 1);
    void DispatchTrack(std::size_t jobCount,
                       std::function<void(std::size_t)> fn,
                       std::size_t batchSize = 1);
    void DispatchWait(std::size_t jobCount,
                      std::function<void(std::size_t)> fn,
                      std::size_t batchSize = 1);

    void SubmitDetach(Task t);
    void DispatchDetach(std::size_t jobCount,
                        std::function<void(std::size_t)> fn,
                        std::size_t batchSize = 1);

    void Wait(TaskHandle handle);
    void Release(TaskHandle& handle);

    void TrackFrameTask(TaskHandle handle);
    void WaitForTrackedAsyncTasks();

    template<class F>
    static void ParallelFor(std::size_t jobCount, F&& fn, std::size_t batchSize)
    {
        Get().DispatchWait(jobCount, std::forward<F>(fn), batchSize);
    }

    void WaitForAll();

    std::size_t ThreadIndex() const;

private:
    struct LambdaTaskSet : TaskWithDeps {
        LambdaTaskSet(TaskSystem& owner, Task&& fn, std::size_t depCapacity);
        void Reset(Task&& fn, std::size_t depCapacity);
        void Execute() override;
        Task fn_;
    };

    struct RangeTaskSet : TaskWithDeps {
        RangeTaskSet(TaskSystem& owner,
                     std::size_t jobCount,
                     std::function<void(std::size_t)>&& fn,
                     std::size_t batchSize,
                     std::size_t depCapacity);
        void Reset(std::size_t jobCount,
                   std::function<void(std::size_t)>&& fn,
                   std::size_t batchSize,
                   std::size_t depCapacity);
        void Execute() override;

        std::function<void(std::size_t)> fn_;
        std::size_t jobCount_;
        std::size_t batchSize_;
        std::mutex chunkMutex_;
        std::condition_variable chunkCv_;
        // Outstanding chunk count guarded by the task ref count so captured lambdas
        // keep the range task (and its synchronization primitives) alive.
        std::atomic<std::size_t> pendingChunks_{0};
    };

    ~TaskSystem();

    LambdaTaskSet* AcquireLambdaTask(Task& fn, std::size_t depCapacity);
    RangeTaskSet* AcquireRangeTask(std::size_t jobCount,
                                   std::function<void(std::size_t)>& fn,
                                   std::size_t batchSize,
                                   std::size_t depCapacity);
    void RecycleTask(TaskWithDeps* task);
    void RecycleLambdaTask(LambdaTaskSet* task);
    void RecycleRangeTask(RangeTaskSet* task);
    void ClearLambdaPool();
    void ClearRangePool();

    void Retain(TaskWithDeps* task);
    void ReleaseRef(TaskWithDeps* task);
    void Schedule(TaskWithDeps* task);
    void OnDependencyComplete(TaskWithDeps* dependent);

    void RunTask(TaskWithDeps* task);
    void FinishTask(TaskWithDeps* task);
    void WorkerLoop(std::size_t index);
    void WaitForWork();
    bool RunInlineTask();
    std::size_t NextPowerOfTwo(std::size_t value) const;

    class LockFreeQueue;

    std::unique_ptr<LockFreeQueue> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_{false};
    // A task is outstanding from Submit until FinishTask; its execution window is
    // strictly inside that, so this single counter covers the WaitForAll predicate.
    // Hot atomics below are cache-line separated: each is hammered by a different
    // mix of threads (submit/finish vs push/pop-notify vs pool acquire/recycle).
    alignas(64) std::atomic<std::size_t> outstandingTasks_{0};
    mutable std::mutex waitMutex_;
    std::condition_variable waitCv_;
    std::mutex trackedFrameMutex_;
    std::vector<TaskHandle> trackedFrameTasks_;
    std::atomic<std::size_t> workerCount_{0};
    alignas(64) std::atomic<std::size_t> availableTasks_{0};
    // Treiber-stack freelists. Heads are tagged pointers: low 48 bits = node,
    // high 16 bits = pop counter, guarding against ABA (pop A / pop B / push A
    // between another thread's load and CAS).
    alignas(64) std::atomic<std::uintptr_t> lambdaPool_{0};
    alignas(64) std::atomic<std::uintptr_t> rangePool_{0};

    static thread_local std::size_t workerIndex_;
};

#endif // TASKSYSTEM_USE_LOCKFREE

