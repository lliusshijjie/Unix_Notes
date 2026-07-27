#include "../ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double Percentile(std::vector<double>& data, double p)
{
    if (data.empty())
    {
        return 0.0;
    }
    std::sort(data.begin(), data.end());
    const double idx = p * static_cast<double>(data.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = (lo + 1 < data.size()) ? (lo + 1) : lo;
    const double frac = idx - static_cast<double>(lo);
    return data[lo] * (1.0 - frac) + data[hi] * frac;
}

struct BenchResult
{
    std::string name;
    int workers;
    int tasks;
    double elapsed_sec;
    double throughput;
    double avg_submit_us;
    double avg_queue_wait_us;
    double avg_e2e_us;
    double p50_us;
    double p95_us;
    double p99_us;
};

void PrintResult(const BenchResult& r)
{
    std::cout << std::fixed << std::setprecision(2);
    std::cout << r.name
              << " workers=" << r.workers
              << " tasks=" << r.tasks
              << " thrpt=" << r.throughput << "/s"
              << " submit=" << r.avg_submit_us << "us"
              << " qwait=" << r.avg_queue_wait_us << "us"
              << " e2e=" << r.avg_e2e_us << "us"
              << " p50=" << r.p50_us
              << " p95=" << r.p95_us
              << " p99=" << r.p99_us
              << '\n';
}

template <class Work>
BenchResult RunBench(const std::string& name, int workers, int task_count, Work work)
{
    ThreadPool pool(static_cast<std::size_t>(workers), static_cast<std::size_t>(task_count));
    std::vector<double> submit_us(static_cast<std::size_t>(task_count));
    std::vector<double> queue_wait_us(static_cast<std::size_t>(task_count));
    std::vector<double> e2e_us(static_cast<std::size_t>(task_count));
    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<std::size_t>(task_count));

    const auto wall_begin = Clock::now();
    for (int i = 0; i < task_count; ++i)
    {
        const auto submit_begin = Clock::now();
        futures.push_back(pool.submit([&, i, submit_begin]() {
            const auto start = Clock::now();
            queue_wait_us[static_cast<std::size_t>(i)] =
                std::chrono::duration<double, std::micro>(start - submit_begin).count();
            work();
            const auto end = Clock::now();
            e2e_us[static_cast<std::size_t>(i)] =
                std::chrono::duration<double, std::micro>(end - submit_begin).count();
        }));
        const auto submit_end = Clock::now();
        submit_us[static_cast<std::size_t>(i)] =
            std::chrono::duration<double, std::micro>(submit_end - submit_begin).count();
    }

    for (auto& f : futures)
    {
        f.get();
    }
    const auto wall_end = Clock::now();

    const double elapsed =
        std::chrono::duration<double>(wall_end - wall_begin).count();

    BenchResult result;
    result.name = name;
    result.workers = workers;
    result.tasks = task_count;
    result.elapsed_sec = elapsed;
    result.throughput = static_cast<double>(task_count) / elapsed;
    result.avg_submit_us =
        std::accumulate(submit_us.begin(), submit_us.end(), 0.0) / task_count;
    result.avg_queue_wait_us =
        std::accumulate(queue_wait_us.begin(), queue_wait_us.end(), 0.0) / task_count;
    result.avg_e2e_us =
        std::accumulate(e2e_us.begin(), e2e_us.end(), 0.0) / task_count;
    result.p50_us = Percentile(e2e_us, 0.50);
    result.p95_us = Percentile(e2e_us, 0.95);
    result.p99_us = Percentile(e2e_us, 0.99);
    return result;
}

volatile std::uint64_t g_sink = 0;

void ShortCpu()
{
    std::uint64_t sum = 0;
    for (int i = 0; i < 128; ++i)
    {
        sum += static_cast<std::uint64_t>(i) * 17u;
    }
    g_sink += sum;
}

void LongCpu()
{
    std::uint64_t sum = 0;
    for (int i = 0; i < 100000; ++i)
    {
        sum += static_cast<std::uint64_t>(i) * 17u;
    }
    g_sink += sum;
}

} // namespace

int main()
{
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    std::vector<int> worker_counts = {1, 2, 4, 8};
    if (hw > 8)
    {
        worker_counts.push_back(hw);
    }

    std::cout << "Phase 3.1 benchmark baseline\n";
    std::cout << "hardware_concurrency=" << hw << '\n';

    for (int workers : worker_counts)
    {
        PrintResult(RunBench("empty", workers, 50000, []() {}));
        PrintResult(RunBench("short", workers, 20000, ShortCpu));
        PrintResult(RunBench("long", workers, 2000, LongCpu));
    }

    return 0;
}
