#include "../ThreadPool.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

void TestBasicExecution()
{
    ThreadPool pool(4, 1024);
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(1000);

    for (int i = 0; i < 1000; ++i)
    {
        futures.push_back(pool.submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futures)
    {
        f.get();
    }

    Expect(counter.load() == 1000, "TestBasicExecution");
}

void TestReturnValue()
{
    ThreadPool pool(4, 1024);

    auto sum = pool.submit([](int a, int b) { return a + b; }, 1, 2);
    Expect(sum.get() == 3, "TestReturnValue_add");

    auto concat = pool.submit([](const std::string& a, const std::string& b) {
        return a + b;
    }, std::string("hello"), std::string("world"));
    Expect(concat.get() == "helloworld", "TestReturnValue_concat");

    std::atomic<bool> called{false};
    auto void_task = pool.submit([&called]() {
        called.store(true, std::memory_order_relaxed);
    });
    void_task.get();
    Expect(called.load(), "TestReturnValue_void");
}

void TestExceptionPropagation()
{
    ThreadPool pool(4, 1024);
    bool submit_threw = false;
    std::future<int> fut;

    try
    {
        fut = pool.submit([]() -> int {
            throw std::runtime_error("task boom");
        });
    }
    catch (...)
    {
        submit_threw = true;
    }

    Expect(!submit_threw, "TestException_submit_no_throw");

    bool get_threw = false;
    try
    {
        fut.get();
    }
    catch (const std::runtime_error& e)
    {
        get_threw = (std::string(e.what()) == "task boom");
    }
    catch (...)
    {
        get_threw = false;
    }
    Expect(get_threw, "TestException_get_throws");

    auto ok = pool.submit([]() { return 42; });
    Expect(ok.get() == 42, "TestException_worker_alive");
}

void TestConcurrentSubmit()
{
    ThreadPool pool(4, 1024);
    constexpr int producer_count = 8;
    constexpr int tasks_per_producer = 200;
    std::atomic<int> counter{0};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);

    for (int p = 0; p < producer_count; ++p)
    {
        producers.emplace_back([&pool, &counter, tasks_per_producer]() {
            for (int i = 0; i < tasks_per_producer; ++i)
            {
                pool.submit([&counter]() {
                    counter.fetch_add(1, std::memory_order_relaxed);
                }).wait();
            }
        });
    }

    for (auto& t : producers)
    {
        t.join();
    }

    Expect(counter.load() == producer_count * tasks_per_producer,
           "TestConcurrentSubmit");
}

void TestDestructorDrain()
{
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4, 1024);
        for (int i = 0; i < 100; ++i)
        {
            pool.submit([&counter]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
    }
    Expect(counter.load() == 100, "TestDestructorDrain");
}

void TestZeroThread()
{
    bool threw = false;
    try
    {
        ThreadPool pool(0, 1);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    catch (...)
    {
        threw = false;
    }
    Expect(threw, "TestZeroThread");
}

} // namespace

int main()
{
    TestBasicExecution();
    TestReturnValue();
    TestExceptionPropagation();
    TestConcurrentSubmit();
    TestDestructorDrain();
    TestZeroThread();

    if (g_failed == 0)
    {
        std::cout << "ALL PASSED\n";
        return 0;
    }

    std::cout << g_failed << " FAILED\n";
    return 1;
}
