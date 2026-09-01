#include "LogQueue.h"

#include <cstring>
#include <new>

namespace logging
{
    namespace
    {
        [[nodiscard]] std::size_t RoundUpPowerOfTwo(std::size_t value) noexcept
        {
            std::size_t result = 16;
            while (result < value && result < (std::size_t{ 1 } << 40))
            {
                result <<= 1;
            }
            return result;
        }
    }

    LogQueue::~LogQueue()
    {
        Destroy();
    }

    bool LogQueue::Initialize(std::size_t capacity) noexcept
    {
        Destroy();
        const std::size_t rounded = RoundUpPowerOfTwo(capacity);
        Slot* slots = new (std::nothrow) Slot[rounded];
        if (slots == nullptr)
        {
            return false;
        }
        for (std::size_t i = 0; i < rounded; ++i)
        {
            slots[i].sequence.store(i, std::memory_order_relaxed);
        }
        slots_ = slots;
        mask_ = rounded - 1;
        enqueuePos_.store(0, std::memory_order_relaxed);
        dequeuePos_.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return true;
    }

    void LogQueue::Destroy() noexcept
    {
        delete[] slots_;
        slots_ = nullptr;
        mask_ = 0;
    }

    bool LogQueue::TryPush(const LogRecord& record) noexcept
    {
        if (slots_ == nullptr)
        {
            return false;
        }

        std::size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        Slot* slot = nullptr;
        for (;;)
        {
            slot = &slots_[pos & mask_];
            const std::size_t sequence = slot->sequence.load(std::memory_order_acquire);
            const std::intptr_t difference =
                static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(pos);
            if (difference == 0)
            {
                // The CAS is a sequentially consistent RMW on purpose: the consumer's "may I
                // sleep" check reads enqueuePos_ after publishing its idle flag, and the producer
                // reads that flag after this CAS. Both being seq_cst rules out a lost wake-up.
                if (enqueuePos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (difference < 0)
            {
                return false; // ring full: the slot still holds an unconsumed record
            }
            else
            {
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }

        std::memcpy(&slot->record, &record, UsedRecordBytes(record));
        slot->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool LogQueue::TryPop(LogRecord& record) noexcept
    {
        if (slots_ == nullptr)
        {
            return false;
        }

        const std::size_t pos = dequeuePos_.load(std::memory_order_relaxed);
        Slot* slot = &slots_[pos & mask_];
        const std::size_t sequence = slot->sequence.load(std::memory_order_acquire);
        const std::intptr_t difference =
            static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(pos + 1);
        if (difference != 0)
        {
            return false; // claimed but not yet published, or simply empty
        }

        std::memcpy(&record, &slot->record, UsedRecordBytes(slot->record));
        slot->sequence.store(pos + mask_ + 1, std::memory_order_release);
        dequeuePos_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    std::size_t LogQueue::ApproximateSize() const noexcept
    {
        const std::size_t enqueue = enqueuePos_.load(std::memory_order_seq_cst);
        const std::size_t dequeue = dequeuePos_.load(std::memory_order_relaxed);
        return enqueue >= dequeue ? enqueue - dequeue : 0;
    }
}
