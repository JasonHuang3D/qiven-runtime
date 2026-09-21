#include <qiven/runtime/admission.hpp>

namespace qiven::runtime
{
ActionAdmissionCoordinator::ActionAdmissionCoordinator(port::IExecutionAuthorityPort& authority,
                                                       const ReconciliationBarrier& barriers) :
m_authority(authority), m_barriers(barriers)
{
}

AdmissionResult ActionAdmissionCoordinator::compose(const ExecutionDecision& decision,
                                                    const qiven::runtime::ConsumeResult& consumption,
                                                    const ObservedAction& action, const CorrelationKey& correlation,
                                                    port::AdmissionPurpose purpose) &&
{
    AdmissionResult result;

    // Gate 1 — Cognitive Control: only a CONSUMED Allow composes. A stale
    // or replayed decision never reaches the authority plane (§41/§42);
    // a non-Allow disposition never had the cognitive gate at all.
    if (consumption.outcome != ConsumeOutcome::Consumed || decision.disposition != Disposition::Allow)
    {
        result.outcome = AdmissionOutcome::DeniedNoCognitiveAllow;
        return result;
    }

    // Defense in depth (test-caught hole): the PRESENTED action must be
    // the exact action the decision bound. §42 freshness is checked at
    // consumption with caller-supplied facts; the composition re-derives
    // the digest from the action itself so a decision can never be
    // spent on a different proposal than the one it allowed.
    if (action_digest_of(action) != decision.action_digest)
    {
        result.outcome = AdmissionOutcome::DeniedNoCognitiveAllow;
        return result;
    }

    // §51: a barred effect scope blocks dispatch — the authority plane is
    // never even asked (uncertainty is not resolved by execution)
    const EffectScope scope = effect_scope_of(action);
    if (m_barriers.check(scope, MutationRelation::Equivalent) == BarrierCheck::Blocked)
    {
        result.outcome = AdmissionOutcome::DeniedReconciliationBarrier;
        return result;
    }

    // Gate 2 — Execution Authority: the live admission request. Errors are
    // subsystem failures (denied-in-substance comes back as failure codes
    // from the Null implementation; a real subsystem distinguishes typed
    // denials — the Result channel carries both without merging them).
    auto grant = m_authority.request_admission(port::AdmissionRequest { purpose, decision.transaction, decision.token,
                                                                        correlation });
    if (!grant.is_ok())
    {
        const qiven::Error& error = grant.reason();
        if (error.category == qiven::error_category::permission_denied)
        {
            result.outcome = AdmissionOutcome::DeniedByExecutionAuthority;
        }
        else
        {
            result.outcome = AdmissionOutcome::DeniedAuthorityError;
        }
        return result;
    }

    // §85 fencing between admission and dispatch: a superseded lease
    // never dispatches
    if (!m_authority.grant_still_valid(grant.value()))
    {
        m_authority.release(std::move(grant.value()));
        result.outcome = AdmissionOutcome::DeniedStaleGrant;
        return result;
    }

    result.outcome              = AdmissionOutcome::Dispatched;
    result.permit.grant         = std::move(grant.value());
    result.permit.transaction   = decision.transaction;
    result.permit.action_digest = decision.action_digest;
    result.permit.effect_scope  = scope;
    return result;
}
} // namespace qiven::runtime
