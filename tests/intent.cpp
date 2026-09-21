#include <qiven/runtime/intent.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <vector>

namespace
{
using qiven::usize;
using qiven::context::ActionIntent;
using qiven::context::ActionKind;
using qiven::context::ClaimClass;
using qiven::runtime::AdapterInstanceId;
using qiven::runtime::CapabilityId;
using qiven::runtime::ClassificationBasis;
using qiven::runtime::StructuralFacts;
using qiven::runtime::adapter::CapabilityDescriptor;
using qiven::runtime::adapter::OperationClass;

qiven::runtime::ObservedAction sample_action()
{
    const std::byte blob[] { std::byte { 0x00 } };
    return qiven::runtime::observe_action(AdapterInstanceId { qiven::fnv1a64("loopback") },
                                          qiven::runtime::HarnessSessionId { 1 }, qiven::runtime::ActorInstanceId { 1 },
                                          CapabilityId { 1 }, "write_file", "include/qiven/x.hpp",
                                          std::span<const std::byte>(blob));
}
} // namespace

int main()
{
    // C-02: one physical action implies SEVERAL intents — a claim-capable,
    // world-mutating surface on a public contract yields the union; nothing
    // is narrowed to "the most likely intent"
    {
        StructuralFacts facts;
        facts.capability.can_publish_claims = true;
        facts.capability.can_mutate_world   = true;
        facts.target_is_public_contract     = true;

        const auto set = qiven::runtime::classify(sample_action(), facts);
        QIVEN_VERIFY(set.contains_kind(ActionKind::MakeCanonicalClaim));
        QIVEN_VERIFY(set.contains_kind(ActionKind::ModifyPublicAPI));
        QIVEN_VERIFY(set.members().size() == 2);
    }

    // over-approximation: a world mutation with no public-contract fact
    // still takes the dependent-sweep interpretation, never the less
    // restrictive reading (ADL §21)
    {
        StructuralFacts facts;
        facts.capability.can_mutate_world = true;

        const auto set = qiven::runtime::classify(sample_action(), facts);
        QIVEN_VERIFY(set.contains_kind(ActionKind::ModifyReferencedContract));
        QIVEN_VERIFY(!set.contains_kind(ActionKind::ModifyPublicAPI));
    }

    // process launch carries the tool-invocation intent mechanically
    {
        StructuralFacts facts;
        facts.capability.operation_class = OperationClass::ProcessLaunch;

        const auto set = qiven::runtime::classify(sample_action(), facts);
        QIVEN_VERIFY(set.contains_kind(ActionKind::InvokeTool));
    }

    // no rule fires: the mechanical floor keeps the set non-empty
    {
        const auto set = qiven::runtime::classify(sample_action(), StructuralFacts {});
        QIVEN_VERIFY(set.members().size() == 1);
        QIVEN_VERIFY(set.contains_kind(ActionKind::BeginTask));
        QIVEN_VERIFY(set.members()[0].basis == ClassificationBasis::Mechanical);
    }

    // declared intents are advisory MEMBERS (evaluated, never exclusive);
    // duplicates against the floor or each other are deduplicated
    {
        ActionIntent declared;
        declared.kind = ActionKind::Commit;
        declared.tool = "write_file";

        ActionIntent duplicate;
        duplicate.kind = ActionKind::Commit;
        duplicate.tool = "write_file";

        StructuralFacts facts;
        facts.capability.can_mutate_world = true;

        const auto set = qiven::runtime::classify(sample_action(), facts, { declared, duplicate });
        QIVEN_VERIFY(set.contains_kind(ActionKind::ModifyReferencedContract));
        QIVEN_VERIFY(set.contains_kind(ActionKind::Commit));

        // the declared Commit appears exactly once, with its basis recorded
        usize commit_members = 0;
        for (const auto& member : set.members())
        {
            if (member.intent.kind == ActionKind::Commit)
            {
                ++commit_members;
                QIVEN_VERIFY(member.basis == ClassificationBasis::Declared);
            }
        }
        QIVEN_VERIFY(commit_members == 1);
    }

    // basis retention is part of the contract (ADL §22): order is stable —
    // mechanical first, then structural, then declared
    {
        ActionIntent declared;
        declared.kind = ActionKind::Publish;

        StructuralFacts facts;
        facts.capability.can_publish_claims = true;
        facts.capability.can_mutate_world   = true;

        const auto set = qiven::runtime::classify(sample_action(), facts, { declared });
        QIVEN_VERIFY(set.members().size() == 3);
        QIVEN_VERIFY(set.members()[0].basis == ClassificationBasis::Mechanical);
        QIVEN_VERIFY(set.members()[1].basis == ClassificationBasis::Structural);
        QIVEN_VERIFY(set.members()[2].basis == ClassificationBasis::Declared);
    }

    // determinism: identical inputs classify to identical sets
    {
        StructuralFacts facts;
        facts.capability.can_publish_claims = true;

        const auto left  = qiven::runtime::classify(sample_action(), facts);
        const auto right = qiven::runtime::classify(sample_action(), facts);
        QIVEN_VERIFY(left == right);
    }

    // make_intent_set enforces the non-empty invariant as a typed failure
    {
        const auto result = qiven::runtime::make_intent_set({});
        QIVEN_VERIFY(!result.is_ok());
        QIVEN_VERIFY(result.reason().category == qiven::error_category::internal);
    }

    std::printf("[ OK ] intent\n");
    return 0;
}
