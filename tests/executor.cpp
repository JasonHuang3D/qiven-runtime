#include <qiven/runtime/executor.hpp>

#include <qiven/contracts.hpp>

#include <atomic>
#include <cstdio>
#include <latch>
#include <thread>

namespace
{
using qiven::runtime::Executor;
}

int main()
{
    // zero workers / capacity rejected
    {
        bool threw = false;
        try
        {
            Executor zero { 0, 4 };
            static_cast<void>(zero);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        QIVEN_VERIFY(threw);
    }

    // bounded concurrency: with 2 workers and blocking tasks the
    // high-water concurrency never exceeds 2
    {
        Executor pool { 2, 16 };
        std::atomic<int> ran { 0 };
        std::latch release { 1 };
        for (int i = 0; i < 6; ++i)
        {
            QIVEN_VERIFY(pool.submit([&ran, &release] {
                ran.fetch_add(1);
                release.wait(); // block until the test releases everyone
            }));
        }
        // wait until at least the first two are inside their tasks
        while (ran.load() < 2)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let any over-scheduling manifest
        QIVEN_VERIFY(pool.max_observed_concurrency() <= 2);
        release.count_down();
        pool.wait_all();
        QIVEN_VERIFY(ran.load() == 6);
    }

    // wait_all: every accepted task finished with visible effects
    {
        Executor pool { 4, 64 };
        std::atomic<int> counter { 0 };
        for (int i = 0; i < 64; ++i)
        {
            QIVEN_VERIFY(pool.submit([&counter] { counter.fetch_add(1); }));
        }
        pool.wait_all();
        QIVEN_VERIFY(counter.load() == 64);
    }

    // bounded submission: a full pending queue refuses (no unbounded
    // growth). Deterministic setup: wait until the worker is PROVABLY
    // inside the blocking task (a slow-starting worker legitimately keeps
    // the first task in pending - capacity bounds PENDING, not in-flight)
    {
        Executor pool { 1, 2 };
        std::latch block { 1 };
        std::atomic<bool> inside { false };
        QIVEN_VERIFY(pool.submit([&block, &inside] {
            inside.store(true);
            block.wait();
        }));
        while (!inside.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // worker is inside the task: the two pending slots fill exactly
        QIVEN_VERIFY(pool.submit([] {}));
        QIVEN_VERIFY(pool.submit([] {}));
        QIVEN_VERIFY(!pool.submit([] {})); // bounded
        block.count_down();
        pool.wait_all();
    }

    // a throwing task is contained: workers survive, later tasks still run
    {
        Executor pool { 2, 16 };
        std::atomic<int> after_throws { 0 };
        QIVEN_VERIFY(pool.submit([] { throw std::runtime_error("resolver exploded"); }));
        for (int i = 0; i < 8; ++i)
        {
            QIVEN_VERIFY(pool.submit([&after_throws] { after_throws.fetch_add(1); }));
        }
        pool.wait_all();
        QIVEN_VERIFY(after_throws.load() == 8);
        QIVEN_VERIFY(pool.task_exceptions() >= 1);

        // the executor remains usable after the contained failure
        std::atomic<int> again { 0 };
        QIVEN_VERIFY(pool.submit([&again] { again.fetch_add(1); }));
        pool.wait_all();
        QIVEN_VERIFY(again.load() == 1);
    }

    // shutdown: submits refused, destructor joins without hanging
    {
        auto pool = std::make_unique<Executor>(2, 8);
        std::atomic<int> done { 0 };
        QIVEN_VERIFY(pool->submit([&done] { done.fetch_add(1); }));
        pool->wait_all();
        QIVEN_VERIFY(done.load() == 1);
        pool->shutdown();
        QIVEN_VERIFY(!pool->submit([] {}));
        pool.reset(); // destructor on a shut-down pool: no hang
    }

    // high-water concurrency actually reaches the worker count when tasks
    // overlap (guards against a serialization bug masquerading as safety)
    {
        Executor pool { 4, 16 };
        std::latch all_inside { 4 };
        std::latch release { 1 };
        for (int i = 0; i < 4; ++i)
        {
            QIVEN_VERIFY(pool.submit([&all_inside, &release] {
                all_inside.count_down();
                release.wait();
            }));
        }
        all_inside.wait();
        QIVEN_VERIFY(pool.max_observed_concurrency() == 4);
        release.count_down();
        pool.wait_all();
    }

    std::printf("[ OK ] executor\n");
    return 0;
}
