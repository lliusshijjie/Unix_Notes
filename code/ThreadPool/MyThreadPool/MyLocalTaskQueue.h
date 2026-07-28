#pragma once

#include <functional>
#include <mutex>
#include <deque>

class MyLocalTaskQueue {
public:
    using Task = std::function<void()>;

    // Push a task to the front of the queue
    void PushOwner(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_front(std::move(task));
    }

    // Try to pop a task from the front of the queue
    bool TryPopOwner(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }

        task = std::move(queue_.front());
        queue_.pop_front();
        
        return true;
    }

    // Try to steal a task from the back of the queue
    bool TrySteal(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }

        task = std::move(queue_.back());
        queue_.pop_back();

        return true;
    }

    bool Empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t Size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::deque<Task> queue_;
    mutable std::mutex mutex_;
};