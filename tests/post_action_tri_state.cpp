#include <qiven/runtime/outcome.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>

namespace
{
using qiven::u64;
using qiven::runtime::ContentDigest;
using qiven::runtime::CorrelationKey;
using qiven::runtime::EffectScope;
using qiven::runtime::ExecutionDecision;
using qiven::runtime::ExecutionOutcome;
using qiven::runtime::HarnessActionId;
using qiven::runtime::ObservationStatus;
using qiven::runtime::PostActionObservation;
using qiven::runtime::PostActionObserver;

ExecutionDecision sample_decision()
{
    ExecutionDecision decision;
    decision.action_digest = ContentDigest { qiven::SHA256Digest { std::byte { 0x11 } } };
    decision.transaction   = qiven::runtime::ControlTransactionId { 3 };
    decision.generation    = qiven::runtime::RuntimeGenerationId { 2 };
    decision.session       = qiven::runtime::HarnessSessionId { 5 };
    decision.actor         = qiven::runtime::ActorInstanceId { 6 };
    decision.disposition   = qiven::runtime::Disposition::Allow;
    return decision;
}

CorrelationKey correlation_of(const ExecutionDecision& decision, u64 harness_action)
{
    return CorrelationKey { decision.generation,
                            qiven::runtime::AdapterInstanceId { 1 },
                            decision.session,
                            decision.actor,
                            HarnessActionId { harness_action } };
}

PostActionObservation observation_of(const ExecutionDecision& decision, ExecutionOutcome reported, std::string evidence)
{
    PostActionObservation observation;
    observation.transaction    = decision.transaction;
    observation.harness_action = HarnessActionId { 1 };
    observation.action_digest  = decision.action_digest;
    observation.generation     = decision.generation;
    observation.reported       = reported;
    observation.scope          = EffectScope { 77 };
    observation.evidence       = std::move(evidence);
    return observation;
}
} // namespace

int main()
{
    const CorrelationKey correlation = correlation_of(sample_decision(), 1);

    // §46/§11.3 correlation: a digest that disagrees with the consumed
    // decision is rejected
    {
        PostActionObserver observer;
        auto decision             = sample_decision();
        auto observation          = observation_of(decision, ExecutionOutcome::Succeeded, "exit 0");
        observation.action_digest = ContentDigest { qiven::SHA256Digest { std::byte { 0x99 } } };
        const auto result         = observer.observe(decision, correlation, observation);
        QIVEN_VERIFY(result.status == ObservationStatus::RejectedCorrelationMismatch);
    }

    // §11.3 COMPLETE correlation: any other bound field disagreeing is
    // equally rejected — transaction, generation, harness action, session
    // and actor are all validated, not only the digest
    {
        PostActionObserver observer;
        const auto decision = sample_decision();

        auto wrong_transaction        = observation_of(decision, ExecutionOutcome::Succeeded, "exit 0");
        wrong_transaction.transaction = qiven::runtime::ControlTransactionId { 999 };
        QIVEN_VERIFY(observer.observe(decision, correlation, wrong_transaction).status ==
                     ObservationStatus::RejectedCorrelationMismatch);

        auto wrong_generation       = observation_of(decision, ExecutionOutcome::Succeeded, "exit 0");
        wrong_generation.generation = qiven::runtime::RuntimeGenerationId { 42 };
        QIVEN_VERIFY(observer.observe(decision, correlation, wrong_generation).status ==
                     ObservationStatus::RejectedCorrelationMismatch);

        auto wrong_harness           = observation_of(decision, ExecutionOutcome::Succeeded, "exit 0");
        wrong_harness.harness_action = HarnessActionId { 77 };
        QIVEN_VERIFY(observer.observe(decision, correlation, wrong_harness).status ==
                     ObservationStatus::RejectedCorrelationMismatch);

        // a correlation tuple whose session differs from the decision's
        // bound session is rejected even with a matching digest
        const CorrelationKey other_session = correlation_of(decision, 1);
        CorrelationKey forged              = other_session;
        forged.session                     = qiven::runtime::HarnessSessionId { 555 };
        QIVEN_VERIFY(observer.observe(decision, forged, observation_of(decision, ExecutionOutcome::Succeeded, "exit 0"))
                         .status == ObservationStatus::RejectedCorrelationMismatch);
    }

    // §46 duplicate: a DIFFERENT second observation for the same exact
    // execution is an integrity rejection; an identical re-delivery is an
    // idempotent replay
    {
        PostActionObserver observer;
        const auto decision = sample_decision();

        const auto first = observer.observe(decision, correlation,
                                            observation_of(decision, ExecutionOutcome::Succeeded, "exit 0"));
        QIVEN_VERIFY(first.status == ObservationStatus::Accepted);

        const auto different = observer.observe(decision, correlation,
                                                observation_of(decision, ExecutionOutcome::Failed, "exit 1"));
        QIVEN_VERIFY(different.status == ObservationStatus::RejectedDuplicate);

        const auto identical = observer.observe(decision, correlation,
                                                observation_of(decision, ExecutionOutcome::Succeeded, "exit 0"));
        QIVEN_VERIFY(identical.status == ObservationStatus::Accepted);
        QIVEN_VERIFY(identical.outcome == ExecutionOutcome::Succeeded);
        QIVEN_VERIFY(observer.observed(correlation));
    }

    // C-14 tri-state: established success is Succeeded and terminal
    {
        PostActionObserver observer;
        const auto decision = sample_decision();
        const auto result =
            observer.observe(decision, correlation, observation_of(decision, ExecutionOutcome::Succeeded, "exit 0"));
        QIVEN_VERIFY(result.status == ObservationStatus::Accepted);
        QIVEN_VERIFY(result.outcome == ExecutionOutcome::Succeeded);
        QIVEN_VERIFY(!result.barrier_active);
    }

    // C-14: established failure mints a NORMALIZED fingerprint (§48)
    {
        PostActionObserver observer;
        const auto decision = sample_decision();
        const auto result   = observer.observe(decision, correlation,
                                               observation_of(decision, ExecutionOutcome::Failed,
                                                              "tool crashed at 2026-09-21 18:00:01"));
        QIVEN_VERIFY(result.status == ObservationStatus::Accepted);
        QIVEN_VERIFY(result.outcome == ExecutionOutcome::Failed);
        QIVEN_VERIFY(!result.fingerprint.stableMessage.empty());
        QIVEN_VERIFY(result.fingerprint.stableMessage.find("18:00:01") == std::string::npos); // volatile text dropped
        QIVEN_VERIFY(!result.barrier_active);
    }

    // C-14 mandatory Indeterminate: "no acknowledgement" NEVER becomes
    // Failed and never mints a fingerprint - it raises the barrier (§49)
    {
        PostActionObserver observer;
        const auto decision = sample_decision();
        auto observation    = observation_of(decision, ExecutionOutcome::Indeterminate, "");
        observation.scope   = EffectScope { 4242 };
        const auto result   = observer.observe(decision, correlation, observation);
        QIVEN_VERIFY(result.status == ObservationStatus::Accepted);
        QIVEN_VERIFY(result.outcome == ExecutionOutcome::Indeterminate);
        QIVEN_VERIFY(result.barrier_active);
        QIVEN_VERIFY(result.barrier_scope == EffectScope { 4242 });
    }

    // a BARE failure claim (no evidence) is demoted to Indeterminate -
    // failure must be mechanically established (§15/§48)
    {
        PostActionObserver observer;
        const auto decision = sample_decision();
        const auto result   = observer.observe(decision, correlation,
                                               observation_of(decision, ExecutionOutcome::Failed, ""));
        QIVEN_VERIFY(result.status == ObservationStatus::Accepted);
        QIVEN_VERIFY(result.outcome == ExecutionOutcome::Indeterminate);
        QIVEN_VERIFY(result.barrier_active);
    }

    // a bare SUCCESS claim is equally unprovable -> Indeterminate
    {
        PostActionObserver observer;
        const auto decision = sample_decision();
        const auto result   = observer.observe(decision, correlation,
                                               observation_of(decision, ExecutionOutcome::Succeeded, ""));
        QIVEN_VERIFY(result.outcome == ExecutionOutcome::Indeterminate);
        QIVEN_VERIFY(result.barrier_active);
    }

    // §50 mechanical scope derivation from the observed action
    {
        const std::byte blob[] { std::byte { 0x01 } };
        const auto action = qiven::runtime::observe_action(
            qiven::runtime::AdapterInstanceId { 1 }, qiven::runtime::HarnessSessionId { 1 },
            qiven::runtime::ActorInstanceId { 1 }, qiven::runtime::CapabilityId { 7 }, "write", "docs/readme.md",
            std::span<const std::byte>(blob, 1));
        const auto scope = qiven::runtime::effect_scope_of(action);
        QIVEN_VERIFY(scope.id != 0);

        // same target+capability -> same scope; different target -> different
        auto same = qiven::runtime::observe_action(
            qiven::runtime::AdapterInstanceId { 2 }, qiven::runtime::HarnessSessionId { 9 },
            qiven::runtime::ActorInstanceId { 3 }, qiven::runtime::CapabilityId { 7 }, "write", "docs/readme.md",
            std::span<const std::byte>(blob, 1));
        QIVEN_VERIFY(qiven::runtime::effect_scope_of(same) == scope);

        auto other_target = qiven::runtime::observe_action(
            qiven::runtime::AdapterInstanceId { 1 }, qiven::runtime::HarnessSessionId { 1 },
            qiven::runtime::ActorInstanceId { 1 }, qiven::runtime::CapabilityId { 7 }, "write", "src/main.cpp",
            std::span<const std::byte>(blob, 1));
        QIVEN_VERIFY(qiven::runtime::effect_scope_of(other_target) != scope);
    }

    std::printf("[ OK ] post-action-tri-state\n");
    return 0;
}
