#pragma once

#ifdef TASKSYSTEM_USE_LOCKFREE

#include <array>
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

    // How many tasks may depend on one task (TaskWithDeps::dependents_). One more is a broken invariant in every build
    // (SetDependencies); --tasksystem-stress --stress-overflow registers exactly that many and then one more.
    // 8: the render graph's Pass_BuildAS recording task fans out to every RT pass that reads the
    // TLAS (RTTrace, the RT resolve, the RT debug view, the glass reflections, the sky visibility,
    // the GI probe trace) -- five of them at once overflowed 4 and aborted the first frame.
    static constexpr std::size_t kDependentCapacity = 8;

    enum class TaskKind { Lambda, Range };

    // What a task's exception means (docs/architecture_review_plan.md R15.3; the owner's answer В6, 2026-11-24). The
    // creator decides (CreateTask); a task created while another task runs on this thread -- a range's chunks, a pass
    // body's fan-out -- takes that task's policy and name. The thread's role and TrackFrameTask decide nothing here.
    enum class FailurePolicy : std::uint8_t {
        // Its result is optional to the process: an Error line, and the task completes as FAILED -- its waiter gets
        // false from Wait (Failed() is true), its dependents still run. The process goes on.
        Background,
        // A frame cannot go on without it (a render-graph pass: its command list unclosed, its barriers never
        // issued): a Fatal line naming the task, then abort and the minidump (InvariantFailure).
        Frame,
    };

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
        tc::inl_vector<TaskHandle, kDependentCapacity> dependents_;
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
        // The thread role current where it was created (game thread plan section 3.4), the same way.
        std::uint8_t role_ = 0;
        // What an exception out of Execute means (FailurePolicy), and the name a Frame task's Fatal line gives --
        // static storage (a render pass's name literal). Set before the task can run; inherited from the task
        // running where it was created.
        FailurePolicy failurePolicy_ = FailurePolicy::Background;
        const wchar_t* name_ = nullptr;
        // Execute threw (or, for a range, a chunk did): written by the executing thread before completed_'s release
        // store, read by a waiter after its acquire load.
        bool failed_ = false;

    protected:
        void InheritFailurePolicy();
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
    // A task whose exception means `policy`; `name` (static storage, may be null) is what its Fatal or Error line
    // calls it. The render graph creates its pass tasks this way (FailurePolicy::Frame, the pass's name).
    TaskHandle CreateTask(Task t, std::size_t depCount, FailurePolicy policy, const wchar_t* name);
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
    // False when a chunk threw (FailurePolicy::Background).
    bool DispatchWait(std::size_t jobCount,
                      std::function<void(std::size_t)> fn,
                      std::size_t batchSize = 1);

    void SubmitDetach(Task t);
    void DispatchDetach(std::size_t jobCount,
                        std::function<void(std::size_t)> fn,
                        std::size_t batchSize = 1);

    // May execute this task or its explicit prerequisites, never unrelated work.
    // Callers must own the handle until Wait returns; dependency graphs are acyclic.
    // Returns false when the task completed as failed (its body threw; FailurePolicy::Background) -- true for a
    // null handle and for a task that finished its work.
    bool Wait(TaskHandle handle);
    // After the task completed: did it fail (see Wait)? False for a null or unfinished handle.
    [[nodiscard]] bool Failed(TaskHandle handle) const;
    void Release(TaskHandle& handle);

    // Transfers the caller's reference. Waiting also drains tasks registered by
    // those tasks, without holding the registration mutex across user callbacks.
    // One list per thread role (core/threading/ThreadRole.h): a task joins its creator's list, and a
    // wait joins the caller's -- the game half never waits on the render half's work, nor the reverse.
    void TrackFrameTask(TaskHandle handle);
    void WaitForTrackedAsyncTasks();

    // False when a chunk threw (see DispatchWait).
    template<class F>
    static bool ParallelFor(std::size_t jobCount, F&& fn, std::size_t batchSize)
    {
        return Get().DispatchWait(jobCount, std::forward<F>(fn), batchSize);
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
    void OnTaskThrew(TaskWithDeps* task, const char* what);
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
    std::array<std::vector<TaskHandle>, 4> trackedFrameTasks_; // by threading::ThreadRole
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

