#include <qiven/runtime/resolver.hpp>

#include <qiven/hashing.hpp>
#include <qiven/hashing_sha256.hpp>

namespace qiven::runtime::resolver
{
const ResolverBinding* RequirementResolverRegistry::find(qiven::context::RequirementKind kind) const noexcept
{
    for (const ResolverBinding& binding : m_bindings)
    {
        if (binding.kind == kind)
        {
            return &binding;
        }
    }
    return nullptr;
}

RequirementResolverRegistry::Builder& RequirementResolverRegistry::Builder::add(ResolverBinding binding)
{
    m_pending.push_back(std::move(binding));
    return *this;
}

qiven::Result<RequirementResolverRegistry> RequirementResolverRegistry::Builder::build()
{
    using qiven::Error;
    using qiven::error_category;

    u64 revision = fnv1a64_offset_basis;
    for (const ResolverBinding& binding : m_pending)
    {
        if (binding.resolver.name.empty() || binding.resolver.version == 0)
        {
            return qiven::Result<RequirementResolverRegistry>::fail(
                Error::make(error_category::invalid_argument, 20, "resolver binding requires identity and version"));
        }
        for (const ResolverBinding& other : m_pending)
        {
            if (&other != &binding && other.kind == binding.kind)
            {
                return qiven::Result<RequirementResolverRegistry>::fail(
                    Error::make(error_category::invalid_argument, 21,
                                "duplicate resolver binding for one requirement kind"));
            }
        }
        // deterministic content revision of the accepted binding set
        revision = fnv1a64(binding.resolver.name, revision);
        revision ^= binding.resolver.version;
        revision *= fnv1a64_prime;
        revision ^= static_cast<u64>(binding.kind);
        revision *= fnv1a64_prime;
        revision ^= static_cast<u64>(binding.accepted_evidence);
        revision *= fnv1a64_prime;
    }
    return qiven::Result<RequirementResolverRegistry>(
        RequirementResolverRegistry { std::move(m_pending), revision });
}

EvidenceReceipt make_evidence_receipt(const RequirementIdentity& requirement,
                                      const ResolverIdentity& resolver,
                                      std::string subject,
                                      std::string source,
                                      std::span<const std::byte> result_content)
{
    EvidenceReceipt receipt;
    receipt.requirement    = requirement;
    receipt.resolver       = resolver;
    receipt.subject        = std::move(subject);
    receipt.source         = std::move(source);
    receipt.content_digest = ContentDigest { sha256(result_content.data(), result_content.size()) };

    u64 id = fnv1a64_offset_basis;
    id     = fnv1a64(receipt.subject, id);
    id     = fnv1a64(receipt.source, id);
    id     = fnv1a64(receipt.resolver.name, id);
    id ^= receipt.resolver.version;
    id *= fnv1a64_prime;
    id ^= static_cast<u64>(receipt.requirement.kind);
    id *= fnv1a64_prime;
    id = fnv1a64(receipt.requirement.subject, id);
    id ^= static_cast<u64>(receipt.requirement.boundary);
    id *= fnv1a64_prime;
    id ^= receipt.requirement.blocking ? 0x01 : 0x00;
    id *= fnv1a64_prime;
    // the content digest participates through its stable hex form
    id         = fnv1a64(to_hex(receipt.content_digest), id);
    receipt.id = EvidenceId { id };
    return receipt;
}
} // namespace qiven::runtime::resolver
