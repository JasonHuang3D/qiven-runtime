#pragma once

// ============================================================================
// port/activation.hpp — activation boundary (component ADL §13/§66;
// production-MVP architecture §8.2, MVP-0)
//
// Activation events (session start, prompt submission, task/workflow
// transition, explicit declared intent) are an OPTIMIZATION and
// PREPARATION opportunity: they may preload cognition and mint trusted
// activation receipts. Correctness NEVER depends on activation predicting
// every later action — when a requirement becomes knowable only at
// proposal time, the action boundary (interception) stays authoritative.
//
// An activation receipt (§66) proves cognition was injected before a
// specific proposal: subject, canonical revision, actor/session,
// injection event identity. MVP-0 hardens this into the §8.2 re-
// deliberation protocol: a BeforeJudgment requirement discovered and
// resolved inside a proposal MUST NOT authorize that proposal — the
// transaction ends ReDeliberate, and only an unexpired receipt whose
// type, source, digest and generation match the NEW judgment may
// pre-satisfy its BeforeJudgment requirement. Pre-satisfaction happens
// at proposal time (before the judgment opens), never inside it.
// ============================================================================

#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/resolver.hpp>

#include <qiven/context/cognition.hpp>

#include <optional>
#include <string>
#include <unordered_set>

namespace qiven::runtime::port
{
enum class ActivationKind : u8
{
    SessionStart,
    PromptSubmission,
    TaskTransition,
    WorkflowTransition,
    DeclaredIntent,
};

struct ActivationEvent
{
    ActivationKind kind = ActivationKind::SessionStart;
    std::string detail;
};

// §66: cognition injected at activation may satisfy a later requirement
// only through such a receipt — preload without proof is not evidence.
// §8.2 step 7 match dimensions: evidence type, source revision, injected
// content digest, and generation must all match the NEW judgment; the
// receipt is single-use and expiring.
struct ActivationReceipt
{
    ActivationKind kind = ActivationKind::SessionStart;
    std::string subject;
    qiven::context::RevisionId canonical_revision; // the revision the new judgment will pin
    ContentDigest injected_digest;                 // digest of the injected cognition content
    resolver::EvidenceType evidence_type = resolver::EvidenceType::CanonicalRecord;
    RuntimeGenerationId generation {}; // generation the receipt was minted under
    u64 actor_token     = 0;
    u64 session_token   = 0;
    u64 injection_event = 0; // identity of the activation event
    u64 expires_at_ms   = 0; // epoch ms; 0 = no expiry claimed

    [[nodiscard]] bool complete() const noexcept
    {
        return !subject.empty() && !canonical_revision.value.empty() && actor_token != 0 && session_token != 0 &&
               injection_event != 0;
    }

    // §8.2 step 7 validity against the NEW judgment's facts: subject and
    // evidence type match the requirement, the revision matches the
    // pinned cognition, the digest matches the injected content identity,
    // the generation matches, and the receipt is unexpired. `now_ms` = 0
    // disables the expiry check (test-only).
    [[nodiscard]] bool valid_for(const RequirementIdentity& requirement,
                                 const qiven::context::RevisionId& pinned_revision,
                                 const ContentDigest& pinned_digest,
                                 const RuntimeGenerationId& live_generation,
                                 const ContentDigest& live_policy_digest,
                                 u64 now_ms) const noexcept;
};

class IActivationBoundaryPort
{
public:
    virtual ~IActivationBoundaryPort() = default;

    // Record an activation; returns the injection-event identity the
    // receipt must cite (0 = activation not tracked).
    [[nodiscard]] virtual u64 notify_activation(const ActivationEvent& event) = 0;
};

// MVP-0 ledger: mints activation receipts for recorded events and enforces
// their single use. RuntimeHost composition (MVP-3) owns the durable
// instance; the core consumes receipts carried by proposals and consults
// this ledger's spent-set semantics through its own per-core bookkeeping.
class ActivationLedger final : public IActivationBoundaryPort
{
public:
    [[nodiscard]] u64 notify_activation(const ActivationEvent& event) override;

    // Mint a receipt citing a recorded injection event. Fails closed
    // (nullopt) when the event identity is unknown or the receipt is
    // incomplete.
    [[nodiscard]] std::optional<ActivationReceipt> mint_receipt(const ActivationReceipt& requested) const;

    // Single-use enforcement: a receipt may pre-satisfy exactly one
    // requirement of one judgment. Returns true when the receipt was
    // unspent and is now spent; false when it was already spent.
    [[nodiscard]] bool try_spend(const ActivationReceipt& receipt);

    [[nodiscard]] bool is_spent(const ActivationReceipt& receipt) const noexcept;

private:
    u64 m_next_event = 1;
    std::unordered_set<u64> m_recorded_events;
    std::unordered_set<u64> m_spent; // keyed by (generation, injection_event, subject) hash
};

// Identity used for spent-bookkeeping: two receipts with the same
// generation, injection event and subject are the same authorization
// opportunity regardless of other field drift.
[[nodiscard]] u64 activation_use_identity(const ActivationReceipt& receipt) noexcept;
} // namespace qiven::runtime::port
