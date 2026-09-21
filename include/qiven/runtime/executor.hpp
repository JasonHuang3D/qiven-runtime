#pragma once

// ============================================================================
// executor.hpp — the bounded resolver executor (design §9)
//
// Independent resolvers run concurrently on N worker threads with a BOUNDED
// pending-task queue: submit() fails when full or closed — no unbounded
// queueing, no silent drop. Worker exceptions are contained (a throwing
// resolver task never kills a worker; the failure is counted and the
// requirement it served is reported as unresolved by the task itself).
//
// Tasks posted here must not touch control state: their result travels
// back through the ingress queue as an EvidenceDelivery/ResolutionFailure
// message (design §9: adapter/resolver threads never touch state).
// ============================================================================

#include <qiven/types.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace qiven::runtime
{
class Executor
{
public:
    explicit Executor(usize workers, usize pending_capacity);
    ~Executor();

    Executor(const Executor&)            = delete;
    Executor& operator=(const Executor&) = delete;

    using Task = std::function<void()>;

    // False when the pending queue is full or the executor is shut down.
    [[nodiscard]] bool submit(Task task);

    // Wait until every accepted task has finished running.
    void wait_all();

    void shutdown() noexcept;

    [[nodiscard]] usize worker_count() const noexcept
    {
        return m_workers.size();
    }

    // Observability for tests/diagnostics: high-water concurrent tasks and
    // the count of contained task exceptions.
    [[nodiscard]] u64 max_observed_concurrency() const noexcept
    {
        return m_max_concurrency.load();
    }
    [[nodiscard]] u64 task_exceptions() const noexcept
    {
        return m_exceptions.load();
    }

private:
    void worker_loop();

    const usize m_capacity;
    std::vector<std::thread> m_workers;
    mutable std::mutex m_mutex;
    std::condition_variable m_not_empty;
    std::condition_variable m_idle;
    std::queue<Task> m_pending;
    usize m_running = 0;
    bool m_shutdown = false;
    std::atomic<u64> m_max_concurrency { 0 };
    std::atomic<u64> m_exceptions { 0 };
};
} // namespace qiven::runtime
