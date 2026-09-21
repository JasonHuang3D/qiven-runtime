#include <qiven/runtime/port/execution_authority.hpp>

namespace qiven::runtime::port
{
qiven::Result<AdmissionGrant> NullExecutionAuthority::request_admission(const AdmissionRequest& request)
{
    if (request.purpose == AdmissionPurpose::RealMutation)
    {
        // §45/§85 fail-closed: no real machine-local mutation authority
        // exists at this landing; DENY is the truthful answer, not an error
        return qiven::Result<AdmissionGrant>::fail(
            qiven::Error::make(qiven::error_category::permission_denied, 40,
                               "real mutation denied: no execution-authority subsystem is enabled"));
    }

    // Simulated admission: single-writer semantics still hold — a second
    // request while a lease is active is refused fail-closed (ADR-0026
    // lineage), so proofs exercise the real invariant, not a fake one.
    if (m_current_lease != 0)
    {
        return qiven::Result<AdmissionGrant>::fail(
            qiven::Error::make(qiven::error_category::permission_denied, 41,
                               "single-writer: another lease is active"));
    }

    AdmissionGrant grant;
    grant.lease_epoch   = m_next_epoch++;
    grant.fencing_token = grant.lease_epoch; // epoch IS the fence day-one
    grant.transaction   = request.transaction;
    m_current_lease     = grant.lease_epoch;
    m_holder            = request.transaction;
    return qiven::Result<AdmissionGrant>(std::move(grant));
}

void NullExecutionAuthority::release(AdmissionGrant grant)
{
    if (m_current_lease == grant.lease_epoch)
    {
        m_current_lease = 0;
        m_holder        = ControlTransactionId {};
    }
    // releasing a stale grant is a no-op: a superseded epoch never had
    // authority to free the current lease
}

bool NullExecutionAuthority::grant_still_valid(const AdmissionGrant& grant) const
{
    return grant.lease_epoch != 0 && m_current_lease == grant.lease_epoch;
}
} // namespace qiven::runtime::port
