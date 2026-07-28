#pragma once

template <class T>
class MyBoundedRingQueue {
public:
    explicit MyBoundedRingQueue(std::size_t capacity) 
        : capacity_(capacity)
        , head_(0)
        , tail_(0)
        , size_(0)
        , buffer_(capacity) {
        if (capacity_ == 0) {
            throw std::runtime_error("Queue capacity must be greater than 0");
        }
    }

    MyBoundedRingQueue(const MyBoundedRingQueue&) = delete;
    MyBoundedRingQueue& operator=(const MyBoundedRingQueue&) = delete;

    MyBoundedRingQueue(MyBoundedRingQueue&&) = default;
    MyBoundedRingQueue& operator=(MyBoundedRingQueue&&) = default;

    bool empty() const noexcept {
        return size_ == 0;
    }

    bool full() const noexcept {
        return size_ >= capacity_;
    }

    std::size_t size() const noexcept {
        return size_;
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    void push(T&& value) {
        if (full()) {
            throw std::runtime_error("Queue is full");
        }

        buffer_[tail_] = std::move(value);
        tail_ = (tail_ + 1) % capacity_;
        size_++;
    }

    T pop() {
        if (empty()) {
            throw std::runtime_error("Queue is empty");
        }

        T value = std::move(buffer_[head_]);
        buffer_[head_] = T(); // Reset the slot to default value
        head_ = (head_ + 1) % capacity_;
        size_--;

        return value;
    }

    void clear() {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
        buffer_.clear();
        buffer_.resize(capacity_);
    }

private:
    std::size_t capacity_;
    std::size_t head_;
    std::size_t tail_;
    std::size_t size_;

    std::vector<T> buffer_;
};