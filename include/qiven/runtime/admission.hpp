#pragma once

// ============================================================================
// admission.hpp — ActionAdmissionCoordinator: the conjunctive composition
// of the two orthogonal gates (component ADL §43; obligation C-13)
//
//   Cognitive Control ALLOW   AND   Execution Authority admission
//                       ↓
//               dispatch permitted
//
// The coordinator does NOT merge their meanings (§43): a cognitive ALLOW
// is evidence the epistemic/policy prerequisites held for one exact
// action; an authority grant is evidence this caller/action is currently
// admitted to execute under machine authority. Neither receipt satisfies
// the other, and the cognitive decision never EMBEDS authority (design
// §10 — host admission is a separate live gate; §85 — no second machine
// authority).
// ============================================================================

#include <qiven/runtime/decision.hpp>
#include <qiven/runtime/outcome.hpp>
#include <qiven/runtime/port/execution_authority.hpp>
#include <qiven/runtime/reconciliation.hpp>

namespace qiven::runtime
{
struct DispatchPermit
{
    port::AdmissionGrant grant;
    ControlTransactionId transaction {};
    ContentDigest action_digest {};

    // The post-action observation the adapter must report back through
    // the tri-state channel (RCA-9) once the mechanism answers.
    EffectScope effect_scope {};
};

enum class AdmissionOutcome : u8
{
    Dispatched,
    DeniedNoCognitiveAllow,     // the cognitive gate did not pass
    DeniedByExecutionAuthority, // fail-closed authority (e.g. real
                                // mutation under NullExecutionAuthority)
    DeniedAuthorityError,       // authority subsystem failure
    DeniedStaleGrant,           // the lease was superseded before dispatch
    DeniedReconciliationBarrier // §51: the effect scope is barred
};

struct AdmissionResult
{
    AdmissionOutcome outcome = AdmissionOutcome::DeniedNoCognitiveAllow;
    DispatchPermit permit {}; // valid only when Dispatched
};

class ActionAdmissionCoordinator
{
public:
    ActionAdmissionCoordinator(port::IExecutionAuthorityPort& authority, const ReconciliationBarrier& barriers);

    // The §43 composition. decision: the (consumed, Consumed-outcome)
    // cognitive decision; correlation: the transaction's boundary key
    // (decisions deliberately bind digests, not correlation keys — the
    // caller, holding the transaction, supplies it); purpose: real vs
    // simulated dispatch.
    [[nodiscard]] AdmissionResult compose(const ExecutionDecision& decision,
                                          const qiven::runtime::ConsumeResult& consumption,
                                          const ObservedAction& action, const CorrelationKey& correlation,
                                          port::AdmissionPurpose purpose) &&;

private:
    port::IExecutionAuthorityPort& m_authority;
    const ReconciliationBarrier& m_barriers;
};
} // namespace qiven::runtime
