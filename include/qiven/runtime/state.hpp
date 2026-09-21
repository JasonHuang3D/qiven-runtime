#pragma once

// ============================================================================
// state.hpp — the single-threaded control core (component ADL §5/§30/§60/
// §61/§62, design §9)
//
// ONE thread drains the ingress queue and owns ALL control state and phase
// transitions. Resolvers run on the Executor and report back as ingress
// messages; they never touch state. Readiness is re-evaluated only after
// EVERY blocking requirement instance of a transaction has reported
// (§30/§62) — partial evidence never produces an early Allow.
//
// Duplicate correlation handling (§60): the same CorrelationKey with
// byte-identical action content is an idempotent REPLAY (the same
// transaction id is re-reported, no second transaction exists); the same
// key with different content is an integrity failure — the second proposal
// is rejected and the original transaction is untouched.
// ============================================================================

#include <qiven/runtime/claims.hpp>
#include <qiven/runtime/decision.hpp>
#include <qiven/runtime/executor.hpp>
#include <qiven/runtime/ingress.hpp>
#include <qiven/runtime/transaction.hpp>

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace qiven::runtime
{
// A resolver the core dispatches concurrently. Returns a receipt bound to
// the EXACT requirement identity it was asked about, or nullopt when no
// trusted evidence could be produced (§29: unavailable/timeout/
// unverifiable never becomes Satisfied).
using ResolverFn = std::function<std::optional<resolver::EvidenceReceipt>(const RequirementIdentity&)>;

struct DrainResult
{
    u64 processed          = 0; // messages consumed this drain
    u64 dropped_unknown    = 0; // §61: evidence for unknown transactions
    u64 integrity_failures = 0; // §60: correlation content conflicts
    u64 replays            = 0; // §60: idempotent duplicate proposals
};

struct TransactionView
{
    bool exists = false;
    ControlTransactionId id {};
    TransactionPhase phase  = TransactionPhase::Observed;
    Disposition disposition = Disposition::Deny;
    usize blocking_pending  = 0; // blocking requirements not yet reported
    bool integrity_failed   = false;
};

class ControlCore
{
public:
    ControlCore(TransactionMinter& minter, Executor& executor, ResolverFn resolver, port::PinnedCognition cognition,
                StructuralFacts facts);

    ControlCore(const ControlCore&)            = delete;
    ControlCore& operator=(const ControlCore&) = delete;

    // Process messages until the queue is momentarily empty (bounded quiesce
    // pass). Runs on THE control thread only.
    [[nodiscard]] DrainResult drain(BoundedIngressQueue& ingress);

    // Point-in-time view for the control thread between drains. Evidence
    // receipts produced for satisfied requirements are retrievable here.
    [[nodiscard]] TransactionView view(const ControlTransactionId& id) const;

    [[nodiscard]] const ControlTransaction* transaction(const ControlTransactionId& id) const;

    // All current transaction views (control thread, between drains).
    [[nodiscard]] std::vector<TransactionView> views() const;

    [[nodiscard]] usize transaction_count() const noexcept
    {
        return m_transactions.size();
    }

private:
    void handle_proposal(BoundedIngressQueue& ingress, const IngressMessage& message, DrainResult& result);
    void handle_evidence(const IngressMessage& message, DrainResult& result);
    void handle_failure(const IngressMessage& message, DrainResult& result);
    void dispatch_resolvers(BoundedIngressQueue& ingress, ControlTransaction& transaction);
    void reevaluate_after_reports(ControlTransaction& transaction);

    TransactionMinter& m_minter;
    Executor& m_executor;
    ResolverFn m_resolver;

    std::unordered_map<u64, ControlTransaction> m_transactions;     // by id.value
    std::unordered_map<u64, ControlTransactionId> m_by_correlation; // correlation hash -> id
    std::unordered_map<u64, u64> m_integrity_failures;              // correlation hash -> count
    std::unordered_map<u64, u64> m_outstanding;                     // id -> unreported resolvers
    port::PinnedCognition m_cognition;
    StructuralFacts m_facts;
};
} // namespace qiven::runtime
