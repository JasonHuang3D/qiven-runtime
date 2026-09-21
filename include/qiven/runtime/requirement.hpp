#pragma once

// ============================================================================
// requirement.hpp — multi-intent requirement derivation with provenance
// (component ADL §26/§27, design §6)
//
// Derivation applies the frozen v4 semantics per intent (the draft's
// derive_requirements — the runtime never re-derives) and takes the UNION
// across the IntentSet. Two intents hitting the same policy rule produce
// ONE RequirementInstance carrying BOTH origin indices: resolution may be
// deduplicated later, but semantic provenance is never erased (ADL §26 —
// deduplication is not requirement deletion).
//
// RequirementIdentity is the stable logical identity (kind + subject +
// boundary + blocking; §27): it is what an evidence receipt binds, and it
// prevents a receipt for SearchLowerLayer("hashing") from satisfying
// SearchLowerLayer("serialization") merely because both share a kind.
// The exact digest format stays deferred — the tuple IS the identity.
// ============================================================================

#include <qiven/runtime/intent.hpp>

#include <qiven/context/cognition.hpp>
#include <qiven/context/runtime.hpp>

#include <string>
#include <vector>

namespace qiven::runtime
{
struct RequirementIdentity
{
    qiven::context::RequirementKind kind = qiven::context::RequirementKind::MandatoryRecall;
    std::string subject;
    qiven::context::RequirementBoundary boundary = qiven::context::RequirementBoundary::BeforeJudgment;
    bool blocking                                = true;

    [[nodiscard]] bool operator==(const RequirementIdentity& other) const noexcept
    {
        return kind == other.kind && subject == other.subject && boundary == other.boundary &&
               blocking == other.blocking;
    }
    [[nodiscard]] bool operator!=(const RequirementIdentity& other) const noexcept
    {
        return !(*this == other);
    }
};

// One derived requirement with runtime provenance: the frozen spec plus
// every intent index that produced it (into the IntentSet passed to the
// derivation).
struct RequirementInstance
{
    RequirementIdentity identity;
    qiven::context::CognitiveRequirement spec;
    std::vector<u32> origin_intents;
};

class RequirementSet
{
public:
    RequirementSet() = default;

    [[nodiscard]] std::span<const RequirementInstance> instances() const& noexcept
    {
        return m_instances;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_instances.empty();
    }

    [[nodiscard]] const RequirementInstance* find(const RequirementIdentity& identity) const noexcept;

    // Insert one derived requirement. An identical identity already present
    // MERGES the origin provenance instead of duplicating or deleting
    // (ADL §26); the specs of identical identities are equal by
    // construction (both derive from the same policy rule shape).
    void merge(RequirementInstance instance);

private:
    std::vector<RequirementInstance> m_instances;
};

// Pure derivation: for every intent in the set, apply the frozen v4
// semantics against the pinned snapshot's invocation policy, then union
// with provenance. Same snapshot + same IntentSet => same RequirementSet.
[[nodiscard]] RequirementSet derive_requirement_set(const qiven::context::Snapshot& snapshot, const IntentSet& intents);
} // namespace qiven::runtime
