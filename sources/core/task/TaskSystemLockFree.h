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
        // Only the overflow publisher / detached-batch consumer touches this link.
        // The ready queue owns a reference until that consumer releases the entry.
        TaskWithDeps* nextReady_{nullptr};
        std::atomic<std::size_t> pendingDeps_{0};
        tc::inl_vector<TaskHandle, 4> dependents_;
        // Immutable after dependency setup; owns references for targeted helping.
        std::vector<TaskHandle> prerequisites_;
        // Completion flag, waited on via C++20 atomic wait. Waiters must hold a
        // reference to the handle so the task cannot be recycled under them.
        std::atomic<std::uint32_t> completed_{0};
        std::atomic<bool> submitted_{false};
        std::atomic<bool> scheduled_{false};
        std::atomic<bool> ready_{false};
        std::atomic<bool> executionClaimed_{false};
        std::atomic<int> refCount_{1};
        // The MemScope tag current where the task was created (memory profiling plan M3): the worker
        // runs the body under it, so a scope around a fan-out owns what the workers allocate.
        std::uint8_t memTag_ = 0;

    protected:
        void Prepare(std::size_t depCapacity);
    };

    using Task = std::function<void()>;

    static TaskSystem& Get();

    // Lifecycle calls are externally serialized; external producers must stop
    // submitting before Stop. Work spawned by already-running tasks is drained.
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

    // May execute this task or its explicit prerequisites, never unrelated work.
    // Callers must own the handle until Wait returns; dependency graphs are acyclic.
    void Wait(TaskHandle handle);
    void Release(TaskHandle& handle);

    // Transfers the caller's reference. Waiting also drains tasks registered by
    // those tasks, without holding the registration mutex across user callbacks.
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
        // Execute's child chunks. Kept with the pooled task for its capacity: a local vector was an
        // allocation per dispatch.
        std::vector<TaskHandle> chunks_;
    };

    TaskSystem();
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
    bool TryRunTask(TaskWithDeps* task);
    bool HelpTask(TaskWithDeps* task);
    void FinishTask(TaskWithDeps* task);
    void WorkerLoop(std::size_t index);
    std::size_t NextPowerOfTwo(std::size_t value) const;

    class LockFreeQueue;

    std::unique_ptr<LockFreeQueue> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_{false};
    // A task is outstanding from Submit until FinishTask; its execution window is
    // strictly inside that, so this single counter covers the WaitForAll predicate.
    // Submission/completion, wakeups and overflow publication use separate lines.
    alignas(64) std::atomic<std::size_t> outstandingTasks_{0};
    mutable std::mutex waitMutex_;
    std::condition_variable waitCv_;
    std::mutex trackedFrameMutex_;
    std::vector<TaskHandle> trackedFrameTasks_;
    // An epoch (not an item count) also changes at shutdown, so atomic::wait
    // cannot miss a wake when all queued entries have already been claimed.
    alignas(64) std::atomic<std::uint64_t> workEpoch_{0};
    // Full-ring fallback: MPSC push + atomic whole-list detach. No speculative
    // reads of a node's link, no finite pointer tag, and no producer waits for space.
    alignas(64) std::atomic<TaskWithDeps*> overflow_{nullptr};
    // Bounded MPMC pointer caches; excess recycled nodes are deleted. A consumer
    // claims the queue cell before reading its pointer, never a speculative node.
    std::unique_ptr<LockFreeQueue> lambdaPool_;
    std::unique_ptr<LockFreeQueue> rangePool_;

    static thread_local std::size_t workerIndex_;
    static thread_local TaskWithDeps* currentTask_;
};

#endif // TASKSYSTEM_USE_LOCKFREE

