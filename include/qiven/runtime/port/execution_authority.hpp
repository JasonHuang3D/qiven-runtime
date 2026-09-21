#pragma once

// ============================================================================
// port/execution_authority.hpp — the execution-authority plane (component
// ADL §43/§44/§85; obligation C-13, design §8.5)
//
// Cognitive Control and machine execution authority are ORTHOGONAL gates
// (§43): the coordinator composes them conjunctively and never merges
// their meanings — a cognitive ALLOW cannot override fencing, override
// quarantine, grant a machine lease, clear reconciliation or bypass
// execution authority (§85); neither receipt satisfies the other.
//
// After ADR-0043 the ADR-0026 authority invariants live as a
// RuntimeHost-INTERNAL execution-authority subsystem behind this port:
// single-writer lease, monotonic fencing epochs, admission, quarantine,
// reconciliation journal. The cognitive core cannot mint authority (it
// can only request it); the day-one implementation is deliberately
// NullExecutionAuthority — fail-closed DENY for every real mutation
// (no real machine-local mutation authority exists until the RuntimeHost
// subsystem is real) and simulated admission for loopback proofs only.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/transaction.hpp>

namespace qiven::runtime::port
{
enum class AdmissionPurpose : u8
{
    RealMutation,   // machine-local mutation with world effects
    SimulatedProof, // loopback/proof dispatch with no world effects
};

struct AdmissionRequest
{
    AdmissionPurpose purpose = AdmissionPurpose::RealMutation;
    ControlTransactionId transaction {};
    DecisionToken token {}; // the consumed cognitive ALLOW (never grants
                            // authority by itself — it is EVIDENCE of the
                            // cognitive gate, not a lease)
    CorrelationKey correlation {};
};

// A machine lease with a fencing epoch (ADR-0026 lineage, absorbed by
// ADR-0043 as RuntimeHost-internal semantics). Movable, not copyable: a
// lease is held, never duplicated.
struct AdmissionGrant
{
    u64 lease_epoch   = 0; // monotonic single-writer epoch
    u64 fencing_token = 0; // staleness fence for dispatch validation
    ControlTransactionId transaction {};

    AdmissionGrant()                                     = default;
    AdmissionGrant(const AdmissionGrant&)                = delete;
    AdmissionGrant& operator=(const AdmissionGrant&)     = delete;
    AdmissionGrant(AdmissionGrant&&) noexcept            = default;
    AdmissionGrant& operator=(AdmissionGrant&&) noexcept = default;
};

class IExecutionAuthorityPort
{
public:
    virtual ~IExecutionAuthorityPort() = default;

    // Request admission. Denied is a typed, expected outcome — never an
    // error. Errors are authority-subsystem failures (unavailable etc.).
    [[nodiscard]] virtual qiven::Result<AdmissionGrant> request_admission(const AdmissionRequest& request) = 0;

    // Release the lease (idempotent; a released grant is never valid again).
    virtual void release(AdmissionGrant grant) = 0;

    // §85 fencing: validate the grant is still the CURRENT lease at
    // dispatch time — a superseded epoch never regains validity.
    [[nodiscard]] virtual bool grant_still_valid(const AdmissionGrant& grant) const = 0;
};

// §45 fail-closed day-one implementation: real mutation is DENIED until a
// real RuntimeHost execution-authority subsystem exists; simulated
// admission exists only so loopback proofs can exercise dispatch paths.
class NullExecutionAuthority final : public IExecutionAuthorityPort
{
public:
    [[nodiscard]] qiven::Result<AdmissionGrant> request_admission(const AdmissionRequest& request) override;
    void release(AdmissionGrant grant) override;
    [[nodiscard]] bool grant_still_valid(const AdmissionGrant& grant) const override;

private:
    u64 m_next_epoch    = 1;
    u64 m_current_lease = 0; // 0 = no active lease (single writer)
    ControlTransactionId m_holder {};
};
} // namespace qiven::runtime::port
