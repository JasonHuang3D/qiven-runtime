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
using qiven::runtime::resolver::ReceiptContext;
using qiven::runtime::resolver::RequirementResolverRegistry;
using qiven::runtime::resolver::ResolverBinding;
using qiven::runtime::resolver::ResolverIdentity;
using qiven::runtime::resolver::ResolverKey;
using qiven::runtime::resolver::ResolverType;
using qiven::runtime::resolver::TrustDomain;

ResolverBinding search_binding()
{
    ResolverBinding binding;
    binding.key.kind           = RequirementKind::SearchLowerLayer;
    binding.key.type           = ResolverType::AttributableSearch;
    binding.key.version        = 1;
    binding.key.cognition_view = "default";
    binding.resolver           = ResolverIdentity { "loopback-lower-search", 1 };
    binding.accepted_evidence  = EvidenceType::AttributableSearch;
    binding.trust              = TrustDomain::Mechanism;
    return binding;
}

RequirementIdentity search_identity(std::string subject)
{
    RequirementIdentity identity;
    identity.kind    = RequirementKind::SearchLowerLayer;
    identity.subject = std::move(subject);
    return identity;
}

ReceiptContext sample_context()
{
    return ReceiptContext { qiven::runtime::RuntimeGenerationId { 4 },
                            qiven::runtime::ContentDigest { qiven::SHA256Digest {} },
                            qiven::context::RevisionId { "git:test-commit" },
                            "test-build",
                            1'000'000 };
}
} // namespace

int main()
{
    // §28/§8.3: selection by the full four-tuple; lookups resolve exactly
    {
        ResolverBinding canonical;
        canonical.key.kind           = RequirementKind::VerifyCanonical;
        canonical.key.type           = ResolverType::CanonicalRecall;
        canonical.key.version        = 1;
        canonical.key.cognition_view = "default";
        canonical.resolver           = ResolverIdentity { "loopback-canonical", 1 };
        canonical.accepted_evidence  = EvidenceType::CanonicalRecord;
        canonical.trust              = TrustDomain::Canonical;

        auto built = RequirementResolverRegistry::Builder {}
                         .add(search_binding())
                         .add(std::move(canonical))
                         .build();
        QIVEN_VERIFY(built.is_ok());
        const auto& registry = built.value();

        const ResolverBinding* search = registry.find(
            ResolverKey { RequirementKind::SearchLowerLayer, ResolverType::AttributableSearch, 1, "default" });
        QIVEN_VERIFY(search != nullptr);
        QIVEN_VERIFY(search->resolver.name == "loopback-lower-search");
        QIVEN_VERIFY(search->accepted_evidence == EvidenceType::AttributableSearch);

        const ResolverBinding* verify = registry.find(
            ResolverKey { RequirementKind::VerifyCanonical, ResolverType::CanonicalRecall, 1, "default" });
        QIVEN_VERIFY(verify != nullptr);
        QIVEN_VERIFY(verify->trust == TrustDomain::Canonical);

        // a different type, version or cognition view does NOT resolve:
        // the exact key is required, there is no partial fallback
        QIVEN_VERIFY(registry.find(
                         ResolverKey { RequirementKind::SearchLowerLayer, ResolverType::CanonicalRecall, 1, "default" }) ==
                     nullptr);
        QIVEN_VERIFY(registry.find(
                         ResolverKey { RequirementKind::SearchLowerLayer, ResolverType::AttributableSearch, 2, "default" }) ==
                     nullptr);
        QIVEN_VERIFY(registry.find(
                         ResolverKey { RequirementKind::SearchLowerLayer, ResolverType::AttributableSearch, 1, "production" }) ==
                     nullptr);

        // kind+view resolution drives dispatch: exactly one binding
        const auto dispatchable = registry.find_for_kind_and_view(RequirementKind::SearchLowerLayer, "default");
        QIVEN_VERIFY(dispatchable.size() == 1 && dispatchable.front()->resolver.name == "loopback-lower-search");
        QIVEN_VERIFY(registry.find_for_kind_and_view(RequirementKind::AskHuman, "default").empty());
    }

    // §28 validation: duplicate four-tuple keys are rejected (exactly one
    // accepted mechanism per key per generation)
    {
        auto built = RequirementResolverRegistry::Builder {}
                         .add(search_binding())
                         .add(search_binding())
                         .build();
        QIVEN_VERIFY(!built.is_ok());
        QIVEN_VERIFY(built.reason().category == qiven::error_category::invalid_argument);
        QIVEN_VERIFY(built.reason().code == 21);
    }

    // the same kind under different views is NOT a duplicate — the view
    // is a selection dimension
    {
        ResolverBinding production    = search_binding();
        production.key.cognition_view = "production";

        auto built = RequirementResolverRegistry::Builder {}.add(search_binding()).add(production).build();
        QIVEN_VERIFY(built.is_ok());
    }

    // §28 validation: bindings require a real identity, version and view
    {
        ResolverBinding anonymous;
        anonymous.key.kind = RequirementKind::MandatoryRecall;
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
        QIVEN_VERIFY(built.value()
                         .find(ResolverKey { RequirementKind::AskHuman, ResolverType::TypedHumanHandoff, 1, "default" }) ==
                     nullptr);
        QIVEN_VERIFY(built.value()
                         .find(ResolverKey { RequirementKind::RunMechanicalCheck, ResolverType::MechanicalCheck, 1,
                                             "default" }) == nullptr);
    }

    // registry revision: deterministic over the accepted binding content —
    // the same set rebuilds the same revision (generation binding is stable)
    {
        auto first  = RequirementResolverRegistry::Builder {}.add(search_binding()).build();
        auto second = RequirementResolverRegistry::Builder {}.add(search_binding()).build();
        QIVEN_VERIFY(first.is_ok() && second.is_ok());
        QIVEN_VERIFY(first.value().revision() == second.value().revision());

        ResolverBinding extra;
        extra.key.kind           = RequirementKind::VerifyLive;
        extra.key.type           = ResolverType::LiveObservation;
        extra.key.version        = 1;
        extra.key.cognition_view = "default";
        extra.resolver           = ResolverIdentity { "loopback-live", 1 };
        auto grown               = RequirementResolverRegistry::Builder {}.add(search_binding()).add(extra).build();
        QIVEN_VERIFY(grown.is_ok());
        QIVEN_VERIFY(grown.value().revision() != first.value().revision());
    }

    // §33 receipts: identical bound fields + identical content => identical
    // id and digest — identical physical operations share evidence (§26)
    {
        const std::byte content[] { std::byte { 'o' }, std::byte { 'k' } };

        const EvidenceReceipt left = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 },
            ResolverType::AttributableSearch, EvidenceType::AttributableSearch, "hashing", "qiven-foundation",
            std::span<const std::byte>(content), sample_context());
        const EvidenceReceipt right = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 },
            ResolverType::AttributableSearch, EvidenceType::AttributableSearch, "hashing", "qiven-foundation",
            std::span<const std::byte>(content), sample_context());

        QIVEN_VERIFY(left == right);
        QIVEN_VERIFY(left.id.fnv == right.id.fnv);
        QIVEN_VERIFY(left.content_digest == right.content_digest);
        QIVEN_VERIFY(left.content_digest.sha256 == qiven::sha256(content, sizeof content));

        // the receipt carries the §8.3 completeness fields
        QIVEN_VERIFY(left.generation.value == 4);
        QIVEN_VERIFY(left.source_revision.value == "git:test-commit");
        QIVEN_VERIFY(left.resolver_build == "test-build");
        QIVEN_VERIFY(left.issued_at_ms == 1'000'000);
        QIVEN_VERIFY(!left.reusable_across_transactions);
    }

    // §8.3 acceptance: a receipt mismatch in type, version, source,
    // digest, generation, evidence type or expiry is REJECTED (MVP-0
    // exit-gate requirement)
    {
        const std::byte content[] { std::byte { 'o' }, std::byte { 'k' } };
        const ResolverBinding binding      = search_binding();
        const RequirementIdentity identity = search_identity("hashing");

        const EvidenceReceipt good = qiven::runtime::resolver::make_evidence_receipt(
            identity, binding.resolver, ResolverType::AttributableSearch, EvidenceType::AttributableSearch, "hashing",
            "qiven-foundation", std::span<const std::byte>(content), sample_context());
        QIVEN_VERIFY(qiven::runtime::resolver::validate_receipt(good, binding, identity,
                                                                qiven::runtime::RuntimeGenerationId { 4 },
                                                                sample_context().policy_digest, 1'000'001)
                         .rejection == qiven::runtime::resolver::ReceiptRejection::None);

        // wrong requirement
        {
            EvidenceReceipt bad = good;
            bad.requirement     = search_identity("serialization");
            QIVEN_VERIFY(qiven::runtime::resolver::validate_receipt(bad, binding, identity,
                                                                    qiven::runtime::RuntimeGenerationId { 4 },
                                                                    sample_context().policy_digest, 1'000'001)
                             .rejection == qiven::runtime::resolver::ReceiptRejection::WrongRequirement);
        }
        // wrong resolver type
        {
            EvidenceReceipt bad = good;
            bad.type            = ResolverType::CanonicalRecall;
            QIVEN_VERIFY(qiven::runtime::resolver::validate_receipt(bad, binding, identity,
                                                                    qiven::runtime::RuntimeGenerationId { 4 },
                                                                    sample_context().policy_digest, 1'000'001)
                             .rejection == qiven::runtime::resolver::ReceiptRejection::WrongResolverType);
        }
        // wrong version
        {
            EvidenceReceipt bad  = good;
            bad.resolver.version = 2;
            QIVEN_VERIFY(qiven::runtime::resolver::validate_receipt(bad, binding, identity,
                                                                    qiven::runtime::RuntimeGenerationId { 4 },
                                                                    sample_context().policy_digest, 1'000'001)
                             .rejection == qiven::runtime::resolver::ReceiptRejection::WrongVersion);
        }
        // wrong evidence type
        {
            EvidenceReceipt bad = good;
            bad.evidence_type   = EvidenceType::LiveObservation;
            QIVEN_VERIFY(qiven::runtime::resolver::validate_receipt(bad, binding, identity,
                                                                    qiven::runtime::RuntimeGenerationId { 4 },
                                                                    sample_context().policy_digest, 1'000'001)
                             .rejection == qiven::runtime::resolver::ReceiptRejection::WrongEvidenceType);
        }
        // wrong generation
        {
            QIVEN_VERIFY(qiven::runtime::resolver::validate_receipt(good, binding, identity,
                                                                    qiven::runtime::RuntimeGenerationId { 5 },
                                                                    sample_context().policy_digest, 1'000'001)
                             .rejection == qiven::runtime::resolver::ReceiptRejection::WrongGeneration);
        }
        // expired
        {
            EvidenceReceipt bad = good;
            bad.expires_at_ms   = 1'000'000;
            QIVEN_VERIFY(qiven::runtime::resolver::validate_receipt(bad, binding, identity,
                                                                    qiven::runtime::RuntimeGenerationId { 4 },
                                                                    sample_context().policy_digest, 1'000'001)
                             .rejection == qiven::runtime::resolver::ReceiptRejection::Expired);
        }
        // zero digest
        {
            EvidenceReceipt bad = good;
            bad.content_digest  = qiven::runtime::ContentDigest { qiven::SHA256Digest {} };
            QIVEN_VERIFY(qiven::runtime::resolver::validate_receipt(bad, binding, identity,
                                                                    qiven::runtime::RuntimeGenerationId { 4 },
                                                                    sample_context().policy_digest, 1'000'001)
                             .rejection == qiven::runtime::resolver::ReceiptRejection::BadDigest);
        }
        // missing source revision
        {
            EvidenceReceipt bad = good;
            bad.source_revision = qiven::context::RevisionId {};
            QIVEN_VERIFY(qiven::runtime::resolver::validate_receipt(bad, binding, identity,
                                                                    qiven::runtime::RuntimeGenerationId { 4 },
                                                                    sample_context().policy_digest, 1'000'001)
                             .rejection == qiven::runtime::resolver::ReceiptRejection::BadSource);
        }
    }

    // §27 prevention: a different requirement subject produces a different
    // id — a hashing receipt cannot satisfy a serialization requirement
    {
        const std::byte content[] { std::byte { 'o' }, std::byte { 'k' } };

        const EvidenceReceipt hashing = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 },
            ResolverType::AttributableSearch, EvidenceType::AttributableSearch, "hashing", "qiven-foundation",
            std::span<const std::byte>(content), sample_context());
        const EvidenceReceipt serialization = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("serialization"), ResolverIdentity { "loopback-lower-search", 1 },
            ResolverType::AttributableSearch, EvidenceType::AttributableSearch, "serialization", "qiven-foundation",
            std::span<const std::byte>(content), sample_context());

        QIVEN_VERIFY(hashing.id.fnv != serialization.id.fnv);
        QIVEN_VERIFY(hashing.requirement != serialization.requirement);
    }

    // different content => different digest and id; different resolver =>
    // different id (trusted satisfaction binds the mechanism identity)
    {
        const std::byte ok[] { std::byte { 'o' }, std::byte { 'k' } };
        const std::byte different[] { std::byte { 'o' }, std::byte { 'k' }, std::byte { '!' } };

        const EvidenceReceipt base = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 },
            ResolverType::AttributableSearch, EvidenceType::AttributableSearch, "hashing", "qiven-foundation",
            std::span<const std::byte>(ok), sample_context());
        const EvidenceReceipt changed_content = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "loopback-lower-search", 1 },
            ResolverType::AttributableSearch, EvidenceType::AttributableSearch, "hashing", "qiven-foundation",
            std::span<const std::byte>(different), sample_context());
        const EvidenceReceipt changed_resolver = qiven::runtime::resolver::make_evidence_receipt(
            search_identity("hashing"), ResolverIdentity { "other-search", 2 }, ResolverType::AttributableSearch,
            EvidenceType::AttributableSearch, "hashing", "qiven-foundation", std::span<const std::byte>(ok),
            sample_context());

        QIVEN_VERIFY(base.content_digest != changed_content.content_digest);
        QIVEN_VERIFY(base.id.fnv != changed_content.id.fnv);
        QIVEN_VERIFY(base.id.fnv != changed_resolver.id.fnv);
    }

    std::printf("[ OK ] resolver-registry\n");
    return 0;
}
