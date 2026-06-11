#pragma once

#ifdef TASKSYSTEM_USE_TBB

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/global_control.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#ifndef TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
#define TASKSYSTEM_ENABLE_PARALLEL_EXECUTION 1
#endif

class TaskSystem {
public:
    struct TaskWithDeps;
    using TaskHandle = TaskWithDeps*;

    struct TaskWithDeps {
        TaskWithDeps(TaskSystem& owner, std::size_t depCapacity);
        virtual ~TaskWithDeps() = default;

        void AddDependent(TaskHandle dependent);
        void IncrementDependency();
        void DependencySatisfied();
        void NotifyDependents();

        virtual void Execute() = 0;

        TaskSystem& owner_;
        std::atomic<std::size_t> pendingDeps_{ 0 };
        std::vector<TaskHandle> dependents_;
        std::mutex dependentsMutex_;
        std::promise<void> completionPromise_;
        std::shared_future<void> completionFuture_;
        std::atomic<bool> submitted_{ false };
        std::atomic<bool> scheduled_{ false };
        std::atomic<int> refCount_{ 1 };
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
        void Execute() override;
        Task fn_;
    };

    struct RangeTaskSet : TaskWithDeps {
        RangeTaskSet(TaskSystem& owner,
                     std::size_t jobCount,
                     std::function<void(std::size_t)> fn,
                     std::size_t batchSize,
                     std::size_t depCapacity);
        void Execute() override;

        std::function<void(std::size_t)> fn_;
        std::size_t jobCount_;
        std::size_t batchSize_;
    };

    void Retain(TaskWithDeps* task);
    void ReleaseRef(TaskWithDeps* task);
    void Schedule(TaskWithDeps* task);
    void OnDependencyComplete(TaskWithDeps* dependent);

    void FinishTask(TaskWithDeps* task);

    tbb::task_arena arena_;
    std::unique_ptr<tbb::global_control> globalControl_;
    std::atomic<bool> started_{false};

    std::mutex activeMutex_;
    std::condition_variable activeCv_;
    std::size_t activeTasks_{0};
    std::atomic<std::size_t> outstandingTasks_{0};

    std::mutex trackedFrameMutex_;
    std::vector<TaskHandle> trackedFrameTasks_;
};

#endif // TASKSYSTEM_USE_TBB

