#include <qiven/runtime/resolver.hpp>

#include <qiven/contracts.hpp>
#include <qiven/hashing_sha256.hpp>

#include <cstdio>
#include <utility>

namespace
{
using qiven::context::RequirementKind;
using qiven::runtime::RequirementIdentity;
using qiven::runtime::resolver::EvidenceReceipt;
using qiven::runtime::resolver::EvidenceType;
using qiven::runtime::resolver::RequirementResolverRegistry;
using qiven::runtime::resolver::ResolverBinding;
using qiven::runtime::resolver::ResolverIdentity;
using qiven::runtime::resolver::TrustDomain;

ResolverBinding search_binding()
{
    ResolverBinding binding;
    binding.kind              = RequirementKind::SearchLowerLayer;
    binding.resolver          = ResolverIdentity { "loopback-lower-search", 1 };
    binding.accepted_evidence = EvidenceType::AttributableSearch;
    binding.trust             = TrustDomain::Mechanism;
    return binding;
}

RequirementIdentity search_identity(std::string subject)
{
    RequirementIdentity identity;
    identity.kind    = RequirementKind::SearchLowerLayer;
    identity.subject = std::move(subject);
    return identity;
}
} // namespace

int main()
{
    // §28: one accepted mechanism per kind; lookups resolve exactly
    {
        ResolverBinding canonical;
        canonical.kind              = RequirementKind::VerifyCanonical;
        canonical.resolver          = ResolverIdentity { "loopback-canonical", 1 };
        canonical.accepted_evidence = EvidenceType::CanonicalRecord;
        canonical.trust             = TrustDomain::Canonical;

        auto built = RequirementResolverRegistry::Builder {}
                         .add(search_binding())
                         .add(std::move(canonical))
                         .build();
        QIVEN_VERIFY(built.is_ok());
        const auto& registry = built.value();

        const ResolverBinding* search = registry.find(RequirementKind::SearchLowerLayer);
        QIVEN_VERIFY(search != nullptr);
        QIVEN_VERIFY(search->resolver.name == "loopback-lower-search");
        QIVEN_VERIFY(search->accepted_evidence == EvidenceType::AttributableSearch);

        const ResolverBinding* verify = registry.find(RequirementKind::VerifyCanonical);
        QIVEN_VERIFY(verify != nullptr);
        QIVEN_VERIFY(verify->trust == TrustDomain::Canonical);
    }

    // §28 validation: duplicate kind bindings are rejected (exactly one
    // accepted mechanism per kind per generation)
    {
        auto built = RequirementResolverRegistry::Builder {}
                         .add(search_binding())
                         .add(search_binding())
                         .build();
        QIVEN_VERIFY(!built.is_ok());
        QIVEN_VERIFY(built.reason().category == qiven::error_category::invalid_argument);
        QIVEN_VERIFY(built.reason().code == 21);
    }

    // §28 validation: bindings require a real identity and version
    {
        ResolverBinding anonymous;
        anonymous.kind     = RequirementKind::MandatoryRecall;
        anonymous.resolver = ResolverIdentity { "", 0 };

        auto built = RequirementResolverRegistry::Builder {}.add(std::move(anonymous)).build();
        QIVEN_VERIFY(!built.is_ok());
        QIVEN_VERIFY(built.reason().code == 20);
    }

    // §29 fail-closed: no accepted binding => nullptr, never a fallback;
    // the CALLER keeps the requirement unsatisfied
    {
        auto built = RequirementResolverRegistry::Builder {}.add(search_binding()).build();
        QIVEN_VERIFY(built.is_ok());
        QIVEN_VERIFY(built.value().find(RequirementKind::AskHuman) == nullptr);
        QIVEN_VERIFY(built.value().find(RequirementKind::RunMechanicalCheck) == nullptr);
    }

    // registry revision: deterministic over the accepted binding content —
    // the same set rebuilds the same revision (generation binding is stable)
    {
        auto first  = RequirementResolverRegistry::Builder {}.add(search_binding()).build();
        auto second = RequirementResolverRegistry::Builder {}.add(search_binding()).build();
        QIVEN_VERIFY(first.is_ok() && second.is_ok());
        QIVEN_VERIFY(first.value().revision() == second.value().revision());

        ResolverBinding extra;
        extra.kind     = RequirementKind::VerifyLive;
        extra.resolver = ResolverIdentity { "loopback-live", 1 };
        auto grown     = RequirementResolverRegistry::Builder {}.add(search_binding()).add(extra).build();
        QIVEN_VERIFY(grown.is_ok());
        QIVEN_VERIFY(grown.value().revision() != first.value().revision());
    }

    // §33 receipts: identical bound fields + identical content => identical
    // id and digest — identical physical operations share evidence (§26)
    {
        const std::byte content[] { std::byte { 'o' }, std::byte { 'k' } };

        const EvidenceReceipt left = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 }, "hashing", "qiven-foundation",
            std::span<const std::byte>(content));
        const EvidenceReceipt right = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 }, "hashing", "qiven-foundation",
            std::span<const std::byte>(content));

        QIVEN_VERIFY(left == right);
        QIVEN_VERIFY(left.id.fnv == right.id.fnv);
        QIVEN_VERIFY(left.content_digest == right.content_digest);
        QIVEN_VERIFY(left.content_digest.sha256 == qiven::sha256(content, sizeof content));
    }

    // §27 prevention: a different requirement subject produces a different
    // id — a hashing receipt cannot satisfy a serialization requirement
    {
        const std::byte content[] { std::byte { 'o' }, std::byte { 'k' } };

        const EvidenceReceipt hashing = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 }, "hashing", "qiven-foundation",
            std::span<const std::byte>(content));
        const EvidenceReceipt serialization = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("serialization"), ResolverIdentity { "loopback-lower-search", 1 }, "serialization",
            "qiven-foundation", std::span<const std::byte>(content));

        QIVEN_VERIFY(hashing.id.fnv != serialization.id.fnv);
        QIVEN_VERIFY(hashing.requirement != serialization.requirement);
    }

    // different content => different digest and id; different resolver =>
    // different id (trusted satisfaction binds the mechanism identity)
    {
        const std::byte ok[] { std::byte { 'o' }, std::byte { 'k' } };
        const std::byte different[] { std::byte { 'o' }, std::byte { 'k' }, std::byte { '!' } };

        const EvidenceReceipt base = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 }, "hashing", "qiven-foundation",
            std::span<const std::byte>(ok));
        const EvidenceReceipt changed_content = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 }, "hashing", "qiven-foundation",
            std::span<const std::byte>(different));
        const EvidenceReceipt changed_resolver = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "other-search", 2 }, "hashing", "qiven-foundation",
            std::span<const std::byte>(ok));

        QIVEN_VERIFY(base.content_digest != changed_content.content_digest);
        QIVEN_VERIFY(base.id.fnv != changed_content.id.fnv);
        QIVEN_VERIFY(base.id.fnv != changed_resolver.id.fnv);
    }

    std::printf("[ OK ] resolver-registry\n");
    return 0;
}
