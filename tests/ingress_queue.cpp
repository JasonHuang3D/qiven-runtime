#include <qiven/runtime/ingress.hpp>

#include <qiven/contracts.hpp>

#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
using qiven::u64;
using qiven::runtime::BoundedIngressQueue;
using qiven::runtime::IngressMessage;

IngressMessage numbered(u64 sequence)
{
    IngressMessage message;
    message.sequence = sequence;
    return message;
}
} // namespace

int main()
{
    // zero capacity is rejected at construction
    {
        bool threw = false;
        try
        {
            BoundedIngressQueue zero { 0 };
            static_cast<void>(zero);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        QIVEN_VERIFY(threw);
    }

    // FIFO order for one producer / one consumer
    {
        BoundedIngressQueue queue { 8 };
        for (u64 i = 0; i < 8; ++i)
        {
            QIVEN_VERIFY(queue.try_push(numbered(i)));
        }
        for (u64 i = 0; i < 8; ++i)
        {
            auto message = queue.pop_wait(std::chrono::milliseconds(1000));
            QIVEN_VERIFY(message.has_value());
            QIVEN_VERIFY(message->sequence == i);
        }
    }

    // full queue: try_push fails with backpressure, never blocks
    {
        BoundedIngressQueue queue { 2 };
        QIVEN_VERIFY(queue.try_push(numbered(1)));
        QIVEN_VERIFY(queue.try_push(numbered(2)));
        QIVEN_VERIFY(!queue.try_push(numbered(3)));
        QIVEN_VERIFY(queue.size() == 2);

        // draining one slot frees exactly one slot
        QIVEN_VERIFY(queue.pop_wait(std::chrono::milliseconds(1000)).has_value());
        QIVEN_VERIFY(queue.try_push(numbered(3)));
    }

    // pop timeout on an empty open queue returns nullopt promptly
    {
        BoundedIngressQueue queue { 2 };
        const auto before = std::chrono::steady_clock::now();
        QIVEN_VERIFY(!queue.pop_wait(std::chrono::milliseconds(50)).has_value());
        const auto elapsed = std::chrono::steady_clock::now() - before;
        QIVEN_VERIFY(elapsed >= std::chrono::milliseconds(45));
    }

    // close(): pushes rejected; buffered messages still drain; then empty
    {
        BoundedIngressQueue queue { 4 };
        QIVEN_VERIFY(queue.try_push(numbered(1)));
        queue.close();
        QIVEN_VERIFY(queue.closed());
        QIVEN_VERIFY(!queue.try_push(numbered(2)));
        QIVEN_VERIFY(queue.pop_wait(std::chrono::milliseconds(1000)).has_value());
        QIVEN_VERIFY(!queue.pop_wait(std::chrono::milliseconds(50)).has_value());
    }

    // close() wakes a blocked consumer
    {
        BoundedIngressQueue queue { 2 };
        std::thread closer([&queue] {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            queue.close();
        });
        const auto before = std::chrono::steady_clock::now();
        QIVEN_VERIFY(!queue.pop_wait(std::chrono::milliseconds(10000)).has_value());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - before);
        QIVEN_VERIFY(elapsed < std::chrono::milliseconds(5000));
        closer.join();
    }

    // MPSC stress: 8 producers x 200 messages, single consumer, no loss,
    // no duplication; capacity small enough to exercise backpressure
    {
        BoundedIngressQueue queue { 16 };
        constexpr u64 producers    = 8;
        constexpr u64 per_producer = 200;
        std::vector<std::thread> threads;
        for (u64 producer = 0; producer < producers; ++producer)
        {
            threads.emplace_back([&queue, producer] {
                for (u64 i = 0; i < per_producer; ++i)
                {
                    const u64 sequence = producer * per_producer + i;
                    while (!queue.try_push(numbered(sequence)))
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                }
            });
        }

        std::vector<u64> received;
        received.reserve(producers * per_producer);
        while (received.size() < producers * per_producer)
        {
            auto message = queue.pop_wait(std::chrono::milliseconds(500));
            if (message.has_value())
            {
                received.push_back(message->sequence);
            }
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }

        QIVEN_VERIFY(received.size() == producers * per_producer);
        std::vector<u64> sorted = received;
        std::sort(sorted.begin(), sorted.end());
        for (u64 i = 0; i < sorted.size(); ++i)
        {
            if (sorted[i] != i)
            {
                QIVEN_VERIFY(sorted[i] == i); // loss or duplication
                break;
            }
        }
    }

    std::printf("[ OK ] ingress-queue\n");
    return 0;
}
