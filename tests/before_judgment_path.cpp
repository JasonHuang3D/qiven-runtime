#include <qiven/runtime/transaction.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>
#include <utility>

namespace
{
using qiven::context::ActionIntent;
using qiven::context::CognitiveRequirement;
using qiven::context::PreparationFailure;
using qiven::context::PreparationPacket;
using qiven::context::PreparedRequirement;
using qiven::context::RequirementBoundary;
using qiven::context::RequirementStatus;
using qiven::runtime::ClassificationBasis;
using qiven::runtime::ControlTransaction;
using qiven::runtime::Disposition;
using qiven::runtime::IntentClassification;
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
    const std::byte blob[] { std::byte { 0x00 } };
    auto action = qiven::runtime::observe_action(qiven::runtime::AdapterInstanceId { 1 },
                                                 qiven::runtime::HarnessSessionId { 1 },
                                                 qiven::runtime::ActorInstanceId { 1 },
                                                 qiven::runtime::CapabilityId { 1 }, "op", "t",
                                                 std::span<const std::byte>(blob));
    ActionIntent intent; // BeginTask / LocalRecall floor
    auto intents = qiven::runtime::make_intent_set({ IntentClassification { intent, ClassificationBasis::Mechanical } });
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
} // namespace

int main()
{
    // ready packet passes the BeforeJudgment gate
    {
        TransactionMinter minter;
        auto transaction = sample_transaction(
            minter, packet_with({ requirement(RequirementBoundary::BeforeJudgment, true, RequirementStatus::Satisfied) }));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::PreparedForJudgment);
        QIVEN_VERIFY(qiven::runtime::disposition_for(transaction) == Disposition::Await);
    }

    // no requirements at all is ready (an empty policy derives nothing)
    {
        TransactionMinter minter;
        auto transaction = sample_transaction(minter, packet_with({}));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::PreparedForJudgment);
    }

    // C-04: a blocking BeforeJudgment miss means the proposal CANNOT
    // continue - ReDeliberate disposition; the next proposal is a NEW
    // transaction carrying causal_parent, never a repair of this one
    {
        TransactionMinter minter;
        auto first = sample_transaction(
            minter, packet_with({ requirement(RequirementBoundary::BeforeJudgment, true, RequirementStatus::Pending) }));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(first));
        QIVEN_VERIFY(first.phase == TransactionPhase::ReDeliberationRequired);
        QIVEN_VERIFY(first.terminal());
        QIVEN_VERIFY(qiven::runtime::disposition_for(first) == Disposition::ReDeliberate);

        // the re-deliberated proposal: new id, causal parent bound, its own
        // authorization - structurally incapable of reusing the first
        auto second          = sample_transaction(minter, packet_with({}));
        const auto first_id  = first.id;
        second.causal_parent = first_id;
        QIVEN_VERIFY(second.id.value != first_id.value);
        QIVEN_VERIFY(second.causal_parent.has_value() && second.causal_parent->value == first_id.value);
        QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(second));
        QIVEN_VERIFY(second.phase == TransactionPhase::PreparedForJudgment);

        // no repair of the terminal first transaction
        QIVEN_VERIFY(!qiven::runtime::evaluate_before_judgment(first));
        QIVEN_VERIFY(first.phase == TransactionPhase::ReDeliberationRequired);
    }

    // a FAILED blocking requirement denies outright
    {
        TransactionMinter minter;
        auto transaction = sample_transaction(
            minter, packet_with({ requirement(RequirementBoundary::BeforeJudgment, true, RequirementStatus::Failed) }));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::Denied);
        QIVEN_VERIFY(transaction.terminal());
        QIVEN_VERIFY(qiven::runtime::disposition_for(transaction) == Disposition::Deny);
        QIVEN_VERIFY(!qiven::runtime::evaluate_before_judgment(transaction));
    }

    // S0-02: a typed preparation failure fails the gate even with zero
    // listed requirements
    {
        TransactionMinter minter;
        PreparationPacket packet;
        packet.failure   = PreparationFailure::InvocationPolicyMissing;
        auto transaction = sample_transaction(minter, std::move(packet));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::Denied);
    }

    // non-blocking requirements never gate readiness (the frozen predicate)
    {
        TransactionMinter minter;
        auto transaction = sample_transaction(
            minter, packet_with({ requirement(RequirementBoundary::BeforeJudgment, false, RequirementStatus::Pending) }));
        QIVEN_VERIFY(qiven::runtime::evaluate_before_judgment(transaction));
        QIVEN_VERIFY(transaction.phase == TransactionPhase::PreparedForJudgment);
    }

    // the minter is monotonic and non-copyable
    {
        static_assert(!std::is_copy_constructible_v<TransactionMinter>);
        TransactionMinter minter;
        QIVEN_VERIFY(minter.next().value == 1);
        QIVEN_VERIFY(minter.next().value == 2);
        QIVEN_VERIFY(minter.last_value() == 2);
    }

    std::printf("[ OK ] before-judgment-path\n");
    return 0;
}
