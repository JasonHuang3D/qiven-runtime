#include <qiven/runtime/reconciliation.hpp>

namespace qiven::runtime
{
void ReconciliationBarrier::raise(EffectScope scope, const ControlTransactionId& transaction)
{
    m_scopes.emplace(scope.id, transaction);
}

BarrierCheck ReconciliationBarrier::check(const EffectScope& scope, MutationRelation relation) const noexcept
{
    if (relation == MutationRelation::Independent)
    {
        return BarrierCheck::Allowed; // §51: independent scopes continue
    }
    return m_scopes.contains(scope.id) ? BarrierCheck::Blocked : BarrierCheck::Allowed;
}

bool ReconciliationBarrier::active(const EffectScope& scope) const noexcept
{
    return m_scopes.contains(scope.id);
}

void ReconciliationBarrier::lift(const EffectScope& scope) noexcept
{
    m_scopes.erase(scope.id);
}

ReconciliationCoordinator::ReconciliationCoordinator(ReconciliationBarrier& barrier) :
m_barrier(barrier)
{
}

ReconciliationCoordinator::Outcome ReconciliationCoordinator::reconcile(const EffectScope& scope,
                                                                        ReconciliationEvidence evidence,
                                                                        const std::string& detail)
{
    Outcome out;
    ++m_resolutions;

    if (evidence != ReconciliationEvidence::AuthoritativeObservation || detail.empty())
    {
        // no authoritative answer: STILL indeterminate — the barrier stays
        // and nothing becomes failure or permission (§53)
        out.result = ReconciliationResult::StillIndeterminate;
        return out;
    }

    m_barrier.lift(scope);

    // the caller distinguishes observed-success from observed-failure
    // through the detail contract: detail starting with "failed:" is an
    // authoritative failure observation; anything else is authoritative
    // success evidence. (Day-one shape: the real resolver ports carry
    // typed evidence with RCA-14; the contract — authoritative-only — is
    // what this batch proves.)
    if (detail.rfind("failed:", 0) == 0)
    {
        out.result = ReconciliationResult::ResolvedFailed;
        qiven::context::FailureFingerprint fingerprint;
        fingerprint.operation     = "reconciled";
        fingerprint.category      = "authoritative-observation";
        fingerprint.stableMessage = qiven::context::normalize_failure_text(detail.substr(7));
        out.fingerprint           = fingerprint;
    }
    else
    {
        out.result = ReconciliationResult::ResolvedSucceeded;
    }
    return out;
}
} // namespace qiven::runtime
