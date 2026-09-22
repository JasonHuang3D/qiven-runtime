#include <qiven/runtime/decision.hpp>
#include <qiven/runtime/scope.hpp>

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
using qiven::runtime::SortableIdMinter;
using qiven::runtime::TransactionMinter;
using qiven::runtime::TransactionPhase;

qiven::runtime::ControlTransaction cognitive_allowed(TransactionMinter& minter, const std::byte* blob, usize size)
{
    auto action  = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                  qiven::runtime::HarnessSessionId { 1 },
                                                  qiven::runtime::ActorInstanceId { 1 },
                                                  qiven::runtime::CapabilityId { 1 }, "build", "state/active-work.yaml",
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
                       .set_conformance_evidence("mvp-0-local")
                       .build();
    QIVEN_VERIFY(profile.is_ok());
    qiven::runtime::GenerationMinter minter;
    return minter.mint(std::move(profile).value(), {}, {});
}

qiven::runtime::auth::SecretKey fixed_secret()
{
    qiven::runtime::auth::SecretKey key {};
    for (usize i = 0; i < key.size(); ++i)
    {
        key[i] = static_cast<std::byte>(0xA5U ^ i);
    }
    return key;
}

qiven::runtime::ResourceScope sample_scope()
{
    auto scope = qiven::runtime::ResourceScope::Builder()
                     .add_path("state/active-work.yaml")
                     .add_path("state/current.md")
                     .add_path("memory/records")
                     .build();
    QIVEN_VERIFY(scope.is_ok());
    return std::move(scope).value();
}

constexpr u64 bind_now_ms = 1'000'000;
constexpr u64 bind_ttl_ms = 3'600'000;

FreshnessFacts facts_for(const ExecutionDecision& decision)
{
    FreshnessFacts facts;
    facts.action_digest              = decision.action_digest;
    facts.generation                 = decision.generation;
    facts.cognition_revision         = decision.cognition_revision;
    facts.profile                    = decision.profile;
    facts.resolver_registry_revision = decision.resolver_registry_revision;
    facts.resource_scope_digest      = decision.resource_scope_digest;
    return facts;
}
} // namespace

int main()
{
    const std::byte blob[] { std::byte { 0x42 } };
    const auto secret = fixed_secret();
    const auto scope  = sample_scope();

    // fail-closed: binding requires CognitiveAllowed
    {
        TransactionMinter minter;
        SortableIdMinter ids;
        auto transaction  = cognitive_allowed(minter, blob, sizeof blob);
        transaction.phase = TransactionPhase::PreparedForJudgment;
        auto refused      = qiven::runtime::bind_allow(transaction, sample_generation(1), scope.digest(), 7, {}, secret,
                                                       ids, bind_now_ms, bind_ttl_ms);
        QIVEN_VERIFY(!refused.is_ok());
        QIVEN_VERIFY(refused.reason().code == 30);
    }

    // C-10 exact binding: the decision digests the exact action; the
    // digests are deterministic for the same controlled state. The TOKEN
    // is single-use per mint (CSPRNG nonce): two binds of identical facts
    // are distinct authorizations with different token values that both
    // validate against the same binding.
    {
        TransactionMinter minter;
        SortableIdMinter ids;
        auto transaction             = cognitive_allowed(minter, blob, sizeof blob);
        RuntimeGeneration generation = sample_generation(5);

        auto first  = qiven::runtime::bind_allow(transaction, generation, scope.digest(), 9, {}, secret, ids,
                                                 bind_now_ms, bind_ttl_ms);
        auto second = qiven::runtime::bind_allow(transaction, generation, scope.digest(), 9, {}, secret, ids,
                                                 bind_now_ms, bind_ttl_ms);
        QIVEN_VERIFY(first.is_ok() && second.is_ok());
        QIVEN_VERIFY(!(first.value().token == second.value().token)); // §40: distinct authorizations
        QIVEN_VERIFY(!(first.value().token_hash == second.value().token_hash));
        QIVEN_VERIFY(qiven::runtime::decision_binding_valid(first.value(), secret));
        QIVEN_VERIFY(qiven::runtime::decision_binding_valid(second.value(), secret));
        QIVEN_VERIFY(first.value().action_digest == qiven::runtime::action_digest_of(transaction.action));
        QIVEN_VERIFY(first.value().action_digest == second.value().action_digest); // digest determinism
        QIVEN_VERIFY(first.value().requirement_set_digest == second.value().requirement_set_digest);
        QIVEN_VERIFY(first.value().disposition == qiven::runtime::Disposition::Allow);
        QIVEN_VERIFY(first.value().profile.value == 5);
        QIVEN_VERIFY(first.value().resolver_registry_revision == 9);
        QIVEN_VERIFY(first.value().generation.value == generation.id.value);
        QIVEN_VERIFY(first.value().expires_at_ms == bind_now_ms + bind_ttl_ms);
        QIVEN_VERIFY(first.value().actor.value == transaction.action.actor.value); // §10.3 binding tuple
        QIVEN_VERIFY(first.value().session.value == transaction.action.session.value);
        QIVEN_VERIFY(first.value().target == transaction.action.target);
    }

    // C-12 single use: consume once; the same token never executes again
    {
        TransactionMinter minter;
        SortableIdMinter ids;
        auto transaction = cognitive_allowed(minter, blob, sizeof blob);
        auto bound       = qiven::runtime::bind_allow(transaction, sample_generation(5), scope.digest(), 9, {}, secret,
                                                      ids, bind_now_ms, bind_ttl_ms);
        QIVEN_VERIFY(bound.is_ok());

        DecisionLedger ledger { secret };
        auto consumed = ledger.consume(bound.value(), facts_for(bound.value()), bind_now_ms + 1);
        QIVEN_VERIFY(consumed.outcome == ConsumeOutcome::Consumed);

        auto replay = ledger.consume(bound.value(), facts_for(bound.value()), bind_now_ms + 2);
        QIVEN_VERIFY(replay.outcome == ConsumeOutcome::AlreadyConsumed);
        QIVEN_VERIFY(ledger.was_consumed(bound.value().token_hash));
    }

    // C-11: any bound fact changing makes the decision stale — the action
    // re-enters control; the token is burned either way
    {
        TransactionMinter minter;
        SortableIdMinter ids;
        auto transaction = cognitive_allowed(minter, blob, sizeof blob);
        auto bound       = qiven::runtime::bind_allow(transaction, sample_generation(5), scope.digest(), 9, {}, secret,
                                                      ids, bind_now_ms, bind_ttl_ms);
        QIVEN_VERIFY(bound.is_ok());
        const auto decision = bound.value();

        // changed arguments (a different exact action)
        {
            DecisionLedger ledger { secret };
            FreshnessFacts facts = facts_for(decision);
            const std::byte other[] { std::byte { 0x99 } };
            facts.action_digest = qiven::runtime::action_digest_of(
                qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                               qiven::runtime::HarnessSessionId { 1 },
                                               qiven::runtime::ActorInstanceId { 1 },
                                               qiven::runtime::CapabilityId { 1 }, "build", "state/active-work.yaml",
                                               std::span<const std::byte>(other, sizeof other)));
            auto result = ledger.consume(decision, facts, bind_now_ms + 1);
            QIVEN_VERIFY(result.outcome == ConsumeOutcome::StaleAction);
            QIVEN_VERIFY(ledger.was_consumed(decision.token_hash));
        }

        // generation crossed
        {
            DecisionLedger ledger { secret };
            FreshnessFacts facts = facts_for(decision);
            facts.generation     = qiven::runtime::RuntimeGenerationId { decision.generation.value + 1 };
            QIVEN_VERIFY(ledger.consume(decision, facts, bind_now_ms + 1).outcome == ConsumeOutcome::StaleGeneration);
        }

        // canonical head advanced past the pin
        {
            DecisionLedger ledger { secret };
            FreshnessFacts facts           = facts_for(decision);
            facts.cognition_revision.value = "sha256:advanced";
            QIVEN_VERIFY(ledger.consume(decision, facts, bind_now_ms + 1).outcome == ConsumeOutcome::StaleCognition);
        }

        // profile revision changed
        {
            DecisionLedger ledger { secret };
            FreshnessFacts facts = facts_for(decision);
            facts.profile        = qiven::runtime::ProfileRevision { decision.profile.value + 1 };
            QIVEN_VERIFY(ledger.consume(decision, facts, bind_now_ms + 1).outcome == ConsumeOutcome::StalePolicy);
        }

        // resolver registry changed
        {
            DecisionLedger ledger { secret };
            FreshnessFacts facts             = facts_for(decision);
            facts.resolver_registry_revision = decision.resolver_registry_revision + 1;
            QIVEN_VERIFY(ledger.consume(decision, facts, bind_now_ms + 1).outcome == ConsumeOutcome::StaleRegistry);
        }

        // evidence set changed (optional revalidation supplied)
        {
            DecisionLedger ledger { secret };
            FreshnessFacts facts      = facts_for(decision);
            facts.evidence_set_digest = qiven::runtime::ContentDigest { qiven::SHA256Digest {} };
            QIVEN_VERIFY(ledger.consume(decision, facts, bind_now_ms + 1).outcome == ConsumeOutcome::StaleEvidence);
        }

        // governed resource scope changed (MVP-0 exit-gate axis)
        {
            DecisionLedger ledger { secret };
            FreshnessFacts facts = facts_for(decision);
            auto wider           = qiven::runtime::ResourceScope::Builder()
                             .add_path("state/active-work.yaml")
                             .add_path("state/current.md")
                             .add_path("memory/records")
                             .add_path("obligations")
                             .build();
            QIVEN_VERIFY(wider.is_ok());
            facts.resource_scope_digest = std::move(wider).value().digest();
            QIVEN_VERIFY(ledger.consume(decision, facts, bind_now_ms + 1).outcome == ConsumeOutcome::StaleScope);
        }
    }

    // §10.3 expiry: past-expiry tokens are Expired even when every fact
    // is otherwise fresh
    {
        TransactionMinter minter;
        SortableIdMinter ids;
        auto transaction = cognitive_allowed(minter, blob, sizeof blob);
        auto bound       = qiven::runtime::bind_allow(transaction, sample_generation(5), scope.digest(), 9, {}, secret,
                                                      ids, bind_now_ms, 1);
        QIVEN_VERIFY(bound.is_ok());
        DecisionLedger ledger { secret };
        QIVEN_VERIFY(ledger.consume(bound.value(), facts_for(bound.value()), bind_now_ms + 2).outcome ==
                     ConsumeOutcome::Expired);
    }

    // §10.3 fail-closed: a tampered binding (forged or modified token)
    // is InvalidToken — never a freshness question, and the token burns
    {
        TransactionMinter minter;
        SortableIdMinter ids;
        auto transaction = cognitive_allowed(minter, blob, sizeof blob);
        auto bound       = qiven::runtime::bind_allow(transaction, sample_generation(5), scope.digest(), 9, {}, secret,
                                                      ids, bind_now_ms, bind_ttl_ms);
        QIVEN_VERIFY(bound.is_ok());

        // tampered MAC: flip one byte of the token
        ExecutionDecision forged = bound.value();
        forged.token.mac[0]      = static_cast<std::byte>(static_cast<unsigned char>(forged.token.mac[0]) ^ 0xFFU);
        DecisionLedger ledger { secret };
        QIVEN_VERIFY(ledger.consume(forged, facts_for(bound.value()), bind_now_ms + 1).outcome ==
                     ConsumeOutcome::InvalidToken);

        // declared hash does not match the token value
        ExecutionDecision mismatched_hash = bound.value();
        mismatched_hash.token_hash.value[0] =
            static_cast<std::byte>(static_cast<unsigned char>(mismatched_hash.token_hash.value[0]) ^ 0x01U);
        DecisionLedger ledger2 { secret };
        QIVEN_VERIFY(ledger2.consume(mismatched_hash, facts_for(bound.value()), bind_now_ms + 1).outcome ==
                     ConsumeOutcome::InvalidToken);

        // the original token is not replayable through a ledger holding
        // different key material: the binding no longer verifies
        qiven::runtime::auth::SecretKey other {};
        for (usize i = 0; i < other.size(); ++i)
        {
            other[i] = static_cast<std::byte>(0x5AU ^ (i * 3U));
        }
        DecisionLedger foreign { other };
        QIVEN_VERIFY(foreign.consume(bound.value(), facts_for(bound.value()), bind_now_ms + 1).outcome ==
                     ConsumeOutcome::InvalidToken);
    }

    // a fresh decision for the re-entered proposal consumes cleanly after
    // the old token burned (§41: re-entry, not patching)
    {
        TransactionMinter minter;
        SortableIdMinter ids;
        auto first_action   = cognitive_allowed(minter, blob, sizeof blob);
        auto first_decision = qiven::runtime::bind_allow(first_action, sample_generation(5), scope.digest(), 9, {},
                                                         secret, ids, bind_now_ms, bind_ttl_ms);
        QIVEN_VERIFY(first_decision.is_ok());

        DecisionLedger ledger { secret };
        FreshnessFacts changed = facts_for(first_decision.value());
        changed.profile        = qiven::runtime::ProfileRevision { 6 };
        QIVEN_VERIFY(ledger.consume(first_decision.value(), changed, bind_now_ms + 1).outcome == ConsumeOutcome::StalePolicy);

        auto reentered       = cognitive_allowed(minter, blob, sizeof blob);
        auto second_decision = qiven::runtime::bind_allow(reentered, sample_generation(6), scope.digest(), 9, {},
                                                          secret, ids, bind_now_ms, bind_ttl_ms);
        QIVEN_VERIFY(second_decision.is_ok());
        QIVEN_VERIFY(!(second_decision.value().token == first_decision.value().token));
        QIVEN_VERIFY(ledger.consume(second_decision.value(), facts_for(second_decision.value()), bind_now_ms + 1).outcome ==
                     ConsumeOutcome::Consumed);
    }

    std::printf("[ OK ] decision-binding\n");
    return 0;
}
