#include "ThreadPool.h"

thread_local LocalTaskQueue* ThreadPool::tls_local = nullptr;

ThreadPool::ThreadPool(std::size_t worker_count,
                       std::size_t queue_capacity,
                       RejectPolicy reject_policy)
    : tasks(queue_capacity)
    , queue_capacity(queue_capacity)
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

    local_queues.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
        local_queues.emplace_back(std::unique_ptr<LocalTaskQueue>(new LocalTaskQueue()));
    }

    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
        workers.emplace_back([this, i]() { WorkerLoop(i); });
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

void ThreadPool::NotifyWorkers()
{
    not_empty.notify_all();
}

bool ThreadPool::TrySteal(std::size_t worker_index, Task& task)
{
    const std::size_t count = local_queues.size();
    for (std::size_t offset = 1; offset < count; ++offset)
    {
        const std::size_t victim = (worker_index + offset) % count;
        if (local_queues[victim]->TrySteal(task))
        {
            return true;
        }
    }
    return false;
}

bool ThreadPool::HasStealableWork(std::size_t worker_index) const
{
    const std::size_t count = local_queues.size();
    for (std::size_t offset = 1; offset < count; ++offset)
    {
        const std::size_t victim = (worker_index + offset) % count;
        if (!local_queues[victim]->Empty())
        {
            return true;
        }
    }
    return false;
}

bool ThreadPool::AllLocalEmpty() const
{
    for (std::size_t i = 0; i < local_queues.size(); ++i)
    {
        if (!local_queues[i]->Empty())
        {
            return false;
        }
    }
    return true;
}

bool ThreadPool::HasAnyWork(std::size_t worker_index) const
{
    if (!tasks.empty())
    {
        return true;
    }
    if (!local_queues[worker_index]->Empty())
    {
        return true;
    }
    return HasStealableWork(worker_index);
}

void ThreadPool::WorkerLoop(std::size_t worker_index)
{
    tls_local = local_queues[worker_index].get();

    for (;;)
    {
        Task task;

        if (tls_local->TryPopOwner(task))
        {
            task();
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!tasks.empty())
            {
                task = tasks.pop();
                lock.unlock();
                not_full.notify_one();
                task();
                continue;
            }
        }

        if (TrySteal(worker_index, task))
        {
            task();
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            not_empty.wait(lock, [this, worker_index]() {
                return stopping || HasAnyWork(worker_index);
            });
            if (stopping && tasks.empty() && AllLocalEmpty())
            {
                return;
            }
        }
    }
}

SubmitResult ThreadPool::try_post(Task task)
{
    if (tls_local != nullptr)
    {
        tls_local->PushOwner(std::move(task));
        NotifyWorkers();
        return SubmitResult::accepted;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (stopping)
        {
            return SubmitResult::pool_stopping;
        }
        if (tasks.full())
        {
            return SubmitResult::queue_full;
        }
        tasks.push(std::move(task));
    }
    NotifyWorkers();
    return SubmitResult::accepted;
}

void ThreadPool::post_wait(Task task)
{
    if (tls_local != nullptr)
    {
        tls_local->PushOwner(std::move(task));
        NotifyWorkers();
        return;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        not_full.wait(lock, [this]() {
            return stopping || !tasks.full();
        });
        if (stopping)
        {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        tasks.push(std::move(task));
    }
    NotifyWorkers();
}

void ThreadPool::post(Task task)
{
    if (tls_local != nullptr)
    {
        tls_local->PushOwner(std::move(task));
        NotifyWorkers();
        return;
    }

    bool accepted = false;
    bool run_here = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (stopping)
        {
            throw std::runtime_error("submit on stopped ThreadPool");
        }
        if (tasks.full())
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
            tasks.push(std::move(task));
            accepted = true;
        }
    }
    if (accepted)
    {
        NotifyWorkers();
        return;
    }
    if (run_here)
    {
        task();
    }
}
