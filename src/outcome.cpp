#include <qiven/runtime/outcome.hpp>

#include <qiven/hashing.hpp>

namespace qiven::runtime
{
EffectScope effect_scope_of(const ObservedAction& action) noexcept
{
    u64 seed = fnv1a64(action.target);
    seed ^= action.capability.value;
    seed *= fnv1a64_prime;
    return EffectScope { seed };
}

ObservationResult PostActionObserver::observe(const ExecutionDecision& decision, const PostActionObservation& observation,
                                              bool already_observed) const
{
    ObservationResult result;

    if (observation.action_digest != decision.action_digest)
    {
        result.status = ObservationStatus::RejectedCorrelationMismatch;
        return result;
    }
    if (already_observed)
    {
        // one correlated observation per exact execution (§46); a second
        // report with any different claim is an integrity conflict. A
        // byte-identical re-delivery is handled by the caller's ledger as
        // an idempotent replay before reaching the observer.
        result.status = ObservationStatus::RejectedDuplicate;
        return result;
    }

    result.outcome = observation.reported;

    if (observation.reported == ExecutionOutcome::Failed)
    {
        if (observation.evidence.empty())
        {
            // failure must be mechanically ESTABLISHED (§15/§48): a bare
            // claim cannot mint a fingerprint
            result.outcome = ExecutionOutcome::Indeterminate;
        }
        else
        {
            qiven::context::FailureFingerprint fingerprint;
            fingerprint.operation     = "post-action";
            fingerprint.category      = "mechanism";
            fingerprint.stableMessage = qiven::context::normalize_failure_text(observation.evidence);
            result.fingerprint        = fingerprint;
        }
    }
    else if (observation.reported == ExecutionOutcome::Succeeded && observation.evidence.empty())
    {
        // success must be PROVEN under the mechanism contract; "no
        // acknowledgement" proves nothing (§15)
        result.outcome = ExecutionOutcome::Indeterminate;
    }

    if (result.outcome == ExecutionOutcome::Indeterminate)
    {
        result.barrier_scope  = observation.scope;
        result.barrier_active = true;
    }
    return result;
}
} // namespace qiven::runtime
