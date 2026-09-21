#include <qiven/runtime/decision.hpp>

#include <qiven/hashing.hpp>
#include <qiven/hashing_sha256.hpp>

namespace qiven::runtime
{
namespace
{
// Canonical little-endian preimage writers (same representation law as the
// manifest identity): fixed widths, host-endian independent.
void put_u32(std::vector<std::byte>& out, u32 value)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<std::byte>(value >> (8 * i)));
    }
}

void put_u64(std::vector<std::byte>& out, u64 value)
{
    for (int i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<std::byte>(value >> (8 * i)));
    }
}

void put_bytes(std::vector<std::byte>& out, const std::string& text)
{
    put_u64(out, static_cast<u64>(text.size()));
    for (const char ch : text)
    {
        out.push_back(static_cast<std::byte>(ch));
    }
}

void put_digest(std::vector<std::byte>& out, const ContentDigest& digest)
{
    out.insert(out.end(), digest.sha256.begin(), digest.sha256.end());
}

void put_action(std::vector<std::byte>& out, const ObservedAction& action)
{
    put_u64(out, action.adapter.fnv);
    put_u64(out, action.session.value);
    put_u64(out, action.actor.value);
    put_u64(out, action.capability.value);
    put_bytes(out, action.operation);
    put_bytes(out, action.target);
    put_digest(out, action.argument_digest);
}

void put_intents(std::vector<std::byte>& out, const IntentSet& intents)
{
    const std::span<const IntentClassification> members = intents.members();
    put_u32(out, static_cast<u32>(members.size()));
    for (const IntentClassification& member : members)
    {
        put_u32(out, static_cast<u32>(member.basis));
        put_u32(out, static_cast<u32>(member.intent.kind));
        put_u32(out, static_cast<u32>(member.intent.claimClass));
        put_bytes(out, member.intent.tool);
        put_bytes(out, member.intent.operation);
        put_u64(out, static_cast<u64>(member.intent.concepts.size()));
        for (const std::string& item : member.intent.concepts)
        {
            put_bytes(out, item);
        }
        put_u64(out, static_cast<u64>(member.intent.files.size()));
        for (const std::string& file : member.intent.files)
        {
            put_bytes(out, file);
        }
        put_u32(out, member.intent.priorFailure.has_value() ? 1U : 0U);
    }
}

void put_requirements(std::vector<std::byte>& out, const qiven::context::PreparationPacket& packet)
{
    put_u32(out, static_cast<u32>(packet.requirements.size()));
    for (const qiven::context::PreparedRequirement& prepared : packet.requirements)
    {
        put_u32(out, static_cast<u32>(prepared.requirement.kind));
        put_bytes(out, prepared.requirement.subject);
        put_u32(out, static_cast<u32>(prepared.boundary));
        put_u32(out, prepared.requirement.blocking ? 1U : 0U);
        put_u32(out, static_cast<u32>(prepared.status));
        put_u64(out, static_cast<u64>(prepared.evidence.size()));
        for (const std::string& evidence : prepared.evidence)
        {
            put_bytes(out, evidence);
        }
    }
}

void put_evidence(std::vector<std::byte>& out, const std::vector<resolver::EvidenceReceipt>& receipts)
{
    put_u32(out, static_cast<u32>(receipts.size()));
    for (const resolver::EvidenceReceipt& receipt : receipts)
    {
        put_u64(out, receipt.id.fnv);
        put_u32(out, static_cast<u32>(receipt.requirement.kind));
        put_bytes(out, receipt.requirement.subject);
        put_u32(out, static_cast<u32>(receipt.requirement.boundary));
        put_u32(out, receipt.requirement.blocking ? 1U : 0U);
        put_bytes(out, receipt.resolver.name);
        put_u64(out, receipt.resolver.version);
        put_bytes(out, receipt.subject);
        put_bytes(out, receipt.source);
        put_digest(out, receipt.content_digest);
    }
}

ContentDigest digest_of(const std::vector<std::byte>& preimage)
{
    return ContentDigest { sha256(preimage.data(), preimage.size()) };
}
} // namespace

ContentDigest action_digest_of(const ObservedAction& action)
{
    std::vector<std::byte> preimage;
    preimage.reserve(128);
    put_action(preimage, action);
    return digest_of(preimage);
}

qiven::Result<ExecutionDecision> bind_allow(const ControlTransaction& transaction,
                                            const RuntimeGeneration& generation,
                                            u64 resolver_registry_revision,
                                            const std::vector<resolver::EvidenceReceipt>& evidence)
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
    decision.transaction   = transaction.id;
    decision.generation    = generation.id;
    decision.action_digest = action_digest_of(transaction.action);
    {
        std::vector<std::byte> preimage;
        put_intents(preimage, transaction.intents);
        decision.intent_set_digest = digest_of(preimage);
    }
    {
        std::vector<std::byte> preimage;
        put_requirements(preimage, transaction.packet);
        decision.requirement_set_digest = digest_of(preimage);
    }
    {
        std::vector<std::byte> preimage;
        put_evidence(preimage, evidence);
        decision.evidence_set_digest = digest_of(preimage);
    }
    decision.cognition_revision         = transaction.cognition.revision;
    decision.policy_digest              = transaction.cognition.policy_digest;
    decision.profile                    = generation.profile.revision;
    decision.resolver_registry_revision = resolver_registry_revision;
    decision.classifier_contract        = std::nullopt; // structural-only first landing
    decision.disposition                = Disposition::Allow;

    // the token binds EVERY fact above: any difference is a different
    // single-use authorization
    u64 token = fnv1a64_offset_basis;
    {
        std::vector<std::byte> preimage;
        put_u64(preimage, decision.transaction.value);
        put_u64(preimage, decision.generation.value);
        put_digest(preimage, decision.action_digest);
        put_digest(preimage, decision.intent_set_digest);
        put_bytes(preimage, decision.cognition_revision.value);
        put_digest(preimage, decision.policy_digest);
        put_digest(preimage, decision.requirement_set_digest);
        put_digest(preimage, decision.evidence_set_digest);
        put_u64(preimage, decision.profile.value);
        put_u64(preimage, decision.resolver_registry_revision);
        token = fnv1a64(preimage.data(), preimage.size());
    }
    decision.token = DecisionToken { token };
    return qiven::Result<ExecutionDecision>(std::move(decision));
}

ConsumeResult DecisionLedger::consume(const ExecutionDecision& decision, const FreshnessFacts& facts)
{
    // already-consumed is checked FIRST and is terminal: a spent token
    // never executes again regardless of freshness (C-12)
    if (m_consumed.contains(decision.token.fnv))
    {
        return ConsumeResult { ConsumeOutcome::AlreadyConsumed };
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

    // §41: stale or not, the token is spent - the action re-enters
    // Cognitive Control as a NEW proposal and can never replay this ALLOW
    m_consumed.insert(decision.token.fnv);
    return ConsumeResult { outcome };
}

bool DecisionLedger::was_consumed(u64 token_value) const noexcept
{
    return m_consumed.contains(token_value);
}
} // namespace qiven::runtime
