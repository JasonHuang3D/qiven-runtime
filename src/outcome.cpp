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

bool PostActionObserver::observed(const CorrelationKey& correlation) const
{
    return m_prior.contains(correlation);
}

ObservationResult PostActionObserver::observe(const ExecutionDecision& decision,
                                              const CorrelationKey& correlation,
                                              const PostActionObservation& observation)
{
    ObservationResult result;

    // §11.3 complete correlation: EVERY identity field the decision bound
    // is validated, not only the action digest. A mismatch in any field
    // rejects the observation as a correlation mismatch.
    if (observation.transaction.value != decision.transaction.value ||
        observation.action_digest != decision.action_digest ||
        observation.generation.value != decision.generation.value ||
        observation.harness_action.value != correlation.action.value ||
        correlation.generation.value != decision.generation.value ||
        correlation.session.value != decision.session.value ||
        correlation.actor.value != decision.actor.value)
    {
        result.status = ObservationStatus::RejectedCorrelationMismatch;
        return result;
    }

    // §46 one final observation per exact execution, keyed by the FULL
    // CorrelationKey value: an identical re-delivery is an idempotent
    // replay (Accepted with the recorded outcome); any difference is an
    // integrity rejection.
    if (const auto prior = m_prior.find(correlation); prior != m_prior.end())
    {
        const PriorObservation& recorded = prior->second;
        const bool identical             = recorded.harness_action.value == observation.harness_action.value &&
                               recorded.transaction.value == observation.transaction.value &&
                               recorded.generation.value == observation.generation.value &&
                               recorded.action_digest == observation.action_digest &&
                               recorded.outcome == observation.reported;
        if (!identical)
        {
            result.status = ObservationStatus::RejectedDuplicate;
            return result;
        }
        result.status  = ObservationStatus::Accepted;
        result.outcome = recorded.outcome;
        if (result.outcome == ExecutionOutcome::Indeterminate)
        {
            result.barrier_scope  = observation.scope;
            result.barrier_active = true;
        }
        return result;
    }

    m_prior.insert_or_assign(correlation,
                             PriorObservation { observation.harness_action,
                                                observation.transaction,
                                                observation.generation,
                                                observation.action_digest,
                                                observation.reported });

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
