#include <qiven/runtime/resolver.hpp>

#include <qiven/hashing.hpp>
#include <qiven/hashing_sha256.hpp>

#include <algorithm>

namespace qiven::runtime::resolver
{
namespace
{
bool same_key(const ResolverKey& a, const ResolverKey& b) noexcept
{
    return a.kind == b.kind && a.type == b.type && a.version == b.version && a.cognition_view == b.cognition_view;
}

u64 receipt_id_of(const EvidenceReceipt& receipt)
{
    // fnv1a64 over the bound identity fields — deterministic identity for
    // deduplicated identical evidence (§26); NOT an authorization token
    // (production-MVP §10.3).
    u64 hash           = fnv1a64_offset_basis;
    const auto mix_u64 = [&hash](const u64 value) {
        for (int i = 0; i < 8; ++i)
        {
            hash ^= (value >> (8 * i)) & 0xFFU;
            hash *= fnv1a64_prime;
        }
    };
    const auto mix_text = [&hash, &mix_u64](const std::string& text) {
        mix_u64(text.size());
        for (const char ch : text)
        {
            hash ^= static_cast<u64>(static_cast<unsigned char>(ch));
            hash *= fnv1a64_prime;
        }
    };
    const auto mix_digest = [&hash](const ContentDigest& digest) {
        for (const std::byte b : digest.sha256)
        {
            hash ^= static_cast<u64>(b);
            hash *= fnv1a64_prime;
        }
    };
    mix_u64(static_cast<u64>(receipt.requirement.kind));
    mix_text(receipt.requirement.subject);
    mix_u64(static_cast<u64>(receipt.requirement.boundary));
    mix_u64(receipt.requirement.blocking ? 1U : 0U);
    mix_text(receipt.resolver.name);
    mix_u64(receipt.resolver.version);
    mix_u64(static_cast<u64>(receipt.type));
    mix_text(receipt.resolver_build);
    mix_text(receipt.source_revision.value);
    mix_text(receipt.source_path);
    mix_u64(static_cast<u64>(receipt.evidence_type));
    mix_text(receipt.subject);
    mix_text(receipt.source);
    mix_digest(receipt.content_digest);
    mix_u64(receipt.issued_at_ms);
    mix_u64(receipt.expires_at_ms);
    mix_digest(receipt.policy_digest);
    mix_u64(receipt.generation.value);
    mix_u64(receipt.reusable_across_transactions ? 1U : 0U);
    return hash;
}
} // namespace

const ResolverBinding* RequirementResolverRegistry::find(const ResolverKey& key) const noexcept
{
    for (const ResolverBinding& binding : m_bindings)
    {
        if (same_key(binding.key, key))
        {
            return &binding;
        }
    }
    return nullptr;
}

std::vector<const ResolverBinding*> RequirementResolverRegistry::find_for_kind_and_view(
    const qiven::context::RequirementKind kind, const std::string& cognition_view) const
{
    std::vector<const ResolverBinding*> found;
    for (const ResolverBinding& binding : m_bindings)
    {
        if (binding.key.kind == kind && binding.key.cognition_view == cognition_view)
        {
            found.push_back(&binding);
        }
    }
    return found;
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

    for (usize i = 0; i < m_pending.size(); ++i)
    {
        if (m_pending[i].resolver.name.empty() || m_pending[i].key.version == 0 ||
            m_pending[i].key.cognition_view.empty())
        {
            return qiven::Result<RequirementResolverRegistry>::fail(
                Error::make(error_category::invalid_argument, 20,
                            "resolver binding requires identity, version and cognition view"));
        }
        for (usize j = i + 1; j < m_pending.size(); ++j)
        {
            if (same_key(m_pending[i].key, m_pending[j].key))
            {
                return qiven::Result<RequirementResolverRegistry>::fail(
                    Error::make(error_category::invalid_argument, 21,
                                "duplicate resolver key: exactly one accepted mechanism per (kind, type, version, view)"));
            }
        }
    }

    // deterministic content revision of the accepted binding set
    u64 revision = fnv1a64_offset_basis;
    for (const ResolverBinding& binding : m_pending)
    {
        revision = fnv1a64(binding.resolver.name, revision);
        revision ^= binding.resolver.version;
        revision *= fnv1a64_prime;
        revision ^= static_cast<u64>(binding.key.kind);
        revision *= fnv1a64_prime;
        revision ^= static_cast<u64>(binding.key.type);
        revision *= fnv1a64_prime;
        revision = fnv1a64(binding.key.cognition_view, revision);
        revision ^= static_cast<u64>(binding.accepted_evidence);
        revision *= fnv1a64_prime;
    }

    std::vector<ResolverBinding> ordered = std::move(m_pending);
    std::sort(ordered.begin(), ordered.end(), [](const ResolverBinding& a, const ResolverBinding& b) {
        if (a.key.cognition_view != b.key.cognition_view)
        {
            return a.key.cognition_view < b.key.cognition_view;
        }
        return static_cast<u32>(a.key.kind) < static_cast<u32>(b.key.kind);
    });
    return qiven::Result<RequirementResolverRegistry>(
        RequirementResolverRegistry { std::move(ordered), revision });
}

EvidenceReceipt make_evidence_receipt(const RequirementIdentity& requirement,
                                      const ResolverIdentity& resolver,
                                      const ResolverType type,
                                      const EvidenceType evidence_type,
                                      std::string subject,
                                      std::string source,
                                      const std::span<const std::byte> result_content,
                                      const ReceiptContext& context)
{
    EvidenceReceipt receipt;
    receipt.requirement     = requirement;
    receipt.resolver        = resolver;
    receipt.type            = type;
    receipt.evidence_type   = evidence_type;
    receipt.subject         = std::move(subject);
    receipt.source          = std::move(source);
    receipt.content_digest  = ContentDigest { sha256(result_content.data(), result_content.size()) };
    receipt.issued_at_ms    = context.now_ms;
    receipt.expires_at_ms   = 0; // no expiry claimed unless the mechanism sets one
    receipt.policy_digest   = context.policy_digest;
    receipt.generation      = context.generation;
    receipt.source_revision = context.source_revision;
    receipt.resolver_build  = context.resolver_build;
    receipt.id.fnv          = receipt_id_of(receipt);
    return receipt;
}

ReceiptValidation validate_receipt(const EvidenceReceipt& receipt,
                                   const ResolverBinding& binding,
                                   const RequirementIdentity& requirement,
                                   const RuntimeGenerationId& generation,
                                   const ContentDigest& policy_digest,
                                   const u64 now_ms) noexcept
{
    ReceiptValidation out;

    if (!(receipt.requirement == requirement))
    {
        out.rejection = ReceiptRejection::WrongRequirement;
        return out;
    }
    if (receipt.type != binding.key.type)
    {
        out.rejection = ReceiptRejection::WrongResolverType;
        return out;
    }
    if (receipt.resolver.version != binding.key.version || !(receipt.resolver == binding.resolver))
    {
        out.rejection = ReceiptRejection::WrongVersion;
        return out;
    }
    if (receipt.evidence_type != binding.accepted_evidence)
    {
        out.rejection = ReceiptRejection::WrongEvidenceType;
        return out;
    }
    if (receipt.generation.value != generation.value)
    {
        out.rejection = ReceiptRejection::WrongGeneration;
        return out;
    }
    if (!(receipt.policy_digest == policy_digest))
    {
        out.rejection = ReceiptRejection::WrongPolicy;
        return out;
    }
    if (receipt.expires_at_ms != 0 && now_ms != 0 && now_ms > receipt.expires_at_ms)
    {
        out.rejection = ReceiptRejection::Expired;
        return out;
    }
    bool any_digest_byte = false;
    for (const std::byte b : receipt.content_digest.sha256)
    {
        any_digest_byte = any_digest_byte || b != std::byte { 0 };
    }
    if (!any_digest_byte)
    {
        out.rejection = ReceiptRejection::BadDigest;
        return out;
    }
    if (receipt.source_revision.value.empty() || receipt.subject.empty())
    {
        out.rejection = ReceiptRejection::BadSource;
        return out;
    }
    return out;
}
} // namespace qiven::runtime::resolver
