#pragma once

// ============================================================================
// intent.hpp — deterministic intent classification (component ADL §20-§22,
// design §4; obligations C-01/C-02)
//
// One physical action may imply several semantic intents: classification
// yields an IntentSet, never one exclusive label, and the control plane
// never narrows the set to "the most likely intent" (ADL §21). Mechanically
// established intents are mandatory members; uncertain-but-possible
// blocking classifications are over-approximated into the set rather than
// silently dropped. Every member records WHY it exists (basis retained,
// ADL §22); the first landing relies on mechanical + structural
// classification only — a semantic classifier's output would itself be
// Judgment and requires a DeploymentProfile-accepted classifier contract.
//
// classify() is a pure function of its inputs: same observed action + same
// structural facts => identical IntentSet, in a stable member order.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/observed_action.hpp>

#include <qiven/context/runtime.hpp>

#include <span>
#include <vector>

namespace qiven::runtime
{
enum class ClassificationBasis : u8
{
    Mechanical, // undeniable from the capability descriptor / observed surface
    Structural, // derived from structural repository/runtime facts
    Declared,   // advisory participant declaration — evaluated, never exclusive
    Judgment,   // semantic classifier output (requires an accepted classifier
                // contract in the DeploymentProfile; not produced by the
                // first landing)
};

struct IntentClassification
{
    qiven::context::ActionIntent intent {};
    ClassificationBasis basis = ClassificationBasis::Mechanical;
};

// One or more intent projections over one observed action (≥1 member,
// deduplicated by full projection equality). Construction goes through
// make_intent_set(), which enforces the invariants.
class IntentSet
{
public:
    IntentSet() = delete;

    [[nodiscard]] std::span<const IntentClassification> members() const& noexcept
    {
        return m_members;
    }

    [[nodiscard]] bool contains_kind(qiven::context::ActionKind kind) const noexcept;

    [[nodiscard]] bool operator==(const IntentSet& other) const noexcept;

private:
    friend qiven::Result<IntentSet> make_intent_set(std::vector<IntentClassification> members);

    explicit IntentSet(std::vector<IntentClassification> members) :
    m_members(std::move(members))
    {
    }

    std::vector<IntentClassification> m_members;
};

// Enforce the invariants: at least one member; duplicates (by full
// projection equality) removed keeping first occurrence order. An empty
// input is a typed failure — an observed action always has at least the
// mechanical floor, so empty means a classifier defect.
[[nodiscard]] qiven::Result<IntentSet> make_intent_set(std::vector<IntentClassification> members);

// Whether two projections are the same intent for set purposes: identical
// kind, claim class, tool, operation, concept/file sets and prior-failure
// identity.
[[nodiscard]] bool same_projection(const qiven::context::ActionIntent& left,
                                   const qiven::context::ActionIntent& right) noexcept;

// Structural facts the classifier consumes. The capability descriptor comes
// from the active DeploymentProfile (it is a structural fact ABOUT the
// deployment, not a claim by the actor); public-contract knowledge is a
// repository-structure fact supplied by the caller.
struct StructuralFacts
{
    adapter::CapabilityDescriptor capability {};
    bool target_is_public_contract = false;
};

// Pure deterministic classification. Rule order (and therefore member
// order) is stable: mechanical floor first, then structural rules, then
// declared (advisory) intents. Always produces at least the BeginTask
// floor when no rule fires.
[[nodiscard]] IntentSet classify(const ObservedAction& action,
                                 const StructuralFacts& facts,
                                 const std::vector<qiven::context::ActionIntent>& declared = {});
} // namespace qiven::runtime
