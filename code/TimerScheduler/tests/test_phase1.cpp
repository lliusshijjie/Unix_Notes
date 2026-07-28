#include "../TimerScheduler.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <mutex>
#include "../ThreadPool/ThreadPool.h"

namespace
{

int g_failed = 0;

void Expect(bool cond, const char* name)
{
    if (cond)
    {
        std::cout << "[PASS] " << name << '\n';
    }
    else
    {
        std::cout << "[FAIL] " << name << '\n';
        ++g_failed;
    }
}

void TestSingleTask()
{
    ThreadPool pool(2, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    std::atomic<bool> executed{false};
    scheduler.schedule_after(std::chrono::milliseconds(50), [&]{
        executed.store(true, std::memory_order_relaxed);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Expect(executed.load(), "TestSingleTask");
}

void TestOrdering()
{
    ThreadPool pool(2, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    std::vector<int> results;
    std::mutex mtx;
    
    scheduler.schedule_after(std::chrono::milliseconds(150), [&]{
        std::lock_guard<std::mutex> lock(mtx);
        results.push_back(3);
    });
    scheduler.schedule_after(std::chrono::milliseconds(50), [&]{
        std::lock_guard<std::mutex> lock(mtx);
        results.push_back(1);
    });
    scheduler.schedule_after(std::chrono::milliseconds(100), [&]{
        std::lock_guard<std::mutex> lock(mtx);
        results.push_back(2);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    
    std::lock_guard<std::mutex> lock(mtx);
    Expect(results.size() == 3 && results[0] == 1 && results[1] == 2 && results[2] == 3, "TestOrdering");
}

void TestPreemption()
{
    ThreadPool pool(2, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    std::atomic<int> first_executed{0};
    
    scheduler.schedule_after(std::chrono::seconds(5), [&]{
        first_executed.store(2, std::memory_order_relaxed);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler.schedule_after(std::chrono::milliseconds(50), [&]{
        first_executed.store(1, std::memory_order_relaxed);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Expect(first_executed.load() == 1, "TestPreemption");
}

void TestMultithreadSubmit()
{
    ThreadPool pool(4, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    std::atomic<int> counter{0};
    
    std::vector<std::thread> threads;
    constexpr int producer_count = 8;
    constexpr int tasks_per_producer = 50;
    
    for (int i = 0; i < producer_count; ++i) {
        threads.emplace_back([&scheduler, &counter]{
            for (int j = 0; j < tasks_per_producer; ++j) {
                scheduler.schedule_after(std::chrono::milliseconds(10), [&counter]{
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    
    for (auto& t : threads) t.join();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Expect(counter.load() == producer_count * tasks_per_producer, "TestMultithreadSubmit");
}

void TestStopAndEmptyQueue()
{
    ThreadPool pool(2, 1024);
    auto start = std::chrono::steady_clock::now();
    {
        TimerScheduler scheduler(pool);
        scheduler.start();
        // 空任务队列，不会忙等
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // 析构时调用 stop()
    }
    auto duration = std::chrono::steady_clock::now() - start;
    // 确保 stop() 及时唤醒并退出
    Expect(duration < std::chrono::milliseconds(150), "TestStopAndEmptyQueue");
}

void TestExceptionSafety()
{
    ThreadPool pool(2, 1024);
    TimerScheduler scheduler(pool);
    scheduler.start();
    std::atomic<bool> after_executed{false};
    
    scheduler.schedule_after(std::chrono::milliseconds(50), []{
        throw std::runtime_error("Test exception");
    });
    
    scheduler.schedule_after(std::chrono::milliseconds(100), [&]{
        after_executed.store(true, std::memory_order_relaxed);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Expect(after_executed.load(), "TestExceptionSafety");
}

} // namespace

int main()
{
    TestSingleTask();
    TestOrdering();
    TestPreemption();
    TestMultithreadSubmit();
    TestStopAndEmptyQueue();
    TestExceptionSafety();

    if (g_failed == 0)
    {
        std::cout << "ALL PASSED\n";
        return 0;
    }

    std::cout << g_failed << " FAILED\n";
    return 1;
}
