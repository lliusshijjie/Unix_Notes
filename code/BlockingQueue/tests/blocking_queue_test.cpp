#include "blocking_queue.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <span>
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

struct Item
{
    int id;
    std::string name;

    Item() = delete;
    Item(int id_value, std::string name_value)
        : id(id_value)
        , name(std::move(name_value))
    {
    }

    Item(const Item&) = delete;
    Item& operator=(const Item&) = delete;
    Item(Item&&) noexcept = default;
    Item& operator=(Item&&) noexcept = default;
};

struct LifetimeTracked
{
    inline static int live_count = 0;
    inline static int ctor_count = 0;
    inline static int move_ctor_count = 0;
    inline static int move_assign_count = 0;
    inline static int dtor_count = 0;

    int value;

    explicit LifetimeTracked(int v)
        : value(v)
    {
        ++live_count;
        ++ctor_count;
    }

    LifetimeTracked(const LifetimeTracked&) = delete;
    LifetimeTracked& operator=(const LifetimeTracked&) = delete;

    LifetimeTracked(LifetimeTracked&& other) noexcept
        : value(other.value)
    {
        ++live_count;
        ++move_ctor_count;
    }

    LifetimeTracked& operator=(LifetimeTracked&& other) noexcept
    {
        value = other.value;
        ++move_assign_count;
        return *this;
    }

    ~LifetimeTracked()
    {
        --live_count;
        ++dtor_count;
    }

    static void Reset()
    {
        live_count = 0;
        ctor_count = 0;
        move_ctor_count = 0;
        move_assign_count = 0;
        dtor_count = 0;
    }
};

struct ThrowingItem
{
    int value;

    explicit ThrowingItem(int v)
        : value(v)
    {
        if (v < 0)
        {
            throw std::runtime_error("ThrowingItem");
        }
    }

    ThrowingItem(const ThrowingItem&) = delete;
    ThrowingItem& operator=(const ThrowingItem&) = delete;
    ThrowingItem(ThrowingItem&&) noexcept = default;
    ThrowingItem& operator=(ThrowingItem&&) noexcept = default;
};

void TestConstruct()
{
    BlockingQueue<int> q(8);
    Expect(q.capacity() == 8, "Construct_capacity");
    Expect(q.size() == 0, "Construct_size");
    Expect(q.empty(), "Construct_empty");
    Expect(!q.full(), "Construct_not_full");
    Expect(!q.closed(), "Construct_not_closed");

    BlockingQueue<int> q1(1);
    Expect(q1.capacity() == 1, "Construct_capacity_1");

    bool threw = false;
    try
    {
        BlockingQueue<int> bad(0);
        (void)bad;
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Expect(threw, "Construct_capacity_0_throws");
}

void TestFifo()
{
    BlockingQueue<int> q(8);
    Expect(q.push(1), "Fifo_push_1");
    Expect(q.push(2), "Fifo_push_2");
    Expect(q.push(3), "Fifo_push_3");

    int v = 0;
    Expect(q.pop(v) && v == 1, "Fifo_pop_1");
    Expect(q.pop(v) && v == 2, "Fifo_pop_2");
    Expect(q.pop(v) && v == 3, "Fifo_pop_3");
    Expect(q.empty(), "Fifo_empty");
}

void TestRingBufferWrapAround()
{
    RingBuffer<int> buffer(3);
    buffer.emplace(1);
    buffer.emplace(2);
    buffer.emplace(3);

    Expect(buffer.pop() == 1, "RingWrap_pop_1");
    Expect(buffer.pop() == 2, "RingWrap_pop_2");

    buffer.emplace(4);
    buffer.emplace(5);

    Expect(buffer.pop() == 3, "RingWrap_pop_3");
    Expect(buffer.pop() == 4, "RingWrap_pop_4");
    Expect(buffer.pop() == 5, "RingWrap_pop_5");
    Expect(buffer.empty(), "RingWrap_empty");
}

void TestRingBufferNonDefaultConstruct()
{
    RingBuffer<Item> buffer(2);
    buffer.emplace(7, "seven");
    buffer.emplace(8, "eight");

    Item first = buffer.pop();
    Item second = buffer.pop();

    Expect(first.id == 7 && first.name == "seven", "RingItem_first");
    Expect(second.id == 8 && second.name == "eight", "RingItem_second");
}

void TestRingBufferLifetime()
{
    LifetimeTracked::Reset();

    {
        RingBuffer<LifetimeTracked> buffer(2);
        buffer.emplace(1);
        buffer.emplace(2);
        Expect(LifetimeTracked::live_count == 2, "RingLife_live_after_emplace");

        LifetimeTracked first = buffer.pop();
        Expect(first.value == 1, "RingLife_first_value");
        Expect(LifetimeTracked::live_count == 2, "RingLife_live_after_pop");

        buffer.emplace(3);
        Expect(LifetimeTracked::live_count == 3, "RingLife_live_after_reuse");
    }

    Expect(LifetimeTracked::live_count == 0, "RingLife_live_after_scope");
    Expect(LifetimeTracked::ctor_count == 3, "RingLife_ctor_count");
    Expect(
        LifetimeTracked::dtor_count ==
            LifetimeTracked::ctor_count + LifetimeTracked::move_ctor_count,
        "RingLife_dtor_count");
}

void TestRingBufferConstructException()
{
    RingBuffer<ThrowingItem> buffer(2);

    bool threw = false;
    try
    {
        buffer.emplace(-1);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    Expect(threw, "RingThrow_throw");
    Expect(buffer.size() == 0, "RingThrow_size_unchanged");

    buffer.emplace(5);
    ThrowingItem item = buffer.pop();
    Expect(item.value == 5, "RingThrow_reusable");
}

void TestLvalueCopy()
{
    BlockingQueue<std::string> q(4);
    std::string value = "hello";
    Expect(q.push(value), "Lvalue_push");
    Expect(value == "hello", "Lvalue_src_intact");

    std::string out;
    Expect(q.pop(out) && out == "hello", "Lvalue_pop");
}

void TestRvalueMove()
{
    BlockingQueue<std::unique_ptr<int>> q(4);
    std::unique_ptr<int> p(new int(42));
    Expect(q.push(std::move(p)), "Move_push");
    Expect(!p, "Move_src_empty");

    std::unique_ptr<int> out;
    Expect(q.pop(out) && out && *out == 42, "Move_pop");
}

void TestFullBlocks()
{
    BlockingQueue<int> q(1);
    Expect(q.push(1), "Full_prefill");

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> done{false};

    std::thread producer([&]() {
        entered.set_value();
        done.store(q.push(2), std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Expect(!done.load(std::memory_order_acquire), "Full_still_blocked");

    int v = 0;
    Expect(q.pop(v) && v == 1, "Full_pop_space");

    producer.join();
    Expect(done.load(std::memory_order_acquire), "Full_unblocked");
    Expect(q.pop(v) && v == 2, "Full_pop_second");
}

void TestEmptyBlocks()
{
    BlockingQueue<int> q(4);

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> done{false};
    std::atomic<int> got{-1};

    std::thread consumer([&]() {
        entered.set_value();
        int v = 0;
        bool ok = q.pop(v);
        got.store(ok ? v : -2, std::memory_order_release);
        done.store(true, std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Expect(!done.load(std::memory_order_acquire), "Empty_still_blocked");

    Expect(q.push(7), "Empty_push");
    consumer.join();
    Expect(done.load(std::memory_order_acquire), "Empty_unblocked");
    Expect(got.load(std::memory_order_acquire) == 7, "Empty_got_value");
}

void TestCloseEmpty()
{
    BlockingQueue<int> q(4);

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> pop_ok{true};

    std::thread consumer([&]() {
        entered.set_value();
        int v = 0;
        pop_ok.store(q.pop(v), std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    consumer.join();
    Expect(!pop_ok.load(std::memory_order_acquire), "CloseEmpty_pop_false");
    Expect(q.closed(), "CloseEmpty_closed");
}

void TestCloseFull()
{
    BlockingQueue<int> q(1);
    Expect(q.push(1), "CloseFull_prefill");

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> push_ok{true};

    std::thread producer([&]() {
        entered.set_value();
        push_ok.store(q.push(2), std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    producer.join();
    Expect(!push_ok.load(std::memory_order_acquire), "CloseFull_push_false");
}

void TestCloseDrain()
{
    BlockingQueue<int> q(8);
    Expect(q.push(1), "Drain_push_1");
    Expect(q.push(2), "Drain_push_2");
    Expect(q.push(3), "Drain_push_3");
    q.close();

    int v = 0;
    Expect(q.pop(v) && v == 1, "Drain_pop_1");
    Expect(q.pop(v) && v == 2, "Drain_pop_2");
    Expect(q.pop(v) && v == 3, "Drain_pop_3");
    Expect(!q.pop(v), "Drain_pop_done");
}

void TestCloseRejectPush()
{
    BlockingQueue<int> q(4);
    q.close();

    int x = 1;
    Expect(!q.push(x), "CloseReject_lvalue");
    Expect(!q.push(2), "CloseReject_rvalue");
    Expect(q.size() == 0, "CloseReject_size");

    BlockingQueue<std::unique_ptr<int>> mq(4);
    mq.close();
    std::unique_ptr<int> p(new int(9));
    Expect(!mq.push(std::move(p)), "CloseReject_move_fail");
    Expect(p && *p == 9, "CloseReject_not_moved");
}

void TestCloseIdempotent()
{
    BlockingQueue<int> q(4);
    q.close();
    q.close();
    q.close();
    Expect(q.closed(), "CloseIdempotent_closed");

    int v = 0;
    Expect(!q.pop(v), "CloseIdempotent_pop");
    Expect(!q.push(1), "CloseIdempotent_push");
}

void TestEmplace()
{
    BlockingQueue<Item> q(2);
    Expect(q.emplace(1, "one"), "Emplace_ok");

    Item item(0, "init");
    Expect(q.pop(item), "Emplace_pop");
    Expect(item.id == 1 && item.name == "one", "Emplace_value");
}

void TestTryEmplace()
{
    BlockingQueue<std::unique_ptr<int>> q(1);
    Expect(q.try_emplace(std::unique_ptr<int>(new int(7))), "TryEmplace_ok");

    std::unique_ptr<int> p(new int(9));
    Expect(!q.try_emplace(std::move(p)), "TryEmplace_full_false");
    Expect(p && *p == 9, "TryEmplace_not_moved");

    std::unique_ptr<int> out;
    Expect(q.pop(out) && out && *out == 7, "TryEmplace_pop");
}

void TestPushForSuccess()
{
    BlockingQueue<int> q(1);
    Expect(q.push(1), "PushForSuccess_prefill");

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};

    std::thread producer([&]() {
        entered.set_value();
        ok.store(q.push_for(2, std::chrono::seconds(1)), std::memory_order_release);
        done.store(true, std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Expect(!done.load(std::memory_order_acquire), "PushForSuccess_waiting");

    int v = 0;
    Expect(q.pop(v) && v == 1, "PushForSuccess_pop_make_room");

    producer.join();
    Expect(ok.load(std::memory_order_acquire), "PushForSuccess_ok");
    Expect(q.pop(v) && v == 2, "PushForSuccess_pop_value");
}

void TestPushForTimeout()
{
    BlockingQueue<int> q(1);
    Expect(q.push(1), "PushForTimeout_prefill");

    const auto begin = std::chrono::steady_clock::now();
    const bool ok = q.push_for(2, std::chrono::milliseconds(80));
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    Expect(!ok, "PushForTimeout_false");
    Expect(elapsed >= std::chrono::milliseconds(40), "PushForTimeout_waited");
    Expect(elapsed < std::chrono::milliseconds(500), "PushForTimeout_bounded");
    Expect(q.size() == 1, "PushForTimeout_size");

    int v = 0;
    Expect(q.pop(v) && v == 1, "PushForTimeout_queue_unchanged");

    BlockingQueue<std::unique_ptr<int>> mq(1);
    Expect(mq.push(std::unique_ptr<int>(new int(7))), "PushForTimeoutMove_prefill");
    std::unique_ptr<int> p(new int(9));
    Expect(!mq.push_for(std::move(p), std::chrono::milliseconds(50)), "PushForTimeoutMove_false");
    Expect(p && *p == 9, "PushForTimeoutMove_not_moved");
}

void TestPushForClose()
{
    BlockingQueue<int> q(1);
    Expect(q.push(1), "PushForClose_prefill");

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> ok{true};
    std::atomic<long long> elapsed_ms{0};

    std::thread producer([&]() {
        entered.set_value();
        const auto begin = std::chrono::steady_clock::now();
        const bool pushed = q.push_for(2, std::chrono::seconds(1));
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin);
        ok.store(pushed, std::memory_order_release);
        elapsed_ms.store(elapsed.count(), std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    producer.join();

    Expect(!ok.load(std::memory_order_acquire), "PushForClose_false");
    Expect(elapsed_ms.load(std::memory_order_acquire) < 500, "PushForClose_early_exit");
}

void TestEmplaceForSuccess()
{
    BlockingQueue<Item> q(1);
    Expect(q.emplace(1, "one"), "EmplaceForSuccess_prefill");

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> ok{false};

    std::thread producer([&]() {
        entered.set_value();
        ok.store(
            q.emplace_for(std::chrono::seconds(1), 2, "two"),
            std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Item item(0, "init");
    Expect(q.pop(item) && item.id == 1, "EmplaceForSuccess_pop_make_room");

    producer.join();
    Expect(ok.load(std::memory_order_acquire), "EmplaceForSuccess_ok");
    Expect(q.pop(item) && item.id == 2 && item.name == "two", "EmplaceForSuccess_value");
}

void TestEmplaceForTimeout()
{
    BlockingQueue<std::unique_ptr<int>> q(1);
    Expect(q.emplace(std::unique_ptr<int>(new int(1))), "EmplaceForTimeout_prefill");

    std::unique_ptr<int> p(new int(2));
    const auto begin = std::chrono::steady_clock::now();
    const bool ok = q.emplace_for(std::chrono::milliseconds(80), std::move(p));
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    Expect(!ok, "EmplaceForTimeout_false");
    Expect(p && *p == 2, "EmplaceForTimeout_not_moved");
    Expect(elapsed >= std::chrono::milliseconds(40), "EmplaceForTimeout_waited");
    Expect(elapsed < std::chrono::milliseconds(500), "EmplaceForTimeout_bounded");
}

void TestEmplaceForClose()
{
    BlockingQueue<Item> q(1);
    Expect(q.emplace(1, "one"), "EmplaceForClose_prefill");

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> ok{true};
    std::atomic<long long> elapsed_ms{0};

    std::thread producer([&]() {
        entered.set_value();
        const auto begin = std::chrono::steady_clock::now();
        const bool emplaced = q.emplace_for(std::chrono::seconds(1), 2, "two");
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin);
        ok.store(emplaced, std::memory_order_release);
        elapsed_ms.store(elapsed.count(), std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    producer.join();

    Expect(!ok.load(std::memory_order_acquire), "EmplaceForClose_false");
    Expect(elapsed_ms.load(std::memory_order_acquire) < 500, "EmplaceForClose_early_exit");
}

void TestEmplaceException()
{
    BlockingQueue<ThrowingItem> q(2);

    bool threw = false;
    try
    {
        q.emplace(-1);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    Expect(threw, "EmplaceException_throw");
    Expect(q.size() == 0, "EmplaceException_size");
    Expect(q.emplace(3), "EmplaceException_reusable");

    ThrowingItem item(0);
    Expect(q.pop(item) && item.value == 3, "EmplaceException_pop");
}

void TestPopForSuccess()
{
    BlockingQueue<int> q(1);

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> done{false};
    std::atomic<int> got{-1};

    std::thread consumer([&]() {
        entered.set_value();
        int v = 0;
        const bool ok = q.pop_for(v, std::chrono::seconds(1));
        got.store(ok ? v : -2, std::memory_order_release);
        done.store(true, std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Expect(!done.load(std::memory_order_acquire), "PopForSuccess_waiting");
    Expect(q.push(7), "PopForSuccess_push");

    consumer.join();
    Expect(done.load(std::memory_order_acquire), "PopForSuccess_done");
    Expect(got.load(std::memory_order_acquire) == 7, "PopForSuccess_value");
}

void TestPopForTimeout()
{
    BlockingQueue<int> q(1);

    int value = 123;
    const auto begin = std::chrono::steady_clock::now();
    const bool ok = q.pop_for(value, std::chrono::milliseconds(80));
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    Expect(!ok, "PopForTimeout_false");
    Expect(value == 123, "PopForTimeout_value_unchanged");
    Expect(elapsed >= std::chrono::milliseconds(40), "PopForTimeout_waited");
    Expect(elapsed < std::chrono::milliseconds(500), "PopForTimeout_bounded");
}

void TestPopForClose()
{
    BlockingQueue<int> q(1);

    std::promise<void> entered;
    std::future<void> entered_fut = entered.get_future();
    std::atomic<bool> ok{true};
    std::atomic<int> value{123};
    std::atomic<long long> elapsed_ms{0};

    std::thread consumer([&]() {
        entered.set_value();
        int v = 123;
        const auto begin = std::chrono::steady_clock::now();
        const bool popped = q.pop_for(v, std::chrono::seconds(1));
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin);
        ok.store(popped, std::memory_order_release);
        value.store(v, std::memory_order_release);
        elapsed_ms.store(elapsed.count(), std::memory_order_release);
    });

    entered_fut.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    consumer.join();

    Expect(!ok.load(std::memory_order_acquire), "PopForClose_false");
    Expect(value.load(std::memory_order_acquire) == 123, "PopForClose_value_unchanged");
    Expect(elapsed_ms.load(std::memory_order_acquire) < 500, "PopForClose_early_exit");
}

void TestPopForDrainAfterClose()
{
    BlockingQueue<int> q(4);
    Expect(q.push(1), "PopForDrain_push_1");
    Expect(q.push(2), "PopForDrain_push_2");
    q.close();

    int v = 0;
    Expect(q.pop_for(v, std::chrono::milliseconds(0)) && v == 1, "PopForDrain_pop_1");
    Expect(q.pop_for(v, std::chrono::milliseconds(0)) && v == 2, "PopForDrain_pop_2");
    Expect(!q.pop_for(v, std::chrono::milliseconds(0)), "PopForDrain_done");
}

void TestZeroTimeout()
{
    BlockingQueue<int> q(1);
    Expect(q.push_for(1, std::chrono::milliseconds(0)), "ZeroTimeout_push_ok");
    Expect(!q.push_for(2, std::chrono::milliseconds(0)), "ZeroTimeout_push_fail");

    int v = 0;
    Expect(q.pop_for(v, std::chrono::milliseconds(0)) && v == 1, "ZeroTimeout_pop_ok");
    v = 99;
    Expect(!q.pop_for(v, std::chrono::milliseconds(0)), "ZeroTimeout_pop_fail");
    Expect(v == 99, "ZeroTimeout_pop_unchanged");
}

void TestPushBatchPartial()
{
    BlockingQueue<int> q(3);

    Expect(q.push(1), "PushBatchPartial_fill_1");
    Expect(q.push(2), "PushBatchPartial_fill_2");

    std::vector<int> values{10, 20, 30, 40};
    const std::size_t pushed = q.push_batch(std::span<const int>(values));
    Expect(pushed == 1, "PushBatchPartial_pushed");
    Expect(q.size() == 3, "PushBatchPartial_size");

    int out[3]{};
    const std::size_t popped = q.pop_batch(std::span<int>(out));
    Expect(popped == 3, "PushBatchPartial_popped");
    Expect(out[0] == 1 && out[1] == 2 && out[2] == 10, "PushBatchPartial_order");
}

void TestPopBatchPartial()
{
    BlockingQueue<int> q(5);
    for (int i = 0; i < 3; ++i)
    {
        Expect(q.push(i + 1), "PopBatchPartial_fill");
    }

    std::vector<int> out(8, 0);
    const std::size_t popped = q.pop_batch(std::span<int>(out));
    Expect(popped == 3, "PopBatchPartial_count");
    Expect(out[0] == 1 && out[1] == 2 && out[2] == 3, "PopBatchPartial_order");
    Expect(q.empty(), "PopBatchPartial_empty");
}

void TestBatchFullAndDrain()
{
    BlockingQueue<int> q(4);
    int values[5] = {1, 2, 3, 4, 5};
    const std::size_t pushed = q.push_batch(std::span<const int>(values));
    Expect(pushed == 4, "BatchFullAndDrain_pushed");
    Expect(q.full(), "BatchFullAndDrain_full");

    std::vector<int> out(2);
    Expect(q.pop_batch(std::span<int>(out)) == 2, "BatchFullAndDrain_pop_half");
    Expect(out[0] == 1 && out[1] == 2, "BatchFullAndDrain_pop_half_order");

    int tail[4]{};
    Expect(q.pop_batch(std::span<int>(tail)) == 2, "BatchFullAndDrain_pop_tail");
    Expect(tail[0] == 3 && tail[1] == 4, "BatchFullAndDrain_pop_tail_order");
    Expect(q.empty(), "BatchFullAndDrain_empty");
}

void TestBatchCloseSemantics()
{
    BlockingQueue<int> q(4);
    int values[2] = {1, 2};
    Expect(q.push_batch(std::span<const int>(values)) == 2, "BatchCloseSemantics_push");
    q.close();

    int more[2] = {9, 10};
    Expect(q.push_batch(std::span<const int>(more)) == 0, "BatchCloseSemantics_reject");

    std::vector<int> out(4);
    const std::size_t popped = q.pop_batch(std::span<int>(out));
    Expect(popped == 2, "BatchCloseSemantics_drain_count");
    Expect(out[0] == 1 && out[1] == 2, "BatchCloseSemantics_drain_order");
    Expect(q.pop_batch(std::span<int>(out)) == 0, "BatchCloseSemantics_done");
}

void TestBatchMpmc()
{
    const int producer_count = 2;
    const int consumer_count = 2;
    const int per_producer = 2000;
    const int batch = 5;
    const int total = producer_count * per_producer;

    BlockingQueue<int> q(128);
    std::mutex result_mutex;
    std::vector<int> results;
    results.reserve(static_cast<std::size_t>(total));

    std::atomic<int> push_fail{0};

    std::vector<std::thread> consumers;
    consumers.reserve(static_cast<std::size_t>(consumer_count));
    for (int i = 0; i < consumer_count; ++i)
    {
        consumers.emplace_back([&]() {
            std::vector<int> local(static_cast<std::size_t>(batch));
            while (true)
            {
                const std::size_t n = q.pop_batch(std::span<int>(local));
                if (n == 0)
                {
                    if (q.closed() && q.empty())
                    {
                        break;
                    }
                    continue;
                }
                std::lock_guard<std::mutex> lock(result_mutex);
                results.insert(results.end(), local.begin(), local.begin() + n);
            }
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(static_cast<std::size_t>(producer_count));
    for (int p = 0; p < producer_count; ++p)
    {
        producers.emplace_back([&, p]() {
            int i = 0;
            while (i < per_producer)
            {
                std::vector<int> chunk;
                chunk.reserve(static_cast<std::size_t>(batch));
                for (int k = 0; k < batch && i < per_producer; ++k, ++i)
                {
                    chunk.push_back(p * per_producer + i);
                }
                const std::size_t remaining = chunk.size();
                std::size_t offset = 0;
                while (offset < remaining)
                {
                    const std::size_t pushed = q.push_batch(
                        std::span<const int>(chunk.data() + offset, remaining - offset));
                    if (pushed == 0 && q.closed())
                    {
                        push_fail.fetch_add(static_cast<int>(remaining - offset),
                                            std::memory_order_relaxed);
                        return;
                    }
                    offset += pushed;
                }
            }
        });
    }

    for (std::size_t i = 0; i < producers.size(); ++i)
    {
        producers[i].join();
    }
    q.close();
    for (std::size_t i = 0; i < consumers.size(); ++i)
    {
        consumers[i].join();
    }

    Expect(push_fail.load() == 0, "BatchMpmc_push");
    Expect(static_cast<int>(results.size()) == total, "BatchMpmc_count");

    std::set<int> unique(results.begin(), results.end());
    Expect(static_cast<int>(unique.size()) == total, "BatchMpmc_unique");
    Expect(*unique.begin() == 0 && *unique.rbegin() == total - 1, "BatchMpmc_range");
}

void TestMpmc()
{
    const int producer_count = 4;
    const int consumer_count = 4;
    const int per_producer = 10000;
    const int total = producer_count * per_producer;

    BlockingQueue<int> q(128);
    std::mutex result_mutex;
    std::vector<int> results;
    results.reserve(static_cast<std::size_t>(total));

    std::vector<std::thread> consumers;
    consumers.reserve(static_cast<std::size_t>(consumer_count));
    for (int i = 0; i < consumer_count; ++i)
    {
        consumers.emplace_back([&]() {
            int v = 0;
            while (q.pop(v))
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                results.push_back(v);
            }
        });
    }

    std::atomic<int> push_fail{0};
    std::vector<std::thread> producers;
    producers.reserve(static_cast<std::size_t>(producer_count));
    for (int p = 0; p < producer_count; ++p)
    {
        producers.emplace_back([&, p]() {
            for (int s = 0; s < per_producer; ++s)
            {
                if (!q.push(p * per_producer + s))
                {
                    push_fail.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (std::size_t i = 0; i < producers.size(); ++i)
    {
        producers[i].join();
    }
    q.close();
    for (std::size_t i = 0; i < consumers.size(); ++i)
    {
        consumers[i].join();
    }

    Expect(push_fail.load() == 0, "Mpmc_push");
    Expect(static_cast<int>(results.size()) == total, "Mpmc_count");

    std::set<int> unique(results.begin(), results.end());
    Expect(static_cast<int>(unique.size()) == total, "Mpmc_unique");
    Expect(*unique.begin() == 0 && *unique.rbegin() == total - 1, "Mpmc_range");
}

} // namespace

int main()
{
    TestConstruct();
    TestFifo();
    TestRingBufferWrapAround();
    TestRingBufferNonDefaultConstruct();
    TestRingBufferLifetime();
    TestRingBufferConstructException();
    TestLvalueCopy();
    TestRvalueMove();
    TestFullBlocks();
    TestEmptyBlocks();
    TestCloseEmpty();
    TestCloseFull();
    TestCloseDrain();
    TestCloseRejectPush();
    TestCloseIdempotent();
    TestEmplace();
    TestTryEmplace();
    TestPushForSuccess();
    TestPushForTimeout();
    TestPushForClose();
    TestEmplaceForSuccess();
    TestEmplaceForTimeout();
    TestEmplaceForClose();
    TestEmplaceException();
    TestPopForSuccess();
    TestPopForTimeout();
    TestPopForClose();
    TestPopForDrainAfterClose();
    TestZeroTimeout();
    TestPushBatchPartial();
    TestPopBatchPartial();
    TestBatchFullAndDrain();
    TestBatchCloseSemantics();
    TestBatchMpmc();
    TestMpmc();

    if (g_failed == 0)
    {
        std::cout << "ALL PASSED\n";
        return 0;
    }

    std::cout << "FAILED: " << g_failed << '\n';
    return 1;
}
