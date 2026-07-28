#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "MyRingBuffer.h"

template <typename T>
class MyBlockingQueue {
public:
    explicit MyBlockingQueue(std::size_t capacity) 
        : capacity_(capacity)
        , buffer_(capacity)
        , closed_(false) {
        
        if (capacity == 0) {
            throw std::invalid_argument("MyBlockingQueue capacity must be greater than 0");
        }
    }

    ~MyBlockingQueue() = default;

    MyBlockingQueue(const MyBlockingQueue&) = delete;
    MyBlockingQueue& operator=(const MyBlockingQueue&) = delete;

    MyBlockingQueue(MyBlockingQueue&&) = delete;
    MyBlockingQueue& operator=(MyBlockingQueue&&) = delete;

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.empty();
    }

    bool full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.size() >= capacity_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.size();
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    bool push(const T& value) {
        std::unique_lock<std::mutex> lock(mutex_);

        not_full_.wait(lock, [this] {
            return closed_ || buffer_.size() < capacity_;
        });

        if (closed_ || buffer_.size() >= capacity_) {
            return false;
        }

        buffer_.emplace(value);

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    bool push(T&& value) {
        std::unique_lock<std::mutex> lock(mutex_);

        not_full_.wait(lock, [this] {
            return closed_ || buffer_.size() < capacity_;
        });

        if (closed_ || buffer_.size() >= capacity_) {
            return false;
        }

        buffer_.emplace(std::move(value));

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    template <typename... Args>
    bool emplace(Args&&... args) {
        std::unique_lock<std::mutex> lock(mutex_);

        not_full.wait(lock, [this] {
            return closed_ || buffer_.size() < capacity_;
        });

        if (closed_ || buffer_.size() >= capacity_) {
            return false;
        }

        buffer_.emplace(std::forward<Args>(args)...);

        lock.unlock();
        not_empty_.notify_all();
        return true;
    }

    bool try_push(const T& value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_ || buffer_.size() >= capacity_) {
                return false;
            }

            buffer_.emplace(value);
        }

        not_empty_.notify_one();
        return true;
    }

    bool try_push(T&& value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            if (closed_ || buffer_.size() >= capacity_) {
                return false;
            }

            buffer_.emplace(std::move(value));
        }

        not_empty_.notify_one();
        return true;
    }

    template <typename... Args>
    bool try_emplace(Args&&... args) {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_ || buffer_.size() >= capacity_) {
                return false;
            }

            buffer_.emplace(std::forward<Args>(args)...);
        }

        not_empty_.notify_one();
        return true;
    }

    template <typename Rep, typename Period> 
    bool push_for(const T&value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        if (!wait_not_full_until(lock, deadline)) {
            return false;
        }

        buffer_.emplace(value);

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool push_for(T&& value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        if (!wait_not_full_until(lock, deadline)) {
            return false;
        }

        buffer_.emplace(std::move(value));

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    template <typename Rep, typename Period, typename... Args>
    bool emplace_for(const std::chrono::duration<Rep, Period>& timeout, Args&&... args) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        if (!wait_not_full_until(lock, deadline)) {
            return false;
        }

        buffer_.emplace(std::forward<Args>(args)...);

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }
 
    
    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);

        not_empty_.wait(lock, [this] {
            return closed_ || buffer_.size() > 0;
        });

        if (closed_) {
            return false;
        }

        value = std::move(buffer_.front());
        buffer_.pop_front();

        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    bool try_pop(T& value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            if (buffer_.empty()) {
                return false;
            }
            
            value = std::move(buffer_.front());
            buffer_.pop_front();
        }
        
        not_full_.notify_one();
        return true;
    }
    
    template <typename Rep, typename Period>
    bool pop_for(T& value, const std::chrono::duration<Rep, Period>&timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        if (!wait_not_empty_until(lock, deadline)) {
            return false;
        }

        value = std::move(buffer_.front());
        buffer_.pop_front();

        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
        }

        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t push_batch(std::span<const T> values) {
        std::size_t pushed = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            not_full_.wait(lock, [this, values] {
                return closed_ || values.empty() || !full();
            });

            if (values.empty() || closed_) {
                return 0;
            }

            pushed = buffer_.push_batch(values);
        }

        if (pushed > 0) {
            not_empty_.notify_one();
        }

        return pushed;
    }

    std::size_t pop_batch(std::span<T> output) {
        std::size_t popped = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            not_empty_.wait(lock, [this, output] {
                return closed_ || output.empty() || !buffer_.empty();
            });

            if (output.empty()) {
                return 0;
            }

            popped = buffer_.pop_batch(output);
        }

        if (popped > 0) {
            not_full_.notify_one();
        } 

        return popped;
    }


private:
    bool wait_not_full_until(
        std::unique_lock<std::mutex>& lock,
        const std::chrono::steady_clock::time_point& deadline
    ) {
        const bool ready = not_full_.wait_until(
            lock, deadline, [this] {
                return closed_ || buffer_.size() < capacity_;
            }
        );

        return ready && !closed_;
    }

    bool wait_not_empty_until(
        std::unique_lock<std::mutex>& lock,
        const std::chrono::steady_clock::time_point& deadline
    ) {
        const bool ready = not_empty_.wait_until(
            lock, deadline, [this] {
                return closed_ || !buffer_.empty();
            }
        );

        return ready && !buffer_.empty();
    }

    std::size_t capacity_;
    MyRingBuffer<T> buffer_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;

    bool closed_;
};