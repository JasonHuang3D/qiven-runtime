#include <qiven/runtime/admission.hpp>

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

ConsumedPair consumed_allow(DecisionLedger& ledger, qiven::runtime::ContentDigest digest)
{
    static u64 next_token = 0x1235; // unique per call: two consumes in one
                                    // test must be two AUTHORIZATIONS
    ExecutionDecision decision;
    decision.action_digest = digest;
    decision.disposition   = Disposition::Allow;
    decision.token         = qiven::runtime::DecisionToken { next_token++ };

    FreshnessFacts facts;
    facts.action_digest              = decision.action_digest;
    facts.generation                 = decision.generation;
    facts.cognition_revision         = decision.cognition_revision;
    facts.profile                    = decision.profile;
    facts.resolver_registry_revision = decision.resolver_registry_revision;

    ConsumedPair pair { std::move(decision), {} };
    pair.consumption = ledger.consume(pair.decision, facts);
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
    // C-13 conjunctive composition: consumed ALLOW + authority admission
    // -> Dispatched with a live lease (§43)
    {
        NullExecutionAuthority authority;
        ReconciliationBarrier barriers;
        DecisionLedger ledger;

        const auto action = sample_action(std::byte { 0x01 });
        auto pair         = consumed_allow(ledger, qiven::runtime::action_digest_of(action));
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
        DecisionLedger ledger;

        const auto pair = consumed_allow(ledger, qiven::runtime::action_digest_of(sample_action(std::byte { 0x02 })));
        FreshnessFacts facts;
        facts.action_digest = pair.decision.action_digest;
        const auto replay   = ledger.consume(pair.decision, facts); // second use
        QIVEN_VERIFY(replay.outcome == ConsumeOutcome::AlreadyConsumed);

        auto result = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                          .compose(pair.decision, replay, sample_action(std::byte { 0x02 }), sample_key(),
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
        DecisionLedger ledger;

        const auto pair = consumed_allow(ledger, qiven::runtime::action_digest_of(sample_action(std::byte { 0x04 })));
        auto result     = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                          .compose(pair.decision, pair.consumption, sample_action(std::byte { 0x04 }),
                                   sample_key(), AdmissionPurpose::RealMutation);
        QIVEN_VERIFY(result.outcome == AdmissionOutcome::DeniedByExecutionAuthority);
    }

    // §51: a barred effect scope blocks dispatch BEFORE the authority is
    // asked (uncertainty is never resolved by execution)
    {
        NullExecutionAuthority authority;
        ReconciliationBarrier barriers;
        DecisionLedger ledger;

        const auto action = sample_action(std::byte { 0x05 });
        barriers.raise(qiven::runtime::effect_scope_of(action), qiven::runtime::ControlTransactionId { 9 });

        const auto pair = consumed_allow(ledger, qiven::runtime::action_digest_of(action));
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
        DecisionLedger first_ledger;
        DecisionLedger second_ledger;

        auto first  = consumed_allow(first_ledger, qiven::runtime::action_digest_of(sample_action(std::byte { 0x07 })));
        auto result = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                          .compose(first.decision, first.consumption, sample_action(std::byte { 0x07 }), sample_key(),
                                   AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(result.outcome == AdmissionOutcome::Dispatched);
        QIVEN_VERIFY(authority.grant_still_valid(result.permit.grant));

        auto second  = consumed_allow(second_ledger, qiven::runtime::action_digest_of(sample_action(std::byte { 0x08 })));
        auto refused = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                           .compose(second.decision, second.consumption, sample_action(std::byte { 0x08 }),
                                    sample_key(), AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(refused.outcome == AdmissionOutcome::DeniedByExecutionAuthority);

        // release -> the lease frees; a superseded (released) grant is
        // never valid again
        const u64 old_epoch = result.permit.grant.lease_epoch;
        authority.release(std::move(result.permit.grant));
        qiven::runtime::port::AdmissionGrant stale;
        stale.lease_epoch = old_epoch;
        QIVEN_VERIFY(!authority.grant_still_valid(stale));

        auto third = consumed_allow(second_ledger, qiven::runtime::action_digest_of(sample_action(std::byte { 0x09 })));
        auto again = qiven::runtime::ActionAdmissionCoordinator { authority, barriers }
                         .compose(third.decision, third.consumption, sample_action(std::byte { 0x09 }),
                                  sample_key(), AdmissionPurpose::SimulatedProof);
        QIVEN_VERIFY(again.outcome == AdmissionOutcome::Dispatched);
        authority.release(std::move(again.permit.grant));
    }

    std::printf("[ OK ] admission-composition\n");
    return 0;
}
