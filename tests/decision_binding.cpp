#include <qiven/runtime/decision.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <utility>

namespace
{
using qiven::u64;
using qiven::usize;
using qiven::context::CognitiveRequirement;
using qiven::context::PreparationPacket;
using qiven::context::PreparedRequirement;
using qiven::context::RequirementBoundary;
using qiven::context::RequirementStatus;
using qiven::runtime::ConsumeOutcome;
using qiven::runtime::DecisionLedger;
using qiven::runtime::ExecutionDecision;
using qiven::runtime::FreshnessFacts;
using qiven::runtime::RuntimeGeneration;
using qiven::runtime::TransactionMinter;
using qiven::runtime::TransactionPhase;

qiven::runtime::ControlTransaction cognitive_allowed(TransactionMinter& minter, const std::byte* blob, usize size)
{
    auto action  = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                  qiven::runtime::HarnessSessionId { 1 },
                                                  qiven::runtime::ActorInstanceId { 1 },
                                                  qiven::runtime::CapabilityId { 1 }, "build", "target",
                                                  std::span<const std::byte>(blob, size));
    auto intents = qiven::runtime::make_intent_set(
        { qiven::runtime::IntentClassification { qiven::context::ActionIntent {}, qiven::runtime::ClassificationBasis::Mechanical } });
    QIVEN_VERIFY(intents.is_ok());
    const qiven::runtime::CorrelationKey correlation { qiven::runtime::RuntimeGenerationId { 1 },
                                                       qiven::runtime::AdapterInstanceId { 1 },
                                                       qiven::runtime::HarnessSessionId { 1 },
                                                       qiven::runtime::ActorInstanceId { 1 },
                                                       qiven::runtime::HarnessActionId { 1 } };
    PreparationPacket packet;
    PreparedRequirement satisfied;
    satisfied.requirement         = CognitiveRequirement {};
    satisfied.requirement.subject = "search";
    satisfied.boundary            = RequirementBoundary::BeforeExecution;
    satisfied.status              = RequirementStatus::Satisfied;
    packet.requirements.push_back(satisfied);

    auto transaction = qiven::runtime::begin_control_transaction(minter.next(), std::nullopt, correlation,
                                                                 std::move(action), std::move(intents).value(),
                                                                 qiven::runtime::port::PinnedCognition {}, std::move(packet));
    QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(transaction));
    QIVEN_VERIFY(qiven::runtime::evaluate_before_execution(transaction));
    QIVEN_VERIFY(transaction.phase == TransactionPhase::CognitiveAllowed);
    return transaction;
}

RuntimeGeneration sample_generation(u64 profile_revision_value)
{
    qiven::runtime::profile::ProfileBuilder builder;
    qiven::runtime::profile::GovernedActorSet actors;
    qiven::runtime::profile::ActorBinding binding;
    binding.adapter          = qiven::runtime::AdapterInstanceId { 1 };
    binding.session_token    = 1;
    binding.credential_token = 1;
    actors.actors.push_back(binding);
    auto profile = builder.set_name("decision-test")
                       .set_revision(qiven::runtime::ProfileRevision { profile_revision_value })
                       .set_actor_set(std::move(actors))
                       .set_conformance_evidence("rca-7-local")
                       .build();
    QIVEN_VERIFY(profile.is_ok());
    qiven::runtime::GenerationMinter minter;
    return minter.mint(std::move(profile).value(), {}, {});
}

FreshnessFacts facts_for(const ExecutionDecision& decision)
{
    FreshnessFacts facts;
    facts.action_digest              = decision.action_digest;
    facts.generation                 = decision.generation;
    facts.cognition_revision         = decision.cognition_revision;
    facts.profile                    = decision.profile;
    facts.resolver_registry_revision = decision.resolver_registry_revision;
    return facts;
}
} // namespace

int main()
{
    const std::byte blob[] { std::byte { 0x42 } };

    // fail-closed: binding requires CognitiveAllowed
    {
        TransactionMinter minter;
        auto transaction  = cognitive_allowed(minter, blob, sizeof blob);
        transaction.phase = TransactionPhase::PreparedForJudgment;
        auto refused      = qiven::runtime::bind_allow(transaction, sample_generation(1), 7, {});
        QIVEN_VERIFY(!refused.is_ok());
        QIVEN_VERIFY(refused.reason().code == 30);
    }

    // C-10 exact binding: the decision digests the exact action and is
    // deterministic for the same controlled state
    {
        TransactionMinter minter;
        auto transaction             = cognitive_allowed(minter, blob, sizeof blob);
        RuntimeGeneration generation = sample_generation(5);

        auto first  = qiven::runtime::bind_allow(transaction, generation, 9, {});
        auto second = qiven::runtime::bind_allow(transaction, generation, 9, {});
        QIVEN_VERIFY(first.is_ok() && second.is_ok());
        QIVEN_VERIFY(first.value().token.fnv == second.value().token.fnv);
        QIVEN_VERIFY(first.value().action_digest == qiven::runtime::action_digest_of(transaction.action));
        QIVEN_VERIFY(first.value().disposition == qiven::runtime::Disposition::Allow);
        QIVEN_VERIFY(first.value().profile.value == 5);
        QIVEN_VERIFY(first.value().resolver_registry_revision == 9);
        QIVEN_VERIFY(first.value().generation.value == generation.id.value);
    }

    // C-12 single use: consume once; the same token never executes again
    {
        TransactionMinter minter;
        auto transaction = cognitive_allowed(minter, blob, sizeof blob);
        auto bound       = qiven::runtime::bind_allow(transaction, sample_generation(5), 9, {});
        QIVEN_VERIFY(bound.is_ok());

        DecisionLedger ledger;
        auto consumed = ledger.consume(bound.value(), facts_for(bound.value()));
        QIVEN_VERIFY(consumed.outcome == ConsumeOutcome::Consumed);

        auto replay = ledger.consume(bound.value(), facts_for(bound.value()));
        QIVEN_VERIFY(replay.outcome == ConsumeOutcome::AlreadyConsumed);
        QIVEN_VERIFY(ledger.was_consumed(bound.value().token.fnv));
    }

    // C-11: any bound fact changing makes the decision stale — the action
    // re-enters control; the token is burned either way
    {
        TransactionMinter minter;
        auto transaction = cognitive_allowed(minter, blob, sizeof blob);
        auto bound       = qiven::runtime::bind_allow(transaction, sample_generation(5), 9, {});
        QIVEN_VERIFY(bound.is_ok());
        const auto decision = bound.value();

        // changed arguments (a different exact action)
        {
            DecisionLedger ledger;
            FreshnessFacts facts = facts_for(decision);
            const std::byte other[] { std::byte { 0x99 } };
            facts.action_digest = qiven::runtime::action_digest_of(
                qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                               qiven::runtime::HarnessSessionId { 1 },
                                               qiven::runtime::ActorInstanceId { 1 },
                                               qiven::runtime::CapabilityId { 1 }, "build", "target",
                                               std::span<const std::byte>(other, sizeof other)));
            auto result = ledger.consume(decision, facts);
            QIVEN_VERIFY(result.outcome == ConsumeOutcome::StaleAction);
            QIVEN_VERIFY(ledger.was_consumed(decision.token.fnv));
        }

        // generation crossed
        {
            DecisionLedger ledger;
            FreshnessFacts facts = facts_for(decision);
            facts.generation     = qiven::runtime::RuntimeGenerationId { decision.generation.value + 1 };
            QIVEN_VERIFY(ledger.consume(decision, facts).outcome == ConsumeOutcome::StaleGeneration);
        }

        // canonical head advanced past the pin
        {
            DecisionLedger ledger;
            FreshnessFacts facts           = facts_for(decision);
            facts.cognition_revision.value = "sha256:advanced";
            QIVEN_VERIFY(ledger.consume(decision, facts).outcome == ConsumeOutcome::StaleCognition);
        }

        // profile revision changed
        {
            DecisionLedger ledger;
            FreshnessFacts facts = facts_for(decision);
            facts.profile        = qiven::runtime::ProfileRevision { decision.profile.value + 1 };
            QIVEN_VERIFY(ledger.consume(decision, facts).outcome == ConsumeOutcome::StalePolicy);
        }

        // resolver registry changed
        {
            DecisionLedger ledger;
            FreshnessFacts facts             = facts_for(decision);
            facts.resolver_registry_revision = decision.resolver_registry_revision + 1;
            QIVEN_VERIFY(ledger.consume(decision, facts).outcome == ConsumeOutcome::StaleRegistry);
        }

        // evidence set changed (optional revalidation supplied)
        {
            DecisionLedger ledger;
            FreshnessFacts facts      = facts_for(decision);
            facts.evidence_set_digest = qiven::runtime::ContentDigest { qiven::SHA256Digest {} };
            QIVEN_VERIFY(ledger.consume(decision, facts).outcome == ConsumeOutcome::StaleEvidence);
        }
    }

    // a fresh decision for the re-entered proposal consumes cleanly after
    // the old token burned (§41: re-entry, not patching)
    {
        TransactionMinter minter;
        auto first_action   = cognitive_allowed(minter, blob, sizeof blob);
        auto first_decision = qiven::runtime::bind_allow(first_action, sample_generation(5), 9, {});
        QIVEN_VERIFY(first_decision.is_ok());

        DecisionLedger ledger;
        FreshnessFacts changed = facts_for(first_decision.value());
        changed.profile        = qiven::runtime::ProfileRevision { 6 };
        QIVEN_VERIFY(ledger.consume(first_decision.value(), changed).outcome == ConsumeOutcome::StalePolicy);

        auto reentered       = cognitive_allowed(minter, blob, sizeof blob);
        auto second_decision = qiven::runtime::bind_allow(reentered, sample_generation(6), 9, {});
        QIVEN_VERIFY(second_decision.is_ok());
        QIVEN_VERIFY(second_decision.value().token.fnv != first_decision.value().token.fnv);
        QIVEN_VERIFY(ledger.consume(second_decision.value(), facts_for(second_decision.value())).outcome ==
                     ConsumeOutcome::Consumed);
    }

    std::printf("[ OK ] decision-binding\n");
    return 0;
}
