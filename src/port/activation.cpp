#include <qiven/runtime/port/activation.hpp>

#include <qiven/hashing.hpp>

namespace qiven::runtime::port
{
namespace
{
resolver::EvidenceType evidence_type_for(const RequirementIdentity&)
{
    // Day-one mapping: a BeforeJudgment cognition requirement is
    // satisfied by canonical-record evidence; richer per-kind mapping
    // arrives with the invocation-policy instance (MVP-2).
    return resolver::EvidenceType::CanonicalRecord;
}
} // namespace

bool ActivationReceipt::valid_for(const RequirementIdentity& requirement,
                                  const qiven::context::RevisionId& pinned_revision,
                                  const ContentDigest& pinned_digest,
                                  const RuntimeGenerationId& live_generation,
                                  const ContentDigest& /*live_policy_digest*/,
                                  const u64 now_ms) const noexcept
{
    if (!complete())
    {
        return false;
    }
    if (subject != requirement.subject)
    {
        return false;
    }
    if (evidence_type != evidence_type_for(requirement))
    {
        return false;
    }
    if (canonical_revision.value != pinned_revision.value)
    {
        return false;
    }
    if (!(injected_digest == pinned_digest))
    {
        return false;
    }
    if (generation.value != live_generation.value)
    {
        return false;
    }
    if (expires_at_ms != 0 && now_ms != 0 && now_ms > expires_at_ms)
    {
        return false;
    }
    return true;
}

u64 ActivationLedger::notify_activation(const ActivationEvent& event)
{
    static_cast<void>(event);
    const u64 identity = m_next_event++;
    m_recorded_events.insert(identity);
    return identity;
}

std::optional<ActivationReceipt> ActivationLedger::mint_receipt(const ActivationReceipt& requested) const
{
    if (!requested.complete() || !m_recorded_events.contains(requested.injection_event))
    {
        return std::nullopt;
    }
    return requested;
}

u64 activation_use_identity(const ActivationReceipt& receipt) noexcept
{
    u64 hash = fnv1a64_offset_basis;
    hash     = fnv1a64(receipt.subject, hash);
    hash ^= receipt.generation.value;
    hash *= fnv1a64_prime;
    hash ^= receipt.injection_event;
    hash *= fnv1a64_prime;
    return hash;
}

bool ActivationLedger::try_spend(const ActivationReceipt& receipt)
{
    const u64 use = activation_use_identity(receipt);
    if (m_spent.contains(use))
    {
        return false;
    }
    m_spent.insert(use);
    return true;
}

bool ActivationLedger::is_spent(const ActivationReceipt& receipt) const noexcept
{
    return m_spent.contains(activation_use_identity(receipt));
}
} // namespace qiven::runtime::port
