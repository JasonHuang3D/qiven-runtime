#include <qiven/runtime/requirement.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <utility>

namespace
{
using qiven::context::ActionIntent;
using qiven::context::ActionKind;
using qiven::context::ClaimClass;
using qiven::context::InvocationRule;
using qiven::context::RequirementBoundary;
using qiven::context::RequirementKind;
using qiven::context::Snapshot;
using qiven::runtime::ClassificationBasis;
using qiven::runtime::IntentClassification;
using qiven::runtime::IntentSet;
using qiven::runtime::RequirementIdentity;
using qiven::runtime::RequirementSet;

qiven::context::ActionIntent intent_of(ActionKind kind, ClaimClass claim_class)
{
    ActionIntent intent;
    intent.kind       = kind;
    intent.claimClass = claim_class;
    return intent;
}

qiven::runtime::IntentSet set_of(std::vector<ActionIntent> intents)
{
    std::vector<IntentClassification> members;
    members.reserve(intents.size());
    for (ActionIntent& intent : intents)
    {
        members.push_back(IntentClassification { std::move(intent), ClassificationBasis::Mechanical });
    }
    auto built = qiven::runtime::make_intent_set(std::move(members));
    QIVEN_VERIFY(built.is_ok());
    return std::move(built).value();
}

Snapshot snapshot_with_rules(std::vector<InvocationRule> rules)
{
    Snapshot snapshot;
    snapshot.invocation.present = true;
    snapshot.invocation.rules   = std::move(rules);
    return snapshot;
}
} // namespace

int main()
{
    // single intent: each applicable rule becomes one instance carrying
    // the originating intent index
    {
        InvocationRule rule {};
        rule.action      = ActionKind::ModifyPublicAPI;
        rule.requirement = RequirementKind::SearchLowerLayer;
        rule.subject     = "foundation surface";
        rule.boundary    = RequirementBoundary::BeforeJudgment;

        Snapshot snapshot = snapshot_with_rules({ rule });
        const auto set    = qiven::runtime::derive_requirement_set(
            snapshot, set_of({ intent_of(ActionKind::ModifyPublicAPI, ClaimClass::LocalRecall) }));

        QIVEN_VERIFY(set.instances().size() == 1);
        QIVEN_VERIFY(set.instances()[0].identity.subject == "foundation surface");
        QIVEN_VERIFY(set.instances()[0].identity.kind == RequirementKind::SearchLowerLayer);
        QIVEN_VERIFY(set.instances()[0].origin_intents.size() == 1);
        QIVEN_VERIFY(set.instances()[0].origin_intents[0] == 0);
    }

    // ADL §26 provenance merge: two intents hitting the same rule produce
    // ONE instance remembering BOTH origins — dedup never deletes policy
    {
        InvocationRule rule {};
        rule.action      = ActionKind::IntroducePrimitive;
        rule.requirement = RequirementKind::SearchLowerLayer;
        rule.subject     = "lower layers";

        Snapshot snapshot = snapshot_with_rules({ rule });
        const auto set    = qiven::runtime::derive_requirement_set(
            snapshot,
            set_of({ intent_of(ActionKind::IntroducePrimitive, ClaimClass::LocalRecall),
                        intent_of(ActionKind::IntroducePrimitive, ClaimClass::CanonicalFact) }));

        QIVEN_VERIFY(set.instances().size() == 1);
        QIVEN_VERIFY(set.instances()[0].origin_intents.size() == 2);
        QIVEN_VERIFY(set.instances()[0].origin_intents[0] == 0);
        QIVEN_VERIFY(set.instances()[0].origin_intents[1] == 1);
    }

    // ADL §27 identity: same kind, different subject => distinct instances;
    // a receipt identity for one subject never matches the other
    {
        InvocationRule hashing {};
        hashing.action      = ActionKind::IntroducePrimitive;
        hashing.requirement = RequirementKind::SearchLowerLayer;
        hashing.subject     = "hashing";

        InvocationRule serialization {};
        serialization.action      = ActionKind::IntroducePrimitive;
        serialization.requirement = RequirementKind::SearchLowerLayer;
        serialization.subject     = "serialization";

        Snapshot snapshot = snapshot_with_rules({ hashing, serialization });
        const auto set    = qiven::runtime::derive_requirement_set(
            snapshot, set_of({ intent_of(ActionKind::IntroducePrimitive, ClaimClass::LocalRecall) }));

        QIVEN_VERIFY(set.instances().size() == 2);

        RequirementIdentity hashing_identity;
        hashing_identity.kind    = RequirementKind::SearchLowerLayer;
        hashing_identity.subject = "hashing";
        QIVEN_VERIFY(set.find(hashing_identity) != nullptr);
        QIVEN_VERIFY(set.find(hashing_identity)->spec.subject == "hashing");

        RequirementIdentity other;
        other.kind    = RequirementKind::SearchLowerLayer;
        other.subject = "serialization";
        QIVEN_VERIFY(set.find(other) != nullptr);
        QIVEN_VERIFY(set.find(hashing_identity)->identity != set.find(other)->identity);
    }

    // frozen S1-01 passthrough: a claim-scoped rule matches only the same
    // claim class; an unscoped rule matches every class
    {
        InvocationRule scoped {};
        scoped.action      = ActionKind::MakeCanonicalClaim;
        scoped.claimClass  = ClaimClass::CanonicalFact;
        scoped.requirement = RequirementKind::VerifyCanonical;
        scoped.subject     = "canonical source";

        Snapshot snapshot = snapshot_with_rules({ scoped });

        const auto matching = qiven::runtime::derive_requirement_set(
            snapshot, set_of({ intent_of(ActionKind::MakeCanonicalClaim, ClaimClass::CanonicalFact) }));
        QIVEN_VERIFY(matching.instances().size() == 1);

        const auto non_matching = qiven::runtime::derive_requirement_set(
            snapshot, set_of({ intent_of(ActionKind::MakeCanonicalClaim, ClaimClass::LiveFact) }));
        QIVEN_VERIFY(non_matching.empty());
    }

    // an absent policy derives nothing — readiness gating (fail-closed
    // World A) is a later batch's predicate, not silently injected here
    {
        Snapshot snapshot;
        const auto set = qiven::runtime::derive_requirement_set(
            snapshot, set_of({ intent_of(ActionKind::BeginTask, ClaimClass::LocalRecall) }));
        QIVEN_VERIFY(set.empty());
    }

    // purity: same inputs derive the same set
    {
        InvocationRule rule {};
        rule.action      = ActionKind::Commit;
        rule.requirement = RequirementKind::RunMechanicalCheck;
        rule.subject     = "attribution format";

        Snapshot snapshot = snapshot_with_rules({ rule });
        const auto left   = qiven::runtime::derive_requirement_set(
            snapshot, set_of({ intent_of(ActionKind::Commit, ClaimClass::LocalRecall) }));
        const auto right = qiven::runtime::derive_requirement_set(
            snapshot, set_of({ intent_of(ActionKind::Commit, ClaimClass::LocalRecall) }));
        QIVEN_VERIFY(left.instances().size() == right.instances().size());
        QIVEN_VERIFY(left.find(right.instances()[0].identity) != nullptr);
    }

    std::printf("[ OK ] requirement-derivation\n");
    return 0;
}
