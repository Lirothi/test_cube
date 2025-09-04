#pragma once
#include <vector>
#include <functional>
#include <mutex>
#include "TaskSystem.h"

// Simple global batcher for UpdateCBField copy operations.
// Collects small copy tasks and submits them to TaskSystem when
// the batch reaches a predefined limit.
class CBUpdateBatcher {
public:
    static CBUpdateBatcher& Get()
    {
        static CBUpdateBatcher instance;
        return instance;
    }

    // Queue copy task. When enough tasks are collected, they are
    // executed asynchronously on the TaskSystem.
    void Enqueue(std::function<void()> fn)
    {
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            tasks_.emplace_back(std::move(fn));
            if (tasks_.size() >= kBatchSize)
            {
                batch.swap(tasks_);
            }
        }

        if (!batch.empty())
        {
            TaskSystem::Get().Submit([batch = std::move(batch)]() mutable {
                for (auto& t : batch) { t(); }
            });
        }
    }

    // Force execution of pending tasks.
    void Flush()
    {
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            batch.swap(tasks_);
        }

        if (!batch.empty())
        {
            TaskSystem::Get().Submit([batch = std::move(batch)]() mutable {
                for (auto& t : batch) { t(); }
            });
        }
    }

private:
    CBUpdateBatcher() = default;
    CBUpdateBatcher(const CBUpdateBatcher&) = delete;
    CBUpdateBatcher& operator=(const CBUpdateBatcher&) = delete;

    static constexpr std::size_t kBatchSize = 32; // threshold
    std::mutex mtx_;
    std::vector<std::function<void()>> tasks_;
};

