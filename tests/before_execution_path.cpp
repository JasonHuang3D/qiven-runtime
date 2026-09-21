#include <qiven/runtime/transaction.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <utility>

namespace
{
using qiven::context::CognitiveRequirement;
using qiven::context::PreparationPacket;
using qiven::context::PreparedRequirement;
using qiven::context::RequirementBoundary;
using qiven::context::RequirementStatus;
using qiven::runtime::ControlTransaction;
using qiven::runtime::Disposition;
using qiven::runtime::TransactionMinter;
using qiven::runtime::TransactionPhase;

PreparedRequirement requirement(RequirementBoundary boundary, bool blocking, RequirementStatus status)
{
    PreparedRequirement prepared;
    prepared.requirement          = CognitiveRequirement {};
    prepared.requirement.subject  = "test requirement";
    prepared.requirement.blocking = blocking;
    prepared.boundary             = boundary;
    prepared.status               = status;
    return prepared;
}

PreparationPacket packet_with(std::vector<PreparedRequirement> requirements)
{
    PreparationPacket packet;
    packet.requirements = std::move(requirements);
    return packet;
}

ControlTransaction sample_transaction(TransactionMinter& minter, PreparationPacket packet)
{
    const std::byte blob[] { std::byte { 0x01 } };
    auto action  = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                  qiven::runtime::HarnessSessionId { 1 },
                                                  qiven::runtime::ActorInstanceId { 1 },
                                                  qiven::runtime::CapabilityId { 1 }, "build", "target",
                                                  std::span<const std::byte>(blob));
    auto intents = qiven::runtime::make_intent_set(
        { qiven::runtime::IntentClassification { qiven::context::ActionIntent {}, qiven::runtime::ClassificationBasis::Mechanical } });
    QIVEN_VERIFY(intents.is_ok());
    const qiven::runtime::CorrelationKey correlation { qiven::runtime::RuntimeGenerationId { 1 },
                                                       qiven::runtime::AdapterInstanceId { 1 },
                                                       qiven::runtime::HarnessSessionId { 1 },
                                                       qiven::runtime::ActorInstanceId { 1 },
                                                       qiven::runtime::HarnessActionId { 1 } };
    return qiven::runtime::begin_control_transaction(minter.next(), std::nullopt, correlation, std::move(action),
                                                     std::move(intents).value(), qiven::runtime::port::PinnedCognition {},
                                                     std::move(packet));
}

// bring a transaction to the §32 boundary (PreparedForJudgment)
ControlTransaction prepared_for_execution(TransactionMinter& minter, PreparationPacket packet)
{
    auto transaction = sample_transaction(minter, std::move(packet));
    QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(transaction));
    QIVEN_VERIFY(transaction.phase == TransactionPhase::PreparedForJudgment);
    return transaction;
}
} // namespace

int main()
{
    // §32: satisfied BeforeExecution requirements allow execution without
    // another judgment cycle
    {
        TransactionMinter minter;
        auto transaction = prepared_for_execution(
            minter, packet_with({ requirement(RequirementBoundary::BeforeExecution, true, RequirementStatus::Satisfied) }));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_execution(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::CognitiveAllowed);
        QIVEN_VERIFY(qiven::runtime::disposition_for(transaction) == Disposition::Allow);
    }

    // a pending blocking BeforeExecution requirement Awaits (trusted
    // external requirement still pending) and may resolve LATER while the
    // exact action stays unchanged
    {
        TransactionMinter minter;
        auto transaction = prepared_for_execution(
            minter, packet_with({ requirement(RequirementBoundary::BeforeExecution, true, RequirementStatus::Pending) }));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_execution(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::AwaitingRequirement);
        QIVEN_VERIFY(qiven::runtime::disposition_for(transaction) == Disposition::Await);

        // the trusted external requirement resolves: no new judgment cycle,
        // the SAME transaction crosses to CognitiveAllowed
        qiven::context::mark_satisfied(transaction.packet.requirements[0], "typed-evidence-receipt");
        QIVEN_VERIFY(qiven::runtime::evaluate_before_execution(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::CognitiveAllowed);
        QIVEN_VERIFY(qiven::runtime::disposition_for(transaction) == Disposition::Allow);
    }

    // a failed blocking BeforeExecution requirement denies
    {
        TransactionMinter minter;
        auto transaction = prepared_for_execution(
            minter, packet_with({ requirement(RequirementBoundary::BeforeExecution, true, RequirementStatus::Failed) }));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_execution(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::Denied);
        QIVEN_VERIFY(transaction.terminal());
        QIVEN_VERIFY(qiven::runtime::disposition_for(transaction) == Disposition::Deny);
    }

    // the §32 path refuses to run from any other phase: a fresh
    // transaction that never passed judgment cannot shortcut to execution
    {
        TransactionMinter minter;
        auto transaction = sample_transaction(
            minter, packet_with({ requirement(RequirementBoundary::BeforeExecution, true, RequirementStatus::Satisfied) }));
        QIVEN_VERIFY(!qiven::runtime::evaluate_before_execution(transaction));

        // and a ReDeliberationRequired transaction has no execution path
        auto blocked = sample_transaction(
            minter, packet_with({ requirement(RequirementBoundary::BeforeJudgment, true, RequirementStatus::Pending) }));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(blocked));
        QIVEN_VERIFY(blocked.phase == TransactionPhase::ReDeliberationRequired);
        QIVEN_VERIFY(!qiven::runtime::evaluate_before_execution(blocked));
    }

    // full journey smoke: observe -> classify floor -> derive (empty) ->
    // judgment ready -> execution ready -> CognitiveAllowed
    {
        TransactionMinter minter;
        auto transaction = prepared_for_execution(minter, packet_with({}));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_execution(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::CognitiveAllowed);
        QIVEN_VERIFY(!transaction.terminal());
    }

    // non-blocking BeforeExecution requirements never await
    {
        TransactionMinter minter;
        auto transaction = prepared_for_execution(
            minter, packet_with({ requirement(RequirementBoundary::BeforeExecution, false, RequirementStatus::Pending) }));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_execution(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::CognitiveAllowed);
    }

    std::printf("[ OK ] before-execution-path\n");
    return 0;
}
