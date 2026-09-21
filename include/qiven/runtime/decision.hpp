#pragma once

// ============================================================================
// decision.hpp — ExecutionDecision, DecisionBinder and the single-use
// freshness-checked consumption (component ADL §39-§42, design §10;
// obligations C-10/C-11/C-12)
//
// An ALLOW decision binds the controlled world exactly: the action digest,
// intent-set digest, pinned cognition revision, policy digest, requirement
// and evidence-set digests, profile and resolver-registry revisions, the
// generation, and a single-use token. Host lease/fencing authority is
// intentionally NOT part of the decision (§39) — admission stays an
// independent live gate (RCA-13).
//
// ALLOW is logically single-use (§40): it means THIS exact action under
// THIS exact controlled state passed the gate — never
// "may-execute-forever". Consumption runs the §42 final freshness check:
// any bound fact changed => Stale (the action re-enters Cognitive Control;
// it never receives an incremental patch, §41).
//
// Day-one freshness scope (recorded): action / generation / pinned
// revision / policy / resolver-registry / evidence-set digests and the
// not-previously-consumed rule. Evidence-validity expiry and
// reconciliation-barrier checks land with their batches (RCA-10/11).
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/resolver.hpp>
#include <qiven/runtime/transaction.hpp>

#include <optional>
#include <unordered_set>

namespace qiven::runtime
{
struct ExecutionDecision
{
    DecisionToken token {};
    ControlTransactionId transaction {};
    RuntimeGenerationId generation {};
    ContentDigest action_digest {};
    ContentDigest intent_set_digest {};
    qiven::context::RevisionId cognition_revision;
    ContentDigest policy_digest {};
    ContentDigest requirement_set_digest {};
    ContentDigest evidence_set_digest {};
    ProfileRevision profile {};
    u64 resolver_registry_revision = 0;
    std::optional<u64> classifier_contract; // day-one: structural only
    Disposition disposition = Disposition::Deny;
};

// §42 facts the admission side revalidates immediately before consuming
// the decision. Evidence re-check is optional day-one (nullopt = not
// revalidated in this check; documented residual).
struct FreshnessFacts
{
    ContentDigest action_digest {};
    RuntimeGenerationId generation {};
    qiven::context::RevisionId cognition_revision;
    ProfileRevision profile {};
    u64 resolver_registry_revision = 0;
    std::optional<ContentDigest> evidence_set_digest;
};

enum class ConsumeOutcome : u8
{
    Consumed,
    AlreadyConsumed, // C-12: the token was spent; one decision, one execution
    StaleAction,     // C-11: arguments/target/payload changed
    StaleGeneration, // C-11: the runtime crossed into a new generation
    StaleCognition,  // C-11: the canonical head advanced past the pin
    StalePolicy,     // C-11: the profile revision changed
    StaleRegistry,   // C-11: the resolver registry changed
    StaleEvidence,   // C-11: the evidence set changed
};

struct ConsumeResult
{
    ConsumeOutcome outcome = ConsumeOutcome::Consumed;
};

// Binds an immutable ALLOW decision for a transaction that has reached
// CognitiveAllowed. The digests are computed here, once — the caller never
// supplies them piecemeal. Fails closed (typed Result) when the
// transaction is not execution-ready.
[[nodiscard]] qiven::Result<ExecutionDecision> bind_allow(const ControlTransaction& transaction,
                                                          const RuntimeGeneration& generation,
                                                          u64 resolver_registry_revision,
                                                          const std::vector<resolver::EvidenceReceipt>& evidence);

// Canonical digest over one exact observed action (also the freshness
// preimage: a stale action is any byte of the binding surface changing).
[[nodiscard]] ContentDigest action_digest_of(const ObservedAction& action);

class DecisionLedger
{
public:
    DecisionLedger() = default;

    // §42 final freshness check + §40 one-shot consumption, atomically.
    // A stale decision is burned (recorded as consumed): §41 sends the
    // action back into Cognitive Control as a NEW proposal, and the spent
    // token prevents replaying the old ALLOW through a patched path.
    [[nodiscard]] ConsumeResult consume(const ExecutionDecision& decision, const FreshnessFacts& facts);

    [[nodiscard]] bool was_consumed(u64 token_value) const noexcept;

private:
    std::unordered_set<u64> m_consumed;
};
} // namespace qiven::runtime
