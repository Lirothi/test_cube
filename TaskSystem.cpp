#include "TaskSystem.h"
#include <algorithm>
#include <atomic>

TaskSystem& TaskSystem::Get() {
    static TaskSystem g;
    return g;
}

TaskSystem::~TaskSystem() {
    Stop();
}

void TaskSystem::Start(unsigned threadCount) {
    if (threadCount == 0) {
        scheduler_.Initialize();
    } else {
        scheduler_.Initialize(threadCount + 1); // include main thread
    }
}

void TaskSystem::Stop() {
    scheduler_.WaitforAllAndShutdown();
}

namespace {
struct LambdaTaskSet : enki::ITaskSet {
    TaskSystem::Task fn;
    TaskGroup* group;
    bool autoDelete;
    LambdaTaskSet(TaskSystem::Task&& f, TaskGroup* g, bool ad)
        : enki::ITaskSet(1), fn(std::move(f)), group(g), autoDelete(ad) {}
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        if (fn) {
            fn();
        }
        if (autoDelete) {
            delete this;
        }
    }
};

struct RangeTaskSet : enki::ITaskSet {
    std::function<void(std::size_t)> fn;
    std::atomic<uint32_t> completed{0};
    bool autoDelete;
    RangeTaskSet(std::size_t count, std::function<void(std::size_t)> f, bool ad)
        : enki::ITaskSet(static_cast<uint32_t>(count)), fn(std::move(f)), autoDelete(ad) {}
    void ExecuteRange(enki::TaskSetPartition range, uint32_t) override {
        for (uint32_t i = range.start; i < range.end; ++i) {
            fn(i);
        }
        if (autoDelete) {
            uint32_t done = completed.fetch_add(range.end - range.start, std::memory_order_acq_rel) +
                            (range.end - range.start);
            if (done == m_SetSize) {
                delete this;
            }
        }
    }
};
}

void TaskSystem::Submit(const Task& t, TaskGroup* group) { Submit(Task(t), group); }

void TaskSystem::Submit(Task&& t, TaskGroup* group) {
    if (group) {
        auto taskPtr = std::make_shared<LambdaTaskSet>(std::move(t), group, false);
        group->tasks.push_back(taskPtr);
        scheduler_.AddTaskSetToPipe(taskPtr.get());
    } else {
        auto* taskPtr = new LambdaTaskSet(std::move(t), nullptr, true);
        scheduler_.AddTaskSetToPipe(taskPtr);
    }
}

void TaskSystem::Dispatch(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize,
                          TaskGroup* group) {
    if (jobCount == 0 || !fn) { return; }
    if (batchSize == 0) { batchSize = 1; }
    if (group) {
        auto taskPtr = std::make_shared<RangeTaskSet>(jobCount, std::move(fn), false);
        taskPtr->m_MinRange = static_cast<uint32_t>(batchSize);
        group->tasks.push_back(taskPtr);
        scheduler_.AddTaskSetToPipe(taskPtr.get());
    } else {
        auto* taskPtr = new RangeTaskSet(jobCount, std::move(fn), true);
        taskPtr->m_MinRange = static_cast<uint32_t>(batchSize);
        scheduler_.AddTaskSetToPipe(taskPtr);
    }
}

void TaskSystem::WaitGroup(TaskGroup* group) {
    if (!group) { return; }
    for (auto& t : group->tasks) {
        scheduler_.WaitforTask(t.get());
    }
    group->tasks.clear();
}

void TaskSystem::WaitForAll() {
    scheduler_.WaitforAll();
}

std::size_t TaskSystem::ThreadIndex() const {
    return scheduler_.GetThreadNum();
}

