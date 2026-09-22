#pragma once

// ============================================================================
// decision.hpp — ExecutionDecision, DecisionBinder and the single-use
// freshness-checked consumption (component ADL §39-§42, design §10;
// obligations C-10/C-11/C-12; production-MVP architecture §10.3, MVP-0)
//
// An ALLOW decision binds the controlled world exactly: the action digest
// (payload), intent-set digest, pinned cognition revision, policy digest,
// requirement and evidence-set digests, profile and resolver-registry
// revisions, the generation, the actor/session/capability/operation/
// target scope tuple, the profile's governed resource-scope digest, an
// expiry, and a single-use cryptographic token. The token is HMAC-SHA256
// over the canonical binding preimage under an installation secret,
// domain-separated by a CSPRNG nonce; the ledger stores only the
// SHA-256 token hash — the plaintext token exists only in the immediate
// issue/use path (§10.3). Host lease/fencing authority is intentionally
// NOT part of the decision (§39) — admission stays an independent live
// gate (RCA-13).
//
// ALLOW is logically single-use (§40): it means THIS exact action under
// THIS exact controlled state passed the gate — never
// "may-execute-forever". Consumption runs the §42 final freshness check:
// any bound fact changed => Stale (the action re-enters Cognitive Control;
// it never receives an incremental patch, §41), a tampered binding =>
// InvalidToken (fail-closed), and a past-expiry token => Expired.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/auth.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/resolver.hpp>
#include <qiven/runtime/transaction.hpp>

#include <optional>
#include <unordered_set>

namespace qiven::runtime
{
struct ExecutionDecision
{
    DecisionId id {};        // 128-bit sortable decision identity
    DecisionToken token {};  // issue-path only; never persisted
    TokenHash token_hash {}; // the stored/compared single-use identity
    ControlTransactionId transaction {};
    RuntimeGenerationId generation {};
    HarnessSessionId session {}; // §10.3 binding tuple
    ActorInstanceId actor {};
    CapabilityId capability {};
    std::string operation;
    std::string target;                     // the action's resource scope
    ContentDigest resource_scope_digest {}; // digest of the profile's governed scope
    u64 expires_at_ms = 0;
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
    ContentDigest resource_scope_digest {};
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
    StaleScope,      // C-11: the governed resource scope changed
    Expired,         // §10.3: the authorization window closed
    InvalidToken,    // §10.3: HMAC mismatch — tampered or forged binding
};

struct ConsumeResult
{
    ConsumeOutcome outcome = ConsumeOutcome::Consumed;
};

// The single-use identity of a token: SHA-256 over (mac || nonce). The
// ledger, the journal and restart reconstruction carry ONLY this hash.
[[nodiscard]] TokenHash token_hash_of(const DecisionToken& token);

// Binds an immutable ALLOW decision for a transaction that has reached
// CognitiveAllowed. The digests are computed here, once — the caller never
// supplies them piecemeal. Fails closed (typed Result) when the
// transaction is not execution-ready. `scope_digest` binds the profile's
// governed resource scope; `now_ms` + `ttl_ms` set the expiry window.
[[nodiscard]] qiven::Result<ExecutionDecision> bind_allow(const ControlTransaction& transaction,
                                                          const RuntimeGeneration& generation,
                                                          const ContentDigest& resource_scope_digest,
                                                          u64 resolver_registry_revision,
                                                          const std::vector<resolver::EvidenceReceipt>& evidence,
                                                          const auth::SecretKey& secret,
                                                          SortableIdMinter& id_minter,
                                                          u64 now_ms,
                                                          u64 ttl_ms);

// Canonical digest over one exact observed action (also the freshness
// preimage: a stale action is any byte of the binding surface changing).
[[nodiscard]] ContentDigest action_digest_of(const ObservedAction& action);

// Recomputes the HMAC over the decision's declared binding; false means
// the decision no longer proves what it claims (tamper or forgery).
[[nodiscard]] bool decision_binding_valid(const ExecutionDecision& decision, const auth::SecretKey& secret);

class DecisionLedger
{
public:
    // The secret validates bindings at consumption time; the ledger
    // itself persists only token hashes.
    explicit DecisionLedger(auth::SecretKey secret) :
    m_secret(std::move(secret))
    {
    }

    // §42 final freshness check + §40 one-shot consumption, atomically.
    // A stale decision is burned (recorded as consumed): §41 sends the
    // action back into Cognitive Control as a NEW proposal, and the spent
    // token prevents replaying the old ALLOW through a patched path.
    [[nodiscard]] ConsumeResult consume(const ExecutionDecision& decision,
                                        const FreshnessFacts& facts,
                                        u64 now_ms);

    [[nodiscard]] bool was_consumed(const TokenHash& token) const noexcept;

    // Restart reconstruction (journal): pre-seed token hashes consumed
    // BEFORE the restart so a replayed old ALLOW hits AlreadyConsumed
    // (C-12 across restarts). Control-thread use only.
    void seed_consumed(const TokenHash& token) noexcept;

    [[nodiscard]] const auth::SecretKey& secret() const noexcept
    {
        return m_secret;
    }

private:
    auth::SecretKey m_secret;
    std::unordered_set<TokenHash> m_consumed;
};
} // namespace qiven::runtime
