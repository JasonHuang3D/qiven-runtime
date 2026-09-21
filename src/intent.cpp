#include <qiven/runtime/intent.hpp>

#include <qiven/contracts.hpp>

#include <algorithm>
#include <utility>

namespace qiven::runtime
{
namespace
{
bool same_string_set(const std::vector<std::string>& left, const std::vector<std::string>& right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    std::vector<const std::string*> left_sorted;
    std::vector<const std::string*> right_sorted;
    for (const std::string& item : left)
    {
        left_sorted.push_back(&item);
    }
    for (const std::string& item : right)
    {
        right_sorted.push_back(&item);
    }
    std::sort(left_sorted.begin(), left_sorted.end(), [](const std::string* a, const std::string* b) { return *a < *b; });
    std::sort(right_sorted.begin(), right_sorted.end(),
              [](const std::string* a, const std::string* b) { return *a < *b; });
    for (usize i = 0; i < left_sorted.size(); ++i)
    {
        if (*left_sorted[i] != *right_sorted[i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

bool same_projection(const qiven::context::ActionIntent& left, const qiven::context::ActionIntent& right) noexcept
{
    if (left.kind != right.kind || left.claimClass != right.claimClass || left.tool != right.tool ||
        left.operation != right.operation)
    {
        return false;
    }
    if (!same_string_set(left.concepts, right.concepts) || !same_string_set(left.files, right.files))
    {
        return false;
    }
    if (left.priorFailure.has_value() != right.priorFailure.has_value())
    {
        return false;
    }
    if (left.priorFailure.has_value())
    {
        const auto& left_failure  = *left.priorFailure;
        const auto& right_failure = *right.priorFailure;
        if (left_failure.operation != right_failure.operation || left_failure.tool != right_failure.tool ||
            left_failure.category != right_failure.category ||
            left_failure.stableMessage != right_failure.stableMessage ||
            left_failure.exitCode != right_failure.exitCode)
        {
            return false;
        }
    }
    return true;
}

bool IntentSet::contains_kind(qiven::context::ActionKind kind) const noexcept
{
    return std::any_of(m_members.begin(), m_members.end(),
                       [kind](const IntentClassification& member) { return member.intent.kind == kind; });
}

bool IntentSet::operator==(const IntentSet& other) const noexcept
{
    if (m_members.size() != other.m_members.size())
    {
        return false;
    }
    for (usize i = 0; i < m_members.size(); ++i)
    {
        if (m_members[i].basis != other.m_members[i].basis ||
            !same_projection(m_members[i].intent, other.m_members[i].intent))
        {
            return false;
        }
    }
    return true;
}

qiven::Result<IntentSet> make_intent_set(std::vector<IntentClassification> members)
{
    using qiven::Error;
    using qiven::error_category;

    if (members.empty())
    {
        return qiven::Result<IntentSet>::fail(
            Error::make(error_category::internal, 10, "an IntentSet requires at least one projection"));
    }

    std::vector<IntentClassification> deduped;
    deduped.reserve(members.size());
    for (IntentClassification& member : members)
    {
        const bool duplicate =
            std::any_of(deduped.begin(), deduped.end(), [&member](const IntentClassification& kept) {
                return kept.basis == member.basis && same_projection(kept.intent, member.intent);
            });
        if (!duplicate)
        {
            deduped.push_back(std::move(member));
        }
    }

    return qiven::Result<IntentSet>(IntentSet { std::move(deduped) });
}

IntentSet classify(const ObservedAction& action,
                   const StructuralFacts& facts,
                   const std::vector<qiven::context::ActionIntent>& declared)
{
    using qiven::context::ActionIntent;
    using qiven::context::ActionKind;
    using qiven::context::ClaimClass;

    std::vector<IntentClassification> members;

    // Mechanical floor — undeniable from the accepted capability descriptor:
    // a claim-publishing surface makes a canonical claim possible whether
    // or not anyone declares one (C-02: over-approximation, never narrowed).
    if (facts.capability.can_publish_claims)
    {
        ActionIntent claim;
        claim.kind       = ActionKind::MakeCanonicalClaim;
        claim.claimClass = ClaimClass::CanonicalFact;
        claim.tool       = action.operation;
        members.push_back(IntentClassification { std::move(claim), ClassificationBasis::Mechanical });
    }

    // Structural rules over the observed surface.
    if (facts.capability.can_mutate_world)
    {
        if (facts.target_is_public_contract)
        {
            ActionIntent modify;
            modify.kind = ActionKind::ModifyPublicAPI;
            modify.tool = action.operation;
            modify.files.push_back(action.target);
            members.push_back(IntentClassification { std::move(modify), ClassificationBasis::Structural });
        }
        else
        {
            // Any world mutation may touch something referenced elsewhere:
            // conservatively require the dependent-sweep interpretation
            // (ADL §21 — never silently choose the less restrictive reading).
            ActionIntent sweep;
            sweep.kind = ActionKind::ModifyReferencedContract;
            sweep.tool = action.operation;
            sweep.files.push_back(action.target);
            members.push_back(IntentClassification { std::move(sweep), ClassificationBasis::Structural });
        }
    }

    if (facts.capability.operation_class == adapter::OperationClass::ProcessLaunch)
    {
        ActionIntent invoke;
        invoke.kind       = ActionKind::InvokeTool;
        invoke.claimClass = ClaimClass::LocalRecall;
        invoke.tool       = action.operation;
        members.push_back(IntentClassification { std::move(invoke), ClassificationBasis::Mechanical });
    }

    // Declared intents are advisory members (ADR-0038): evaluated alongside
    // the mechanical/structural floor, never treated as exclusive.
    for (const ActionIntent& intent : declared)
    {
        members.push_back(IntentClassification { intent, ClassificationBasis::Declared });
    }

    // The BeginTask floor guarantees a non-empty set for any action.
    if (members.empty())
    {
        ActionIntent floor;
        floor.kind = ActionKind::BeginTask;
        members.push_back(IntentClassification { std::move(floor), ClassificationBasis::Mechanical });
    }

    // make_intent_set only fails on empty input (a classifier defect); the
    // floor above makes that unreachable here, so a failure is a defect we
    // surface loudly rather than paper over.
    auto built = make_intent_set(std::move(members));
    QIVEN_VERIFY(built.is_ok());
    return std::move(built).value();
}
} // namespace qiven::runtime
