#include <qiven/runtime/transaction.hpp>

namespace qiven::runtime
{
ControlTransaction begin_control_transaction(ControlTransactionId id,
                                             std::optional<ControlTransactionId> causal_parent,
                                             const CorrelationKey& correlation,
                                             ObservedAction action,
                                             IntentSet intents,
                                             port::PinnedCognition cognition,
                                             qiven::context::PreparationPacket packet)
{
    // aggregate initialization: ControlTransaction is never default-
    // constructed (IntentSet's >=1 invariant deletes its default ctor)
    return ControlTransaction { id,
                                causal_parent,
                                correlation,
                                std::move(action),
                                std::move(intents),
                                std::move(cognition),
                                std::move(packet),
                                TransactionPhase::RequirementsDerived };
}

bool evaluate_before_judgment(ControlTransaction& transaction)
{
    using qiven::context::PreparationFailure;
    using qiven::context::RequirementBoundary;
    using qiven::context::RequirementStatus;

    if (transaction.terminal())
    {
        return false; // no post-hoc repair of an accepted or failed path (§18)
    }

    transaction.phase = TransactionPhase::PreparingBeforeJudgment;

    // S0-02: a typed preparation failure fails BOTH gates
    if (transaction.packet.failure != PreparationFailure::None)
    {
        transaction.phase = TransactionPhase::Denied;
        return true;
    }

    if (transaction.packet.ready_for_judgment())
    {
        transaction.phase = TransactionPhase::PreparedForJudgment;
        return true;
    }

    // a blocking BeforeJudgment miss: Failed denies; Pending means the
    // resolution supplies cognition required for the next reasoning cycle
    for (const qiven::context::PreparedRequirement& prepared : transaction.packet.requirements)
    {
        if (!prepared.requirement.blocking || prepared.boundary != RequirementBoundary::BeforeJudgment)
        {
            continue;
        }
        if (prepared.status == RequirementStatus::Failed)
        {
            transaction.phase = TransactionPhase::Denied;
            return true;
        }
    }
    transaction.phase = TransactionPhase::ReDeliberationRequired;
    return true;
}

bool evaluate_before_execution(ControlTransaction& transaction)
{
    using qiven::context::PreparationFailure;
    using qiven::context::RequirementStatus;

    if (transaction.phase != TransactionPhase::PreparedForJudgment &&
        transaction.phase != TransactionPhase::AwaitingRequirement)
    {
        return false; // only the §32 path may run from these phases
    }

    transaction.phase = TransactionPhase::PreparingBeforeExecution;

    if (transaction.packet.failure != PreparationFailure::None)
    {
        transaction.phase = TransactionPhase::Denied;
        return true;
    }

    if (transaction.packet.ready_for_execution())
    {
        transaction.phase = TransactionPhase::CognitiveAllowed;
        return true;
    }

    for (const qiven::context::PreparedRequirement& prepared : transaction.packet.requirements)
    {
        if (!prepared.requirement.blocking)
        {
            continue;
        }
        if (prepared.status == RequirementStatus::Failed)
        {
            transaction.phase = TransactionPhase::Denied;
            return true;
        }
    }
    transaction.phase = TransactionPhase::AwaitingRequirement;
    return true;
}

Disposition disposition_for(const ControlTransaction& transaction) noexcept
{
    switch (transaction.phase)
    {
    case TransactionPhase::CognitiveAllowed:
        return Disposition::Allow;
    case TransactionPhase::ReDeliberationRequired:
        return Disposition::ReDeliberate;
    case TransactionPhase::AwaitingRequirement:
    case TransactionPhase::PreparingBeforeExecution:
    case TransactionPhase::PreparedForJudgment:
        return Disposition::Await;
    default:
        return Disposition::Deny;
    }
}
} // namespace qiven::runtime
