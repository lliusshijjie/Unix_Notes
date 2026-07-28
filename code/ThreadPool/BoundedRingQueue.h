#pragma once

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

template <class T>
class BoundedRingQueue
{
public:
    explicit BoundedRingQueue(std::size_t capacity)
        : m_buffer(capacity)
        , m_capacity(capacity)
        , m_head(0)
        , m_tail(0)
        , m_size(0)
    {
        if (capacity == 0)
        {
            throw std::runtime_error("queue capacity must be greater than 0");
        }
    }

    BoundedRingQueue(const BoundedRingQueue&) = delete;
    BoundedRingQueue& operator=(const BoundedRingQueue&) = delete;

    BoundedRingQueue(BoundedRingQueue&&) = default;
    BoundedRingQueue& operator=(BoundedRingQueue&&) = default;

    bool empty() const noexcept
    {
        return m_size == 0;
    }

    bool full() const noexcept
    {
        return m_size == m_capacity;
    }

    std::size_t size() const noexcept
    {
        return m_size;
    }

    std::size_t capacity() const noexcept
    {
        return m_capacity;
    }

    void push(T&& value)
    {
        if (full())
        {
            throw std::runtime_error("queue is full");
        }
        m_buffer[m_tail] = std::move(value);
        m_tail = (m_tail + 1) % m_capacity;
        ++m_size;
    }

    T pop()
    {
        if (empty())
        {
            throw std::runtime_error("queue is empty");
        }
        T value = std::move(m_buffer[m_head]);
        m_buffer[m_head] = T();
        m_head = (m_head + 1) % m_capacity;
        --m_size;
        return value;
    }

    void clear()
    {
        while (!empty())
        {
            pop();
        }
    }

private:
    std::vector<T> m_buffer;
    std::size_t m_capacity;
    std::size_t m_head;
    std::size_t m_tail;
    std::size_t m_size;
};
