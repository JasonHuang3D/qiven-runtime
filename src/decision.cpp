#include <qiven/runtime/decision.hpp>

#include <qiven/byte_builder.hpp>
#include <qiven/contracts.hpp>
#include <qiven/hashing.hpp>
#include <qiven/hashing_sha256.hpp>
#include <qiven/memory/allocator.hpp>
#include <qiven/memory/system_allocator.hpp>

namespace qiven::runtime
{
namespace
{
// RR-0: the local shift-loop put mechanics are retired; preimage
// accumulation rides the foundation ByteBuilder and scalars ride the
// foundation codecs. Every width and field order below is a
// representation decision that stays with this format (byte-compat
// goldens pin the action digest; the golden fixtures bind the rest).
constexpr usize preimage_capacity_limit = 16 * 1024 * 1024;

[[nodiscard]] std::optional<qiven::ByteBuilder> make_preimage_builder() noexcept
{
    static qiven::memory::SystemAllocator allocator;
    return qiven::ByteBuilder::try_create(qiven::memory::AllocatorRef { allocator },
                                          preimage_capacity_limit);
}

void put_bytes(qiven::ByteBuilder& out, const std::string& text)
{
    // per-format representation: u64 length prefix + raw bytes
    (void)out.append_le_u64(static_cast<u64>(text.size()));
    (void)out.append({ reinterpret_cast<const std::byte*>(text.data()), text.size() });
}

void put_digest(qiven::ByteBuilder& out, const ContentDigest& digest)
{
    (void)out.append({ digest.sha256.data(), digest.sha256.size() });
}

void put_action(qiven::ByteBuilder& out, const ObservedAction& action)
{
    (void)out.append_le_u64(action.adapter.fnv);
    (void)out.append_le_u64(action.session.value);
    (void)out.append_le_u64(action.actor.value);
    (void)out.append_le_u64(action.capability.value);
    put_bytes(out, action.operation);
    put_bytes(out, action.target);
    put_digest(out, action.argument_digest);
}

void put_intents(qiven::ByteBuilder& out, const IntentSet& intents)
{
    const std::span<const IntentClassification> members = intents.members();
    (void)out.append_le_u32(static_cast<u32>(members.size()));
    for (const IntentClassification& member : members)
    {
        (void)out.append_le_u32(static_cast<u32>(member.basis));
        (void)out.append_le_u32(static_cast<u32>(member.intent.kind));
        (void)out.append_le_u32(static_cast<u32>(member.intent.claimClass));
        put_bytes(out, member.intent.tool);
        put_bytes(out, member.intent.operation);
        (void)out.append_le_u64(static_cast<u64>(member.intent.concepts.size()));
        for (const std::string& item : member.intent.concepts)
        {
            put_bytes(out, item);
        }
        (void)out.append_le_u64(static_cast<u64>(member.intent.files.size()));
        for (const std::string& file : member.intent.files)
        {
            put_bytes(out, file);
        }
        (void)out.append_le_u32(member.intent.priorFailure.has_value() ? 1U : 0U);
    }
}

void put_requirements(qiven::ByteBuilder& out, const qiven::context::PreparationPacket& packet)
{
    (void)out.append_le_u32(static_cast<u32>(packet.requirements.size()));
    for (const qiven::context::PreparedRequirement& prepared : packet.requirements)
    {
        (void)out.append_le_u32(static_cast<u32>(prepared.requirement.kind));
        put_bytes(out, prepared.requirement.subject);
        (void)out.append_le_u32(static_cast<u32>(prepared.boundary));
        (void)out.append_le_u32(prepared.requirement.blocking ? 1U : 0U);
        (void)out.append_le_u32(static_cast<u32>(prepared.status));
        (void)out.append_le_u64(static_cast<u64>(prepared.evidence.size()));
        for (const std::string& evidence : prepared.evidence)
        {
            put_bytes(out, evidence);
        }
    }
}

void put_evidence(qiven::ByteBuilder& out, const std::vector<resolver::EvidenceReceipt>& receipts)
{
    (void)out.append_le_u32(static_cast<u32>(receipts.size()));
    for (const resolver::EvidenceReceipt& receipt : receipts)
    {
        (void)out.append_le_u64(receipt.id.fnv);
        (void)out.append_le_u32(static_cast<u32>(receipt.requirement.kind));
        put_bytes(out, receipt.requirement.subject);
        (void)out.append_le_u32(static_cast<u32>(receipt.requirement.boundary));
        (void)out.append_le_u32(receipt.requirement.blocking ? 1U : 0U);
        (void)out.append_le_u64(static_cast<u64>(receipt.type));
        put_bytes(out, receipt.resolver.name);
        (void)out.append_le_u64(receipt.resolver.version);
        put_bytes(out, receipt.subject);
        put_bytes(out, receipt.source);
        put_bytes(out, receipt.source_revision.value);
        put_bytes(out, receipt.source_path);
        put_bytes(out, receipt.resolver_build);
        put_digest(out, receipt.content_digest);
        (void)out.append_le_u64(receipt.issued_at_ms);
        (void)out.append_le_u64(receipt.expires_at_ms);
        put_digest(out, receipt.policy_digest);
        (void)out.append_le_u64(receipt.generation.value);
        (void)out.append_le_u32(receipt.reusable_across_transactions ? 1U : 0U);
    }
}

ContentDigest digest_of(const qiven::ByteBuilder& preimage)
{
    const auto bytes = preimage.bytes();
    return ContentDigest { sha256(bytes.data(), bytes.size()) };
}

// The canonical authorization binding (production-MVP §4 invariant 8 /
// §10.3): generation + session + transaction + action + actor + profile +
// resource scope + operation + candidate digests + expiry + identity.
// Every field the token authorizes participates; changing any bound fact
// is a different authorization.
void put_binding(qiven::ByteBuilder& out, const ExecutionDecision& decision)
{
    (void)out.append_le_u32(1); // binding preimage version
    (void)out.append({ decision.id.bytes.data(), decision.id.bytes.size() });
    (void)out.append_le_u64(decision.transaction.value);
    (void)out.append_le_u64(decision.generation.value);
    (void)out.append_le_u64(decision.session.value);
    (void)out.append_le_u64(decision.actor.value);
    (void)out.append_le_u64(decision.capability.value);
    put_bytes(out, decision.operation);
    put_bytes(out, decision.target);
    put_digest(out, decision.resource_scope_digest);
    (void)out.append_le_u64(decision.expires_at_ms);
    put_digest(out, decision.action_digest);
    put_digest(out, decision.intent_set_digest);
    put_bytes(out, decision.cognition_revision.value);
    put_digest(out, decision.policy_digest);
    put_digest(out, decision.requirement_set_digest);
    put_digest(out, decision.evidence_set_digest);
    (void)out.append_le_u64(decision.profile.value);
    (void)out.append_le_u64(decision.resolver_registry_revision);
}
} // namespace

TokenHash token_hash_of(const DecisionToken& token)
{
    SHA256Hasher hasher;
    hasher.update(token.mac.data(), token.mac.size());
    hasher.update(token.nonce.data(), token.nonce.size());
    return TokenHash { hasher.finish() };
}

ContentDigest action_digest_of(const ObservedAction& action)
{
    auto preimage = make_preimage_builder();
    QIVEN_ASSERT(preimage.has_value());
    put_action(*preimage, action);
    QIVEN_ASSERT(preimage->ok());
    return digest_of(*preimage);
}

qiven::Result<ExecutionDecision> bind_allow(const ControlTransaction& transaction,
                                            const RuntimeGeneration& generation,
                                            const ContentDigest& resource_scope_digest,
                                            const u64 resolver_registry_revision,
                                            const std::vector<resolver::EvidenceReceipt>& evidence,
                                            const auth::SecretKey& secret,
                                            SortableIdMinter& id_minter,
                                            const u64 now_ms,
                                            const u64 ttl_ms)
{
    using qiven::Error;
    using qiven::error_category;

    if (transaction.phase != TransactionPhase::CognitiveAllowed)
    {
        return qiven::Result<ExecutionDecision>::fail(
            Error::make(error_category::invalid_argument, 30,
                        "an ALLOW decision binds only a transaction that reached CognitiveAllowed"));
    }

    ExecutionDecision decision;
    decision.id                    = id_minter.next();
    decision.transaction           = transaction.id;
    decision.generation            = generation.id;
    decision.session               = transaction.action.session;
    decision.actor                 = transaction.action.actor;
    decision.capability            = transaction.action.capability;
    decision.operation             = transaction.action.operation;
    decision.target                = transaction.action.target;
    decision.resource_scope_digest = resource_scope_digest;
    decision.expires_at_ms         = now_ms + ttl_ms;
    decision.action_digest         = action_digest_of(transaction.action);
    {
        auto preimage = make_preimage_builder();
        QIVEN_ASSERT(preimage.has_value());
        put_intents(*preimage, transaction.intents);
        QIVEN_ASSERT(preimage->ok());
        decision.intent_set_digest = digest_of(*preimage);
    }
    {
        auto preimage = make_preimage_builder();
        QIVEN_ASSERT(preimage.has_value());
        put_requirements(*preimage, transaction.packet);
        QIVEN_ASSERT(preimage->ok());
        decision.requirement_set_digest = digest_of(*preimage);
    }
    {
        auto preimage = make_preimage_builder();
        QIVEN_ASSERT(preimage.has_value());
        put_evidence(*preimage, evidence);
        QIVEN_ASSERT(preimage->ok());
        decision.evidence_set_digest = digest_of(*preimage);
    }
    decision.cognition_revision         = transaction.cognition.revision;
    decision.policy_digest              = transaction.cognition.policy_digest;
    decision.profile                    = generation.profile.revision;
    decision.resolver_registry_revision = resolver_registry_revision;
    decision.classifier_contract        = std::nullopt; // structural-only first landing
    decision.disposition                = Disposition::Allow;

    // the token binds EVERY fact above plus a fresh CSPRNG nonce: any
    // difference is a different single-use authorization, and two binds
    // of identical facts are still distinct authorizations (§40)
    decision.token.nonce = {};
    auth::csrandom_fill(decision.token.nonce);
    {
        auto preimage = make_preimage_builder();
        QIVEN_ASSERT(preimage.has_value());
        put_binding(*preimage, decision);
        (void)preimage->append({ decision.token.nonce.data(), decision.token.nonce.size() });
        QIVEN_ASSERT(preimage->ok());
        const auto bytes       = preimage->bytes();
        const SHA256Digest mac = auth::hmac_sha256(
            secret, { bytes.data(), bytes.size() });
        std::copy(mac.begin(), mac.end(), decision.token.mac.begin());
    }
    decision.token_hash = token_hash_of(decision.token);
    return qiven::Result<ExecutionDecision>(std::move(decision));
}

bool decision_binding_valid(const ExecutionDecision& decision, const auth::SecretKey& secret)
{
    auto preimage = make_preimage_builder();
    QIVEN_ASSERT(preimage.has_value());
    put_binding(*preimage, decision);
    (void)preimage->append({ decision.token.nonce.data(), decision.token.nonce.size() });
    QIVEN_ASSERT(preimage->ok());
    const auto bytes            = preimage->bytes();
    const SHA256Digest expected = auth::hmac_sha256(secret, { bytes.data(), bytes.size() });
    return auth::constant_time_equal(expected, decision.token.mac);
}

ConsumeResult DecisionLedger::consume(const ExecutionDecision& decision, const FreshnessFacts& facts, const u64 now_ms)
{
    // already-consumed is checked FIRST and is terminal: a spent token
    // never executes again regardless of freshness (C-12)
    if (m_consumed.contains(decision.token_hash))
    {
        return ConsumeResult { ConsumeOutcome::AlreadyConsumed };
    }

    // §10.3 fail-closed: a token whose binding does not verify is an
    // invalid authorization, never a freshness question
    if (!decision_binding_valid(decision, m_secret) || !(token_hash_of(decision.token) == decision.token_hash))
    {
        m_consumed.insert(decision.token_hash); // burn it: no retry of a forged token
        return ConsumeResult { ConsumeOutcome::InvalidToken };
    }
    if (decision.expires_at_ms != 0 && now_ms > decision.expires_at_ms)
    {
        m_consumed.insert(decision.token_hash);
        return ConsumeResult { ConsumeOutcome::Expired };
    }

    ConsumeOutcome outcome = ConsumeOutcome::Consumed;
    if (decision.action_digest != facts.action_digest)
    {
        outcome = ConsumeOutcome::StaleAction;
    }
    else if (decision.generation.value != facts.generation.value)
    {
        outcome = ConsumeOutcome::StaleGeneration;
    }
    else if (decision.cognition_revision.value != facts.cognition_revision.value)
    {
        outcome = ConsumeOutcome::StaleCognition;
    }
    else if (decision.profile.value != facts.profile.value)
    {
        outcome = ConsumeOutcome::StalePolicy;
    }
    else if (decision.resolver_registry_revision != facts.resolver_registry_revision)
    {
        outcome = ConsumeOutcome::StaleRegistry;
    }
    else if (facts.evidence_set_digest.has_value() && decision.evidence_set_digest != *facts.evidence_set_digest)
    {
        outcome = ConsumeOutcome::StaleEvidence;
    }
    else if (!(decision.resource_scope_digest == facts.resource_scope_digest))
    {
        outcome = ConsumeOutcome::StaleScope;
    }

    // §41: stale or not, the token is spent - the action re-enters
    // Cognitive Control as a NEW proposal and can never replay this ALLOW
    m_consumed.insert(decision.token_hash);
    return ConsumeResult { outcome };
}

bool DecisionLedger::was_consumed(const TokenHash& token) const noexcept
{
    return m_consumed.contains(token);
}

void DecisionLedger::seed_consumed(const TokenHash& token) noexcept
{
    m_consumed.insert(token);
}
} // namespace qiven::runtime
