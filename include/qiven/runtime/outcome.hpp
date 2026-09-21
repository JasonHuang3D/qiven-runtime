#pragma once

// ============================================================================
// outcome.hpp — post-action tri-state observation (component ADL §15/§46/
// §47/§49/§50; obligation C-14, design §11 first half)
//
// After mechanism dispatch the adapter submits ONE correlated observation.
// The observer validates the correlation (transaction, harness action,
// action digest) and classifies Succeeded / Failed / Indeterminate.
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
    ContentDigest action_digest {}; // of the EXACT dispatched action
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
    RejectedCorrelationMismatch, // digest or harness action disagrees with
                                 // the decision's bound facts (§46)
    RejectedDuplicate,           // a different outcome already observed for
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

// Validates correlation against the consumed decision's bound facts and
// classifies. Pure with respect to everything except the passed ledger of
// already-observed executions (duplicate/conflict detection).
class PostActionObserver
{
public:
    // decision: the consumed ALLOW (its digest binds the exact action);
    // observation: the adapter's report. first_seen: whether an
    // observation for this transaction was already recorded (the caller
    // owns the ledger; day-one the caller passes a seen-set).
    [[nodiscard]] ObservationResult observe(const ExecutionDecision& decision, const PostActionObservation& observation,
                                            bool already_observed) const;
};
} // namespace qiven::runtime
