#pragma once

// ============================================================================
// transaction.hpp — ControlTransaction, phase machine and the two
// preparation paths (component ADL §18/§19/§31/§32/§73, design §6)
//
// A ControlTransaction begins when an externally meaningful proposal enters
// a governed boundary and ends in one terminal state (§19). There is no
// post-hoc repair of an accepted judgment: a BeforeJudgment miss stops the
// proposal; the NEXT participant proposal is a NEW transaction that may
// carry causal_parent for traceability and MUST NOT reuse the old action
// authorization (§18). Re-deliberation is a chain of exact transactions,
// never one mutable transaction.
//
// The two ADL semantic gates reduce to the frozen draft predicates
// (design §6, ADR-0038 §7 inherited): JudgmentContinuationAllowed ==
// packet.ready_for_judgment(); ExecutionAllowed ==
// packet.ready_for_execution(); a preparation failure fails both (S0-02).
//
// Phases beyond CognitiveAllowed (FreshnessCheck, admission, dispatch,
// observation, reconciliation) land with their own batches; the enum lists
// the §73 shape, transitions are enabled batch by batch.
// ============================================================================

#include <qiven/runtime/generation.hpp>
#include <qiven/runtime/intent.hpp>
#include <qiven/runtime/observed_action.hpp>
#include <qiven/runtime/port/cognition_port.hpp>
#include <qiven/runtime/requirement.hpp>

#include <qiven/context/runtime.hpp>

#include <optional>

namespace qiven::runtime
{
enum class TransactionPhase : u8
{
    Observed,
    Classified,
    PolicyPinned,
    RequirementsDerived,
    PreparingBeforeJudgment,
    Denied,                 // terminal: requirement failed / policy blocks
    ReDeliberationRequired, // terminal: resolution supplies cognition
    PreparedForJudgment,
    PreparingBeforeExecution,
    AwaitingRequirement, // trusted external requirement still pending
    CognitiveAllowed,
    // later batches:
    Stale, // freshness miss: re-enter Cognitive Control (§41)
};

struct ControlTransaction
{
    ControlTransactionId id {};
    std::optional<ControlTransactionId> causal_parent; // re-deliberation chain (§18)
    CorrelationKey correlation {};
    ObservedAction action;
    IntentSet intents;
    port::PinnedCognition cognition;
    qiven::context::PreparationPacket packet; // FROZEN draft type
    TransactionPhase phase = TransactionPhase::Observed;

    [[nodiscard]] bool terminal() const noexcept
    {
        return phase == TransactionPhase::Denied || phase == TransactionPhase::ReDeliberationRequired;
    }
};

// Monotonic per-host transaction identity. One authority per host
// (mirrors GenerationMinter).
class TransactionMinter
{
public:
    TransactionMinter()                                    = default;
    TransactionMinter(const TransactionMinter&)            = delete;
    TransactionMinter& operator=(const TransactionMinter&) = delete;

    [[nodiscard]] ControlTransactionId next() noexcept
    {
        return ControlTransactionId { m_next_value++ };
    }

    [[nodiscard]] u64 last_value() const noexcept
    {
        return m_next_value - 1;
    }

private:
    u64 m_next_value = 1;
};

// Begin one exact proposal. The packet is supplied by the caller (the
// derivation pipeline fills it); the phase starts where the caller has
// evidence for: RequirementsDerived when a derived requirement set has been
// folded into the packet.
[[nodiscard]] ControlTransaction begin_control_transaction(ControlTransactionId id,
                                                           std::optional<ControlTransactionId> causal_parent,
                                                           const CorrelationKey& correlation,
                                                           ObservedAction action,
                                                           IntentSet intents,
                                                           port::PinnedCognition cognition,
                                                           qiven::context::PreparationPacket packet);

// ADL §31 + §73 PreparingBeforeJudgment evaluation. Pure with respect to
// everything except the transaction's own phase:
//   - packet.ready_for_judgment()            -> PreparedForJudgment
//   - preparation failure (S0-02)            -> Denied (fails both gates)
//   - a blocking BeforeJudgment requirement Failed -> Denied
//   - a blocking BeforeJudgment requirement Pending -> ReDeliberationRequired
//     (resolution supplies cognition; the next proposal is a NEW
//     transaction with causal_parent)
// Returns false when the transaction was already terminal (no repair).
[[nodiscard]] bool evaluate_before_judgment(ControlTransaction& transaction);

// ADL §32 + §73 PreparingBeforeExecution evaluation. Requires
// PreparedForJudgment or AwaitingRequirement:
//   - packet.ready_for_execution()           -> CognitiveAllowed
//   - a blocking BeforeExecution requirement Failed -> Denied
//   - a blocking BeforeExecution requirement Pending -> AwaitingRequirement
//     (trusted external requirement still pending; disposition Await)
// BeforeExecution requirements may be satisfied without another judgment
// cycle ONLY while the exact action stays unchanged — reclassification on
// change is enforced through action-digest freshness (RCA-7).
[[nodiscard]] bool evaluate_before_execution(ControlTransaction& transaction);

// §75 disposition projection of the current phase (what a harness would be
// told). NotGoverned is produced by claim coverage analysis, not phases.
enum class Disposition : u8
{
    Allow,
    ReDeliberate,
    Await,
    Deny,
};

[[nodiscard]] Disposition disposition_for(const ControlTransaction& transaction) noexcept;
} // namespace qiven::runtime
