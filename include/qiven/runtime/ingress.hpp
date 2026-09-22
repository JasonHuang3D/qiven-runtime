#pragma once

// ============================================================================
// ingress.hpp — the bounded MPSC ingress queue and its message values
// (component ADL §5/§60/§61, design §9)
//
// Adapter and resolver threads NEVER touch control state: they post
// IngressMessage VALUES into this bounded queue; the ONE control thread
// drains it and owns every phase transition (deterministic ordering).
//
// Bounds and honesty: the queue has a fixed capacity; try_push FAILS
// (returns false) when full — producers get backpressure instead of an
// unbounded wait, and the runtime never grows an unbounded queue. Out-of-
// order or unknown-destination messages are never "guessed into place"
// (§61): they are dropped and counted.
// ============================================================================

#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/observed_action.hpp>
#include <qiven/runtime/port/activation.hpp>
#include <qiven/runtime/resolver.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace qiven::runtime
{
struct IngressMessage
{
    enum class Kind : u8
    {
        Proposal,          // a new exact action proposal entering control
        EvidenceDelivery,  // a resolver finished for one requirement
        ResolutionFailure, // a resolver could not produce trusted evidence
    };

    Kind kind = Kind::Proposal;

    // Proposal payload
    CorrelationKey correlation {};
    ObservedAction action;
    // §18/§8.2: a proposal following a ReDeliberate outcome carries the
    // prior transaction as its causal parent for traceability.
    std::optional<ControlTransactionId> causal_parent;
    // §8.2 step 7: activation receipts presented BEFORE the judgment
    // opens may pre-satisfy its BeforeJudgment requirements. Receipts
    // discovered inside the current judgment never authorize it.
    std::vector<port::ActivationReceipt> activation_receipts;

    // Evidence/failure payload
    ControlTransactionId transaction {};
    RequirementIdentity requirement;
    resolver::EvidenceReceipt receipt; // valid for EvidenceDelivery only

    // Diagnostics the drop path counts (never guessed, §61)
    u64 sequence = 0; // producer-assigned arrival hint; ordering is the
                      // QUEUE's job, this is for audit only
};

class BoundedIngressQueue
{
public:
    explicit BoundedIngressQueue(usize capacity);

    BoundedIngressQueue(const BoundedIngressQueue&)            = delete;
    BoundedIngressQueue& operator=(const BoundedIngressQueue&) = delete;

    // Producer side: never blocks. False means FULL (backpressure) or
    // closed — the producer decides to retry or give up.
    [[nodiscard]] bool try_push(IngressMessage message);

    // Consumer side: waits up to the timeout for one message; nullopt on
    // timeout, or when the queue is drained AND closed.
    [[nodiscard]] std::optional<IngressMessage> pop_wait(std::chrono::milliseconds timeout);

    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] usize capacity() const noexcept
    {
        return m_capacity;
    }

private:
    const usize m_capacity;
    mutable std::mutex m_mutex;
    std::condition_variable m_not_empty;
    std::queue<IngressMessage> m_messages;
    bool m_closed = false;
};
} // namespace qiven::runtime
