#pragma once

// ============================================================================
// resolver.hpp — accepted resolver bindings and typed evidence receipts
// (component ADL §28/§29/§33; obligations C-05/C-06)
//
// The registry answers: which ACCEPTED mechanism may satisfy this
// RequirementKind in this generation. No binding => the requirement stays
// unsatisfied (fail-closed, §29 — never "skip requirement"); there is no
// silent fallback to an arbitrary available mechanism. The registry is
// immutable once built; changing a binding is a new generation (§28).
//
// A resolver never returns a naked bool for mandatory satisfaction: it
// produces an immutable EvidenceReceipt binding the requirement identity,
// the resolver identity and a content digest over the evidence bytes
// (§33). The receipt is runtime evidence, not automatically canonical
// evidence.
//
// Day-one scope note (recorded, not silent): DeploymentProfile
// applicability per binding arrives with multi-profile conformance
// (RCA-16); the first landing has a single accepted profile.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/identity.hpp>
#include <qiven/runtime/requirement.hpp>

#include <qiven/context/cognition.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace qiven::runtime::resolver
{
enum class EvidenceType : u8
{
    AttributableSearch,     // LowerLayerSearchPort-shaped evidence (ADL §35)
    CanonicalRecord,        // CanonicalCognitionPort-shaped recall/verify
    LiveObservation,        // LiveFactPort-shaped verification
    ToolContractDocument,   // ToolContractPort-shaped contract (ADL §36)
    MechanicalCheckReceipt, // MechanicalCheckPort: exit code + bound artifact
    TypedHumanHandoff,      // HumanHandoffPort: H1-H4 typed evidence (ADL §38)
};

enum class TrustDomain : u8
{
    Mechanism, // machine-verifiable output of an accepted mechanism port
    Canonical, // the canonical cognition store itself
    Human,     // a typed human handoff (H1-H4)
};

struct ResolverIdentity
{
    std::string name;
    u64 version = 0;

    [[nodiscard]] bool operator==(const ResolverIdentity& other) const noexcept
    {
        return name == other.name && version == other.version;
    }
};

struct ResolverBinding
{
    qiven::context::RequirementKind kind = qiven::context::RequirementKind::MandatoryRecall;
    ResolverIdentity resolver;
    EvidenceType accepted_evidence = EvidenceType::AttributableSearch;
    TrustDomain trust              = TrustDomain::Mechanism;
};

class RequirementResolverRegistry
{
public:
    RequirementResolverRegistry() = default;

    // nullptr means NO accepted binding: the requirement remains
    // unsatisfied (fail-closed). There is no fallback lookup.
    [[nodiscard]] const ResolverBinding* find(qiven::context::RequirementKind kind) const noexcept;

    [[nodiscard]] u64 revision() const noexcept
    {
        return m_revision;
    }

    [[nodiscard]] std::span<const ResolverBinding> bindings() const& noexcept
    {
        return m_bindings;
    }

    class Builder;

private:
    RequirementResolverRegistry(std::vector<ResolverBinding> bindings, u64 revision) :
    m_bindings(std::move(bindings)), m_revision(revision)
    {
    }

    std::vector<ResolverBinding> m_bindings;
    u64 m_revision = 0;
};

// The ONLY construction path. Duplicate bindings for one RequirementKind
// are rejected (exactly one accepted mechanism per kind per generation).
class RequirementResolverRegistry::Builder
{
public:
    Builder& add(ResolverBinding binding);
    [[nodiscard]] qiven::Result<RequirementResolverRegistry> build();

private:
    std::vector<ResolverBinding> m_pending;
};

// Typed immutable evidence receipt (ADL §33). The digest is SHA-256 over
// the resolver's result content bytes; the id is fnv1a64 over the bound
// identity fields — identical evidence therefore carries an identical id
// (shareable across deduplicated identical operations, §26), and any
// bound-field difference produces a different id.
struct EvidenceReceipt
{
    EvidenceId id {};
    RequirementIdentity requirement;
    ResolverIdentity resolver;
    std::string subject; // subject/scope the evidence covers
    std::string source;  // source identity (corpus, tool, run, artifact)
    ContentDigest content_digest {};

    [[nodiscard]] bool operator==(const EvidenceReceipt& other) const noexcept
    {
        return id.fnv == other.id.fnv && requirement == other.requirement && resolver == other.resolver &&
               subject == other.subject && source == other.source && content_digest == other.content_digest;
    }
};

// Mint a receipt from the resolver's raw result content. The content bytes
// are digested, never stored: the receipt binds identity, not payload.
[[nodiscard]] EvidenceReceipt make_evidence_receipt(const RequirementIdentity& requirement,
                                                    const ResolverIdentity& resolver,
                                                    std::string subject,
                                                    std::string source,
                                                    std::span<const std::byte> result_content);
} // namespace qiven::runtime::resolver
