#include <qiven/runtime/ingress.hpp>

#include <stdexcept>

namespace qiven::runtime
{
BoundedIngressQueue::BoundedIngressQueue(usize capacity) :
m_capacity(capacity)
{
    if (capacity == 0)
    {
        throw std::invalid_argument("BoundedIngressQueue capacity must be non-zero");
    }
}

bool BoundedIngressQueue::try_push(IngressMessage message)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
        {
            return false;
        }
        if (m_messages.size() >= m_capacity)
        {
            return false; // backpressure, never an unbounded wait
        }
        m_messages.push(std::move(message));
    }
    m_not_empty.notify_one();
    return true;
}

std::optional<IngressMessage> BoundedIngressQueue::pop_wait(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const bool arrived = m_not_empty.wait_for(lock, timeout, [this] { return !m_messages.empty() || m_closed; });
    if (m_messages.empty())
    {
        return std::nullopt; // timeout, or drained-and-closed
    }
    IngressMessage message = std::move(m_messages.front());
    m_messages.pop();
    return message;
}

void BoundedIngressQueue::close() noexcept
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }
    m_not_empty.notify_all();
}

bool BoundedIngressQueue::closed() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closed;
}

usize BoundedIngressQueue::size() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_messages.size();
}
} // namespace qiven::runtime
