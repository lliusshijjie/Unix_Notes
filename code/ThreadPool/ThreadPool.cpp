#include "ThreadPool.h"

ThreadPool::ThreadPool(std::size_t worker_count,
                       std::size_t queue_capacity,
                       RejectPolicy reject_policy)
    : queue_capacity(queue_capacity)
    , reject_policy(reject_policy)
    , stopping(false)
{
    if (worker_count == 0)
    {
        throw std::runtime_error("worker_count must be greater than 0");
    }
    if (queue_capacity == 0)
    {
        throw std::runtime_error("queue_capacity must be greater than 0");
    }

    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
        workers.emplace_back([this]() {
            for (;;)
            {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    not_empty.wait(lock, [this]() {
                        return stopping || !tasks.empty();
                    });
                    if (stopping && tasks.empty())
                    {
                        return;
                    }
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                not_full.notify_one();
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(mutex);
        stopping = true;
    }
    not_empty.notify_all();
    not_full.notify_all();
    for (std::thread& worker : workers)
    {
        worker.join();
    }
}

SubmitResult ThreadPool::try_post(Task task)
{
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (stopping)
        {
            return SubmitResult::pool_stopping;
        }
        if (tasks.size() >= queue_capacity)
        {
            return SubmitResult::queue_full;
        }
        tasks.emplace(std::move(task));
    }
    not_empty.notify_one();
    return SubmitResult::accepted;
}

void ThreadPool::post_wait(Task task)
{
    {
        std::unique_lock<std::mutex> lock(mutex);
        not_full.wait(lock, [this]() {
            return stopping || tasks.size() < queue_capacity;
        });
        if (stopping)
        {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        tasks.emplace(std::move(task));
    }
    not_empty.notify_one();
}

void ThreadPool::post(Task task)
{
    bool accepted = false;
    bool run_here = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (stopping)
        {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        if (tasks.size() >= queue_capacity)
        {
            if (reject_policy == RejectPolicy::abort)
            {
                throw std::runtime_error("task queue is full");
            }
            if (reject_policy == RejectPolicy::discard)
            {
                return;
            }
            run_here = true;
        }
        else
        {
            tasks.emplace(std::move(task));
            accepted = true;
        }
    }
    if (accepted)
    {
        not_empty.notify_one();
        return;
    }
    if (run_here)
    {
        task();
    }
}
