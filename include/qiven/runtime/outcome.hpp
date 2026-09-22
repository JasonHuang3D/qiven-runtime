#pragma once

// ============================================================================
// outcome.hpp — post-action tri-state observation (component ADL §15/§46/
// §47/§49/§50; obligation C-14, design §11 first half; production-MVP
// architecture §11.3, MVP-0)
//
// After mechanism dispatch the adapter submits ONE correlated observation.
// The observer validates the COMPLETE correlation identity — generation,
// session, transaction, action, harness action, digests, expected base —
// never a digest plus an already_observed boolean (§11.3: comparing only
// an action digest and a boolean is insufficient). The prior-observation
// ledger is keyed by the FULL CorrelationKey value; its fnv hash
// accelerates bucketing only.
//
// Indeterminate is MANDATORY when completion cannot be proven: "no success
// acknowledgement" is never Failed (§15) and never a permission to retry
// (§49) — it produces a ReconciliationBarrier over the action's
// EffectScope, not a FailureFingerprint. Conflicting observations for the
// same exact execution are an integrity failure (§46).
//
// EffectScope is derived mechanically from the ObservedAction (§50): the
// world region for which an unknown outcome is unsafe.
// ============================================================================

#include <qiven/runtime/decision.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/observed_action.hpp>

#include <qiven/context/runtime.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace qiven::runtime
{
enum class ExecutionOutcome : u8
{
    Succeeded,
    Failed,
    Indeterminate,
};

// Mechanically derived effect scope (§50). Day-one derivation: the
// action's target within its capability class — adapter-specific schemas
// refine this without changing the type.
struct EffectScope
{
    u64 id { 0 }; // fnv1a64 over capability + target

    [[nodiscard]] bool operator==(const EffectScope& other) const noexcept
    {
        return id == other.id;
    }
    [[nodiscard]] bool operator!=(const EffectScope& other) const noexcept
    {
        return id != other.id;
    }
};

[[nodiscard]] EffectScope effect_scope_of(const ObservedAction& action) noexcept;

struct PostActionObservation
{
    ControlTransactionId transaction {};
    HarnessActionId harness_action {};
    ContentDigest action_digest {};    // of the EXACT dispatched action
    RuntimeGenerationId generation {}; // the generation the decision bound
    ExecutionOutcome reported = ExecutionOutcome::Indeterminate;
    EffectScope scope {}; // mechanically derived by the
                          // adapter from the dispatched
                          // ObservedAction (ADL 50); the
                          // digest alone cannot recover it
    std::string evidence; // outcome evidence under the
                          // mechanism contract (empty or
                          // "no acknowledgement" evidence
                          // cannot establish Succeeded/Failed)
};

enum class ObservationStatus : u8
{
    Accepted,
    RejectedCorrelationMismatch, // ANY correlation field disagrees with the
                                 // consumed decision's bound facts (§11.3)
    RejectedDuplicate,           // a DIFFERENT observation already exists for
                                 // this exact execution (§46 integrity)
    RejectedUnknownDecision,
};

struct ObservationResult
{
    ObservationStatus status = ObservationStatus::Accepted;
    ExecutionOutcome outcome = ExecutionOutcome::Indeterminate;
    // valid when Accepted:
    qiven::context::FailureFingerprint fingerprint {}; // Failed only (§48)
    EffectScope barrier_scope {};                      // Indeterminate only (§49)
    bool barrier_active = false;
};

// Validates the COMPLETE correlation against the consumed decision's
// bound facts and classifies. The observer owns the prior-observation
// ledger keyed by the full CorrelationKey value: one final observation
// per exact execution; a second submission of the SAME observation is an
// idempotent replay, a DIFFERENT one is an integrity rejection.
class PostActionObserver
{
public:
    // decision: the consumed ALLOW (its digests bind the exact action);
    // correlation: the full five-tuple of the exact execution;
    // observation: the adapter's report.
    [[nodiscard]] ObservationResult observe(const ExecutionDecision& decision,
                                            const CorrelationKey& correlation,
                                            const PostActionObservation& observation);

    [[nodiscard]] bool observed(const CorrelationKey& correlation) const;

private:
    struct PriorObservation
    {
        HarnessActionId harness_action {};
        ControlTransactionId transaction {};
        RuntimeGenerationId generation {};
        ContentDigest action_digest {};
        ExecutionOutcome outcome = ExecutionOutcome::Indeterminate;
    };

    std::unordered_map<CorrelationKey, PriorObservation> m_prior;
};
} // namespace qiven::runtime
