#pragma once

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

template <typename T>
class RingBuffer
{
public:
    explicit RingBuffer(std::size_t capacity)
        : buffer_(capacity)
        , capacity_(capacity)
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("RingBuffer capacity must be greater than 0");
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    template <typename... Args>
    void emplace(Args&&... args)
    {
        if (full())
        {
            throw std::logic_error("RingBuffer is full");
        }

        buffer_[tail_].emplace(std::forward<Args>(args)...);
        tail_ = next_index(tail_);
        ++size_;
    }

    T& front()
    {
        if (empty())
        {
            throw std::logic_error("RingBuffer is empty");
        }

        return *buffer_[head_];
    }

    const T& front() const
    {
        if (empty())
        {
            throw std::logic_error("RingBuffer is empty");
        }

        return *buffer_[head_];
    }

    void pop_front()
    {
        if (empty())
        {
            throw std::logic_error("RingBuffer is empty");
        }

        buffer_[head_].reset();
        head_ = next_index(head_);
        --size_;
    }

    T pop()
    {
        T value = std::move(front());
        pop_front();
        return value;
    }

    std::size_t push_batch(std::span<const T> values)
    {
        const std::size_t available = capacity_ - size_;
        const std::size_t pushed = values.size() <= available ? values.size() : available;

        for (std::size_t i = 0; i < pushed; ++i)
        {
            buffer_[tail_].emplace(values[i]);
            tail_ = next_index(tail_);
        }

        size_ += pushed;
        return pushed;
    }

    std::size_t pop_batch(std::span<T> output)
    {
        const std::size_t popped = output.size() <= size_ ? output.size() : size_;

        for (std::size_t i = 0; i < popped; ++i)
        {
            output[i] = std::move(*buffer_[head_]);
            buffer_[head_].reset();
            head_ = next_index(head_);
        }

        size_ -= popped;
        return popped;
    }

    bool empty() const noexcept
    {
        return size_ == 0;
    }

    bool full() const noexcept
    {
        return size_ == capacity_;
    }

    std::size_t size() const noexcept
    {
        return size_;
    }

    std::size_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    std::size_t next_index(std::size_t index) const noexcept
    {
        return (index + 1) % capacity_;
    }

    std::vector<std::optional<T>> buffer_;
    std::size_t capacity_;
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
};

template <typename T>
class BlockingQueue
{
public:
    explicit BlockingQueue(std::size_t capacity)
        : capacity_(capacity)
        , buffer_(capacity)
        , closed_(false)
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("BlockingQueue capacity must be greater than 0");
        }
    }

    ~BlockingQueue() = default;

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    BlockingQueue(BlockingQueue&&) = delete;
    BlockingQueue& operator=(BlockingQueue&&) = delete;

    bool push(const T& value)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        not_full_.wait(lock, [this] {
            return closed_ || !buffer_.full();
        });

        if (closed_)
        {
            return false;
        }

        buffer_.emplace(value);

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    bool push(T&& value)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        not_full_.wait(lock, [this] {
            return closed_ || !buffer_.full();
        });

        if (closed_)
        {
            return false;
        }

        buffer_.emplace(std::move(value));

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    template <typename... Args>
    bool emplace(Args&&... args)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (!wait_not_full(lock))
        {
            return false;
        }

        buffer_.emplace(std::forward<Args>(args)...);

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    bool pop(T& value)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        not_empty_.wait(lock, [this] {
            return closed_ || !buffer_.empty();
        });

        if (buffer_.empty())
        {
            return false;
        }

        value = std::move(buffer_.front());
        buffer_.pop_front();

        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    bool try_push(const T& value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_ || buffer_.full())
            {
                return false;
            }

            buffer_.emplace(value);
        }

        not_empty_.notify_one();
        return true;
    }

    bool try_push(T&& value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_ || buffer_.full())
            {
                return false;
            }

            buffer_.emplace(std::move(value));
        }

        not_empty_.notify_one();
        return true;
    }

    template <typename... Args>
    bool try_emplace(Args&&... args)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_ || buffer_.full())
            {
                return false;
            }

            buffer_.emplace(std::forward<Args>(args)...);
        }

        not_empty_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool push_for(const T& value, const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        if (!wait_not_full_until(lock, deadline))
        {
            return false;
        }

        buffer_.emplace(value);

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool push_for(T&& value, const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        if (!wait_not_full_until(lock, deadline))
        {
            return false;
        }

        buffer_.emplace(std::move(value));

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    template <typename Rep, typename Period, typename... Args>
    bool emplace_for(const std::chrono::duration<Rep, Period>& timeout, Args&&... args)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        if (!wait_not_full_until(lock, deadline))
        {
            return false;
        }

        buffer_.emplace(std::forward<Args>(args)...);

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    bool try_pop(T& value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (buffer_.empty())
            {
                return false;
            }

            value = std::move(buffer_.front());
            buffer_.pop_front();
        }

        not_full_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool pop_for(T& value, const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        if (!wait_not_empty_until(lock, deadline))
        {
            return false;
        }

        value = std::move(buffer_.front());
        buffer_.pop_front();

        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    std::size_t push_batch(std::span<const T> values)
    {
        std::size_t pushed = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);

            not_full_.wait(lock, [this, values] {
                return closed_ || values.empty() || buffer_.size() < buffer_.capacity();
            });

            if (values.empty() || closed_)
            {
                return 0;
            }

            pushed = buffer_.push_batch(values);
        }

        if (pushed > 0)
        {
            not_empty_.notify_one();
        }
        return pushed;
    }

    std::size_t pop_batch(std::span<T> output)
    {
        std::size_t popped = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);

            not_empty_.wait(lock, [this, output] {
                return closed_ || output.empty() || !buffer_.empty();
            });

            if (output.empty())
            {
                return 0;
            }

            popped = buffer_.pop_batch(output);
        }

        if (popped > 0)
        {
            not_full_.notify_one();
        }
        return popped;
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_)
            {
                return;
            }
            closed_ = true;
        }

        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.empty();
    }

    bool full() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.full();
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.size();
    }

    std::size_t capacity() const noexcept
    {
        return capacity_;
    }

private:
    bool wait_not_full(std::unique_lock<std::mutex>& lock)
    {
        not_full_.wait(lock, [this] {
            return closed_ || !buffer_.full();
        });
        return !closed_;
    }

    bool wait_not_full_until(
        std::unique_lock<std::mutex>& lock,
        const std::chrono::steady_clock::time_point& deadline)
    {
        const bool ready = not_full_.wait_until(lock, deadline, [this] {
            return closed_ || !buffer_.full();
        });
        return ready && !closed_;
    }

    bool wait_not_empty_until(
        std::unique_lock<std::mutex>& lock,
        const std::chrono::steady_clock::time_point& deadline)
    {
        const bool ready = not_empty_.wait_until(lock, deadline, [this] {
            return closed_ || !buffer_.empty();
        });
        return ready && !buffer_.empty();
    }

    std::size_t capacity_;
    RingBuffer<T> buffer_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;

    bool closed_;
};
