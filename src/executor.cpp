#include <qiven/runtime/executor.hpp>

#include <stdexcept>

namespace qiven::runtime
{
Executor::Executor(usize workers, usize pending_capacity) :
m_capacity(pending_capacity)
{
    if (workers == 0 || pending_capacity == 0)
    {
        throw std::invalid_argument("Executor requires non-zero workers and capacity");
    }
    m_workers.reserve(workers);
    for (usize i = 0; i < workers; ++i)
    {
        m_workers.emplace_back([this] { worker_loop(); });
    }
}

Executor::~Executor()
{
    shutdown();
}

void Executor::worker_loop()
{
    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_not_empty.wait(lock, [this] { return m_shutdown || !m_pending.empty(); });
            if (m_pending.empty())
            {
                if (m_shutdown)
                {
                    return;
                }
                continue;
            }
            task = std::move(m_pending.front());
            m_pending.pop();
            ++m_running;
        }

        u64 now_running = 0;
        {
            // read under the same lock that owns m_running: an unlocked
            // read would be a data race, diagnostic or not
            std::lock_guard<std::mutex> lock(m_mutex);
            now_running = m_running;
        }
        u64 observed = m_max_concurrency.load(std::memory_order_relaxed);
        while (now_running > observed &&
               !m_max_concurrency.compare_exchange_weak(observed, now_running, std::memory_order_relaxed))
        {
        }

        try
        {
            task();
        }
        catch (...)
        {
            // a throwing resolver task is contained: counted, never fatal
            // to the worker, never silently swallowed as success
            m_exceptions.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_running;
            if (m_pending.empty() && m_running == 0)
            {
                m_idle.notify_all();
            }
        }
    }
}

bool Executor::submit(Task task)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown || m_pending.size() >= m_capacity)
        {
            return false; // bounded: no unbounded queue, no silent drop
        }
        m_pending.push(std::move(task));
    }
    m_not_empty.notify_one();
    return true;
}

void Executor::wait_all()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return m_pending.empty() && m_running == 0; });
}

void Executor::shutdown() noexcept
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown)
        {
            return;
        }
        m_shutdown = true;
    }
    m_not_empty.notify_all();
    for (std::thread& worker : m_workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}
} // namespace qiven::runtime
