#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

template <class T>
class MpscRingBuffer {
public:
    explicit MpscRingBuffer(std::size_t capacity)
        : capacity_(capacity)
        , cells_(std::make_unique<Cell[]>(capacity)) {
        if (capacity == 0) {
            throw std::invalid_argument("MpscRingBuffer capacity must be positive");               
        }
        for (std::size_t index = 0; index < capacity_; index++) {
            cells_[index].sequence.store(index, std::memory_order_relaxed);
        }
    }

    MpscRingBuffer(const MpscRingBuffer&) = delete;
    MpscRingBuffer& operator=(const MpscRingBuffer&) = delete;

    bool try_enqueue(T&& value) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<T>);
        static_assert(std::is_nothrow_move_assignable_v<T>);

        std::size_t position = enqueue_position_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[position % capacity_];
            const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t difference = static_cast<std::ptrdiff_t>(sequence) - 
                                            static_cast<std::ptrdiff_t>(position);
            if (difference == 0) {
                if (enqueue_position_.compare_exchange_weak(position, position + 1, 
                                                            std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
                    cell.value = std::move(value);
                    cell.sequence.store(position + 1, std::memory_order_release);
                    return true;                                            
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_position_.load(std::memory_order_relaxed);
            }
        }
    }

    bool try_dequeue(T& value) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<T>);
        static_assert(std::is_nothrow_move_assignable_v<T>);

        const std::size_t position = dequeue_position_.load(std::memory_order_relaxed);
        Cell& cell = cells_[position % capacity_];
        const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
        if (sequence != position + 1) {
            return false;
        }

        value = std::move(cell.value);
        cell.sequence.store(position + capacity_, std::memory_order_release);
        dequeue_position_.store(position + 1, std::memory_order_release);
        return true;
    }

    bool full() const noexcept {
        const std::size_t produced = enqueue_position_.load(std::memory_order_acquire);
        const std::size_t consumed = dequeue_position_.load(std::memory_order_acquire);
        return produced - consumed >= capacity_;
    }
private:
    struct alignas(64) Cell {
        std::atomic<std::size_t> sequence{0};
        T value{};
    };

    const std::size_t capacity_;
    std::unique_ptr<Cell[]> cells_;
    std::atomic<std::size_t> enqueue_position_{0};
    std::atomic<std::size_t> dequeue_position_{0};
};