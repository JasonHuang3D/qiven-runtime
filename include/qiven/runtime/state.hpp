#pragma once

// ============================================================================
// state.hpp — the single-threaded control core (component ADL §5/§30/§60/
// §61/§62, design §9; production-MVP architecture §8.2, MVP-0)
//
// ONE thread drains the ingress queue and owns ALL control state and phase
// transitions. Resolvers run on the Executor and report back as ingress
// messages; they never touch state. Readiness is re-evaluated only after
// EVERY blocking requirement instance of a transaction has reported
// (§30/§62) — partial evidence never produces an early Allow.
//
// BeforeJudgment timing (production-MVP §8.2 — the MVP-0 correction): a
// BeforeJudgment requirement that is discovered and resolved INSIDE the
// current proposal can never authorize that proposal. When in-flight
// evidence satisfies a blocking BeforeJudgment requirement, the
// transaction ends ReDeliberationRequired with the receipt retained as
// resume context; only the NEXT proposal — a NEW transaction carrying
// causal_parent and an unexpired activation receipt whose type, source,
// digest and generation match the new judgment — may pre-satisfy that
// requirement, and only at proposal time, before the judgment opens.
// Nothing may decide first, investigate later, and retroactively
// authorize the original decision.
//
// Duplicate correlation handling (§60): the same CorrelationKey with
// byte-identical action content is an idempotent REPLAY (the same
// transaction id is re-reported, no second transaction exists); the same
// key with different content is an integrity failure — the second proposal
// is rejected and the original transaction is untouched. Correlation is
// keyed by the FULL CorrelationKey value; its fnv1a64 hash accelerates
// bucketing only and is never treated as identity (§10.3).
// ============================================================================

#include <qiven/runtime/claims.hpp>
#include <qiven/runtime/decision.hpp>
#include <qiven/runtime/executor.hpp>
#include <qiven/runtime/ingress.hpp>
#include <qiven/runtime/transaction.hpp>

#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qiven::runtime
{
// A resolver mechanism the core dispatches concurrently THROUGH the
// accepted registry binding. Returns a receipt bound to the EXACT
// requirement identity it was asked about, or nullopt when no trusted
// evidence could be produced (§29: unavailable/timeout/unverifiable
// never becomes Satisfied). The mechanism never selects itself: the
// binding (type, version, view acceptance) comes from the registry.
using ResolverMechanism = std::function<std::optional<resolver::EvidenceReceipt>(
    const resolver::ResolverBinding&, const RequirementIdentity&, const resolver::ReceiptContext&)>;

struct DrainResult
{
    u64 processed          = 0; // messages consumed this drain
    u64 dropped_unknown    = 0; // §61: evidence for unknown transactions
    u64 integrity_failures = 0; // §60: correlation content conflicts
    u64 replays            = 0; // §60: idempotent duplicate proposals
    u64 receipt_rejections = 0; // §8.3: receipts rejected at validation
};

struct TransactionView
{
    bool exists = false;
    ControlTransactionId id {};
    std::optional<ControlTransactionId> causal_parent; // §18 re-deliberation chain
    TransactionPhase phase  = TransactionPhase::Observed;
    Disposition disposition = Disposition::Deny;
    usize blocking_pending  = 0; // blocking requirements not yet reported
    bool integrity_failed   = false;
};

class ControlCore
{
public:
    ControlCore(TransactionMinter& minter,
                Executor& executor,
                const resolver::RequirementResolverRegistry& registry,
                ResolverMechanism mechanism,
                port::PinnedCognition cognition,
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

    // §8.2 step 4: resume context of a ReDeliberate transaction — the
    // receipts produced while resolving its late-discovered BeforeJudgment
    // requirements. Presented to the participant so the NEW proposal can
    // carry equivalent activation receipts.
    [[nodiscard]] const std::vector<resolver::EvidenceReceipt>& resume_receipts(const ControlTransactionId& id) const;

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

    [[nodiscard]] const resolver::ResolverBinding* binding_for(const RequirementIdentity& identity) const;

    TransactionMinter& m_minter;
    Executor& m_executor;
    const resolver::RequirementResolverRegistry& m_registry;
    ResolverMechanism m_mechanism;

    std::unordered_map<u64, ControlTransaction> m_transactions;                // by id.value
    std::unordered_map<CorrelationKey, ControlTransactionId> m_by_correlation; // FULL key value (§10.3)
    std::unordered_map<CorrelationKey, u64> m_integrity_failures;              // full key -> count
    std::unordered_map<u64, u64> m_outstanding;                                // id -> unreported resolvers
    std::unordered_set<u64> m_redeliberation_pending;                          // txs whose BeforeJudgment resolved in-flight (§8.2)
    std::unordered_map<u64, std::vector<resolver::EvidenceReceipt>> m_resume;  // tx -> resume receipts
    std::unordered_set<u64> m_spent_activations;                               // activation_use_identity of consumed receipts
    port::PinnedCognition m_cognition;
    StructuralFacts m_facts;
};
} // namespace qiven::runtime
