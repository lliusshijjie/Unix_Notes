#pragma once

#include <vector>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

template <typename T>
class MyRingBuffer {
public:
    explicit MyRingBuffer(std::size_t capacity) 
    : buffer_(capacity)
    , capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("RingBuffer capacity must be greater than 0");
        }
    }

    MyRingBuffer(const MyRingBuffer&) = delete;
    MyRingBuffer& operator=(const MyRingBuffer&) = delete;
    MyRingBuffer(MyRingBuffer&&) = delete;
    MyRingBuffer& operator=(MyRingBuffer&&) = delete;

    bool empty() const noexcept {
        return size_ == 0;
    }

    bool full() const noexcept {
        return size_ == capacity_;
    }

    std::size_t size() const noexcept {
        return size_;
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    template <typename... Args> 
    void emplace(Args&&... args) {
        if (full()) {
            throw std::logic_error("RingBuffer is full!");
        }

        buffer_[tail_].emplace(std::forward<Args>(args)...);
        tail_ = (tail_ + 1) % capacity_;
        size_++;
    } 

    T& front() {
        if (empty()) {
            throw std::logic_error("RingBuffer is empty!");
        }

        return *buffer_[head_];
    }

    const T& front() const {
        if (empty()) {
            throw std::logic_error("RingBuffer is empty!");
        }

        return *buffer_[head_];
    }

    void pop_front() {
        if (empty()) {
            throw std::logic_error("RingBuffer is empty!");
        }

        buffer_[head_].reset();
        head_ = (head_ + 1) % capacity_;
        size_--;
    }

    T pop() {
        T value = std::move(front());
        pop_front();
        return value;
    }

    std::size_t push_batch(std::span<const T> values) {
        const std::size_t available = capacity_ - size_;
        const std::size_t pushed = std::min(values.size(), available);

        for (std::size_t i = 0; i < pushed; i++) {
            buffer_[tail_].emplace(values[i]);
            tail_ = (tail_ + 1) % capacity_;
        }

        size_ += pushed;
        return pushed;
    }

    std::size_t pop_batch(std::span<T> output) {
        const std::size_t popped = std::min(output.size(), size_);
        for (std::size_t i = 0; i < popped; i++) {
            output[i] = std::move(*buffer_[head_]);
            buffer_[head_].reset();
            head_ = (head_ + 1) % capacity_;
        }

        size_ -= popped;
        return popped;
    }

private:
    std::size_t capacity_;
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};

    std::vector<std::optional<T>> buffer_;
};