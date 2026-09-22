#include <qiven/runtime/admission.hpp>
#include <qiven/runtime/scope.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <utility>

namespace
{
using qiven::u64;
using qiven::runtime::AdmissionOutcome;
using qiven::runtime::BarrierCheck;
using qiven::runtime::ConsumeOutcome;
using qiven::runtime::DecisionLedger;
using qiven::runtime::Disposition;
using qiven::runtime::EffectScope;
using qiven::runtime::ExecutionDecision;
using qiven::runtime::FreshnessFacts;
using qiven::runtime::MutationRelation;
using qiven::runtime::ReconciliationBarrier;
using qiven::runtime::port::AdmissionPurpose;
using qiven::runtime::port::NullExecutionAuthority;

struct ConsumedPair
{
    ExecutionDecision decision;
    qiven::runtime::ConsumeResult consumption {};
};

constexpr u64 now_ms = 1'000'000;
constexpr u64 ttl_ms = 3'600'000;

qiven::runtime::auth::SecretKey fixed_secret()
{
    qiven::runtime::auth::SecretKey key {};
    for (qiven::usize i = 0; i < key.size(); ++i)
    {
        key[i] = static_cast<std::byte>(0x3CU ^ i);
    }
    return key;
}

qiven::runtime::RuntimeGeneration sample_generation()
{
    qiven::runtime::profile::ProfileBuilder builder;
    qiven::runtime::profile::GovernedActorSet actors;
    qiven::runtime::profile::ActorBinding binding;
    binding.adapter          = qiven::runtime::AdapterInstanceId { 1 };
    binding.session_token    = 1;
    binding.credential_token = 1;
    actors.actors.push_back(binding);
    auto profile = builder.set_name("admission-test")
                       .set_revision(qiven::runtime::ProfileRevision { 1 })
                       .set_actor_set(std::move(actors))
                       .set_conformance_evidence("mvp-0-local")
                       .build();
    QIVEN_VERIFY(profile.is_ok());
    qiven::runtime::GenerationMinter minter;
    return minter.mint(std::move(profile).value(), {}, {});
}

// Mint a REAL consumed authorization for one exact action: a
// CognitiveAllowed transaction bound through bind_allow (MVP-0: the
// token is cryptographic — hand-made tokens no longer consume).
ConsumedPair consumed_allow(DecisionLedger& ledger, const qiven::runtime::ObservedAction& action)
{
    static qiven::runtime::TransactionMinter tx_minter;
    static qiven::runtime::SortableIdMinter id_minter;
    static qiven::runtime::RuntimeGeneration generation = sample_generation();
    static const auto scope                             = [] {
        auto built = qiven::runtime::ResourceScope::Builder().add_path("out/bin").build();
        QIVEN_VERIFY(built.is_ok());
        return std::move(built).value();
    }();

    auto intents = qiven::runtime::make_intent_set(
        { qiven::runtime::IntentClassification { qiven::context::ActionIntent {},
                                                 qiven::runtime::ClassificationBasis::Mechanical } });
    QIVEN_VERIFY(intents.is_ok());

    qiven::context::PreparationPacket packet; // no requirements: ready day-one

    auto transaction = qiven::runtime::begin_control_transaction(
        tx_minter.next(), std::nullopt,
        qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 },
                                         qiven::runtime::AdapterInstanceId { 1 },
                                         qiven::runtime::HarnessSessionId { 1 },
                                         qiven::runtime::ActorInstanceId { 1 },
                                         qiven::runtime::HarnessActionId { 1 } },
        action, std::move(intents).value(), qiven::runtime::port::PinnedCognition {}, std::move(packet));
    QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(transaction));
    QIVEN_VERIFY(qiven::runtime::evaluate_before_execution(transaction));
    QIVEN_VERIFY(transaction.phase == qiven::runtime::TransactionPhase::CognitiveAllowed);

    auto bound = qiven::runtime::bind_allow(transaction, generation, scope.digest(), 1, {}, ledger.secret(), id_minter,
                                            now_ms, ttl_ms);
    QIVEN_VERIFY(bound.is_ok());

    FreshnessFacts facts;
    facts.action_digest              = bound.value().action_digest;
    facts.generation                 = bound.value().generation;
    facts.cognition_revision         = bound.value().cognition_revision;
    facts.profile                    = bound.value().profile;
    facts.resolver_registry_revision = bound.value().resolver_registry_revision;
    facts.resource_scope_digest      = bound.value().resource_scope_digest;

    ConsumedPair pair { std::move(bound).value(), {} };
    pair.consumption = ledger.consume(pair.decision, facts, now_ms + 1);
    QIVEN_VERIFY(pair.consumption.outcome == ConsumeOutcome::Consumed);
    return pair;
}

qiven::runtime::ObservedAction sample_action(std::byte payload)
{
    const std::byte blob[] { payload };
    return qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 }, qiven::runtime::HarnessSessionId { 1 },
                                          qiven::runtime::ActorInstanceId { 1 }, qiven::runtime::CapabilityId { 1 },
                                          "build", "out/bin", std::span<const std::byte>(blob, 1));
}

qiven::runtime::CorrelationKey sample_key()
{
    return qiven::runtime::CorrelationKey { qiven::runtime::RuntimeGenerationId { 1 },
                                            qiven::runtime::AdapterInstanceId { 1 },
                                            qiven::runtime::HarnessSessionId { 1 },
                                            qiven::runtime::ActorInstanceId { 1 },
                                            qiven::runtime::HarnessActionId { 1 } };
}
} // namespace

int main()
{
    const auto secret = fixed_secret();

    // C-13 conjunctive composition: consumed ALLOW + authority admission
    // -> Dispatched with a live lease (§43)
    {
        NullExecutionAuthority authority;
        ReconciliationBarrier barriers;
        DecisionLedger ledger { secret };

        const auto action = sample_action(std::byte { 0x01 });
        auto pair         = consumed_allow(ledger, action);
        auto result       = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                          .compose(pair.decision, pair.consumption, action, sample_key(),
                                   AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(result.outcome == AdmissionOutcome::Dispatched);
        QIVEN_VERIFY(result.permit.grant.lease_epoch != 0);
        QIVEN_VERIFY(result.permit.effect_scope == qiven::runtime::effect_scope_of(action));
        QIVEN_VERIFY(result.permit.action_digest == pair.decision.action_digest);

        authority.release(std::move(result.permit.grant));
    }

    // cognitive gate failed: a replayed (already consumed) decision never
    // even reaches the authority plane
    {
        NullExecutionAuthority authority;
        ReconciliationBarrier barriers;
        DecisionLedger ledger { secret };

        const auto action = sample_action(std::byte { 0x02 });
        const auto pair   = consumed_allow(ledger, action);
        FreshnessFacts facts;
        facts.action_digest              = pair.decision.action_digest;
        facts.generation                 = pair.decision.generation;
        facts.cognition_revision         = pair.decision.cognition_revision;
        facts.profile                    = pair.decision.profile;
        facts.resolver_registry_revision = pair.decision.resolver_registry_revision;
        facts.resource_scope_digest      = pair.decision.resource_scope_digest;
        const auto replay                = ledger.consume(pair.decision, facts, now_ms + 2); // second use
        QIVEN_VERIFY(replay.outcome == ConsumeOutcome::AlreadyConsumed);

        auto result = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                          .compose(pair.decision, replay, action, sample_key(),
                                   AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(result.outcome == AdmissionOutcome::DeniedNoCognitiveAllow);
    }

    // a Deny disposition is never composed further, even "consumed"
    {
        NullExecutionAuthority authority;
        ReconciliationBarrier barriers;

        ExecutionDecision denied;
        denied.disposition = Disposition::Deny;
        qiven::runtime::ConsumeResult consumption {};
        consumption.outcome = ConsumeOutcome::Consumed;
        const auto result   = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                                .compose(denied, consumption, sample_action(std::byte { 0x03 }), sample_key(),
                                         AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(result.outcome == AdmissionOutcome::DeniedNoCognitiveAllow);
    }

    // §45/§85 fail-closed: REAL mutation under the Null authority is
    // denied — no execution-authority subsystem exists yet, and a
    // cognitive ALLOW cannot conjure one
    {
        NullExecutionAuthority authority;
        ReconciliationBarrier barriers;
        DecisionLedger ledger { secret };

        const auto action = sample_action(std::byte { 0x04 });
        const auto pair   = consumed_allow(ledger, action);
        auto result       = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                          .compose(pair.decision, pair.consumption, action, sample_key(),
                                   AdmissionPurpose::RealMutation);
        QIVEN_VERIFY(result.outcome == AdmissionOutcome::DeniedByExecutionAuthority);
    }

    // §51: a barred effect scope blocks dispatch BEFORE the authority is
    // asked (uncertainty is never resolved by execution)
    {
        NullExecutionAuthority authority;
        ReconciliationBarrier barriers;
        DecisionLedger ledger { secret };

        const auto action = sample_action(std::byte { 0x05 });
        barriers.raise(qiven::runtime::effect_scope_of(action), qiven::runtime::ControlTransactionId { 9 });

        const auto pair = consumed_allow(ledger, action);
        auto result     = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                          .compose(pair.decision, pair.consumption, action, sample_key(),
                                   AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(result.outcome == AdmissionOutcome::DeniedReconciliationBarrier);

        // an independent scope dispatches fine while the barrier stands
        const auto other = sample_action(std::byte { 0x06 });
        auto unbarred    = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                            .compose(pair.decision, pair.consumption, other, sample_key(),
                                     AdmissionPurpose::SimulatedProof);
        // NOTE: different action bytes -> a decision bound to the FIRST
        // action is stale for the second; the cognitive gate must refuse.
        // This is exactly the §41 discipline: compose with a decision
        // bound to THIS action.
        QIVEN_VERIFY(unbarred.outcome == AdmissionOutcome::DeniedNoCognitiveAllow);
    }

    // single-writer lease (ADR-0026 lineage): a second simulated request
    // while a lease is held is refused fail-closed; release re-arms
    {
        NullExecutionAuthority authority;
        ReconciliationBarrier barriers;
        DecisionLedger first_ledger { secret };
        DecisionLedger second_ledger { secret };

        const auto action_first = sample_action(std::byte { 0x07 });
        auto first              = consumed_allow(first_ledger, action_first);
        auto result             = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                          .compose(first.decision, first.consumption, action_first, sample_key(),
                                   AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(result.outcome == AdmissionOutcome::Dispatched);
        QIVEN_VERIFY(authority.grant_still_valid(result.permit.grant));

        const auto action_second = sample_action(std::byte { 0x08 });
        auto second              = consumed_allow(second_ledger, action_second);
        auto refused             = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                           .compose(second.decision, second.consumption, action_second, sample_key(),
                                    AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(refused.outcome == AdmissionOutcome::DeniedByExecutionAuthority);

        // release -> the lease frees; a superseded (released) grant is
        // never valid again
        const u64 old_epoch = result.permit.grant.lease_epoch;
        authority.release(std::move(result.permit.grant));
        qiven::runtime::port::AdmissionGrant stale;
        stale.lease_epoch = old_epoch;
        QIVEN_VERIFY(!authority.grant_still_valid(stale));

        const auto action_third = sample_action(std::byte { 0x09 });
        auto third              = consumed_allow(second_ledger, action_third);
        auto again              = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                         .compose(third.decision, third.consumption, action_third, sample_key(),
                                  AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(again.outcome == AdmissionOutcome::Dispatched);
        authority.release(std::move(again.permit.grant));
    }

    std::printf("[ OK ] admission-composition\n");
    return 0;
}
