#pragma once

#include "MoveOnlyFunction.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

class LocalTaskQueue
{
public:
    using Task = MoveOnlyFunction<void()>;

    void PushOwner(Task task)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_front(std::move(task));
    }

    bool TryPopOwner(Task& task)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
        {
            return false;
        }
        task = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    bool TrySteal(Task& task)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
        {
            return false;
        }
        task = std::move(m_queue.back());
        m_queue.pop_back();
        return true;
    }

    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    std::size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<Task> m_queue;
};
