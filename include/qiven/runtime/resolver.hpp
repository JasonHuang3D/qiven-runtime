#pragma once

// ============================================================================
// resolver.hpp — accepted resolver bindings and typed evidence receipts
// (component ADL §28/§29/§33; obligations C-05/C-06; production-MVP
// architecture §8.3, MVP-0)
//
// The registry answers: which ACCEPTED mechanism may satisfy this
// RequirementKind under this CognitionView in this generation. Selection
// and validation are keyed by the full four-tuple
// (RequirementKind, ResolverType, ResolverVersion, CognitionView) —
// production-MVP §8.3. No binding => the requirement stays unsatisfied
// (fail-closed, §29 — never "skip requirement"); there is no silent
// fallback to an arbitrary available mechanism. The registry is
// immutable once built; changing a binding is a new generation (§28).
//
// A resolver never returns a naked bool for mandatory satisfaction: it
// produces an immutable EvidenceReceipt binding the requirement
// identity, the resolver identity (type, name, version, build), the
// evidence source (revision and path), a content digest over the
// evidence bytes, issuance/expiry, the policy digest and generation
// under which it was accepted, and its reuse class (§33; §8.3 receipt
// field list). The receipt is runtime evidence, not automatically
// canonical evidence. DecisionBinder consumes only receipts the
// registry validated and the journal persisted — never free-form
// strings or caller assertions.
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

// The mechanism family a resolver belongs to. Selection requires the
// exact type — a receipt from a different type can never satisfy a
// binding even when its RequirementKind matches (production-MVP §8.3).
enum class ResolverType : u8
{
    AttributableSearch,
    CanonicalRecall,
    LiveObservation,
    ToolContract,
    MechanicalCheck,
    TypedHumanHandoff,
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

// The full selection key (production-MVP §8.3). Exact-match only.
struct ResolverKey
{
    qiven::context::RequirementKind kind = qiven::context::RequirementKind::MandatoryRecall;
    ResolverType type                    = ResolverType::AttributableSearch;
    u64 version                          = 0;
    std::string cognition_view; // e.g. "default" day-one; the bundle view in MVP-2

    [[nodiscard]] bool operator==(const ResolverKey& other) const noexcept
    {
        return kind == other.kind && type == other.type && version == other.version &&
               cognition_view == other.cognition_view;
    }
};

struct ResolverBinding
{
    ResolverKey key;
    ResolverIdentity resolver; // name must be stable per (type, version)
    EvidenceType accepted_evidence = EvidenceType::AttributableSearch;
    TrustDomain trust              = TrustDomain::Mechanism;
};

class RequirementResolverRegistry
{
public:
    RequirementResolverRegistry() = default;

    // Exact four-tuple lookup. nullptr means NO accepted binding: the
    // requirement remains unsatisfied (fail-closed). There is no fallback
    // lookup and no kind-only shortcut.
    [[nodiscard]] const ResolverBinding* find(const ResolverKey& key) const noexcept;

    // All bindings accepted for one (kind, cognition_view) pair. The
    // control core requires this to resolve to EXACTLY ONE binding for a
    // dispatchable requirement: ambiguity is fail-closed, never guessed.
    [[nodiscard]] std::vector<const ResolverBinding*> find_for_kind_and_view(
        qiven::context::RequirementKind kind, const std::string& cognition_view) const;

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

// The ONLY construction path. Duplicate keys (the exact four-tuple) are
// rejected: exactly one accepted mechanism per key per generation.
class RequirementResolverRegistry::Builder
{
public:
    Builder& add(ResolverBinding binding);
    [[nodiscard]] qiven::Result<RequirementResolverRegistry> build();

private:
    std::vector<ResolverBinding> m_pending;
};

// Typed immutable evidence receipt (ADL §33; production-MVP §8.3 field
// list). The content digest is SHA-256 over the resolver's result
// content bytes; the id is fnv1a64 over the bound identity fields —
// identical evidence therefore carries an identical id (shareable
// across deduplicated identical operations, §26), and any bound-field
// difference produces a different id. The id is identity, never an
// authorization token.
struct EvidenceReceipt
{
    EvidenceId id {};
    RequirementIdentity requirement;                      // the requirement this receipt binds
    ResolverIdentity resolver;                            // name + version of the producing resolver
    ResolverType type = ResolverType::AttributableSearch; // mechanism family
    std::string resolver_build;                           // build identity of the resolver
    qiven::context::RevisionId source_revision;           // exact source revision (git oid / content addr)
    std::string source_path;                              // path within the source
    EvidenceType evidence_type = EvidenceType::AttributableSearch;
    std::string subject; // subject/scope the evidence covers
    std::string source;  // source identity (corpus, tool, run, artifact)
    ContentDigest content_digest {};
    u64 issued_at_ms  = 0;             // epoch milliseconds; 0 = unattributed (test-only)
    u64 expires_at_ms = 0;             // epoch milliseconds; 0 = no expiry claimed
    ContentDigest policy_digest {};    // policy digest under which accepted
    RuntimeGenerationId generation {}; // generation under which accepted
    bool reusable_across_transactions = false;

    [[nodiscard]] bool operator==(const EvidenceReceipt& other) const noexcept
    {
        return id.fnv == other.id.fnv && requirement == other.requirement && resolver == other.resolver &&
               type == other.type && resolver_build == other.resolver_build &&
               source_revision.value == other.source_revision.value && source_path == other.source_path &&
               evidence_type == other.evidence_type && subject == other.subject && source == other.source &&
               content_digest == other.content_digest && issued_at_ms == other.issued_at_ms &&
               expires_at_ms == other.expires_at_ms && policy_digest == other.policy_digest &&
               generation.value == other.generation.value &&
               reusable_across_transactions == other.reusable_across_transactions;
    }
};

// The resolution context a mechanism mints receipts against: the live
// generation, the pinned policy digest and cognition source revision,
// the resolver build identity, and the time authority. Receipts minted
// outside this context cannot validate.
struct ReceiptContext
{
    RuntimeGenerationId generation {};
    ContentDigest policy_digest {};
    qiven::context::RevisionId source_revision; // the pinned revision evidence resolves against
    std::string resolver_build;                 // build identity of the executing resolver
    u64 now_ms = 0;
};

// Mint a receipt from the resolver's raw result content. The content bytes
// are digested, never stored: the receipt binds identity, not payload.
[[nodiscard]] EvidenceReceipt make_evidence_receipt(const RequirementIdentity& requirement,
                                                    const ResolverIdentity& resolver,
                                                    ResolverType type,
                                                    EvidenceType evidence_type,
                                                    std::string subject,
                                                    std::string source,
                                                    std::span<const std::byte> result_content,
                                                    const ReceiptContext& context);

// Receipt acceptance check (production-MVP §8.3): a receipt satisfies a
// requirement ONLY when its requirement identity matches, its resolver
// type and version match the accepted binding, its evidence type is the
// binding's accepted type, its source revision is the binding's (or the
// receipt's source is non-empty and the revision non-default), its
// content digest is well-formed, it is not expired, and its generation
// and policy digest match the live ones. Any mismatch is enumerated —
// fail-closed, never guessed into place.
enum class ReceiptRejection : u8
{
    None,
    WrongRequirement,
    WrongResolverType,
    WrongVersion,
    WrongEvidenceType,
    WrongGeneration,
    WrongPolicy,
    Expired,
    BadDigest,
    BadSource,
};

struct ReceiptValidation
{
    ReceiptRejection rejection = ReceiptRejection::None;
};

[[nodiscard]] ReceiptValidation validate_receipt(const EvidenceReceipt& receipt,
                                                 const ResolverBinding& binding,
                                                 const RequirementIdentity& requirement,
                                                 const RuntimeGenerationId& generation,
                                                 const ContentDigest& policy_digest,
                                                 u64 now_ms) noexcept;
} // namespace qiven::runtime::resolver
