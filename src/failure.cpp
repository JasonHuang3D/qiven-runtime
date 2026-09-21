#include <qiven/runtime/failure.hpp>

namespace qiven::runtime
{
void FailureTracker::record(const FailureKey& key, qiven::context::FailureFingerprint fingerprint)
{
    auto found = m_entries.find(key);
    if (found == m_entries.end())
    {
        m_entries.emplace(key, Entry { std::move(fingerprint), 1 });
        return;
    }
    found->second.fingerprint = std::move(fingerprint);
    ++found->second.recurrence;
}

std::optional<qiven::context::ActionIntent> FailureTracker::retry_intent_for(const FailureKey& key) const
{
    const auto found = m_entries.find(key);
    if (found == m_entries.end())
    {
        return std::nullopt;
    }
    qiven::context::ActionIntent intent;
    intent.kind         = qiven::context::ActionKind::RetryFailure;
    intent.priorFailure = found->second.fingerprint;
    intent.tool         = found->second.fingerprint.tool;
    return intent;
}

IntentSet classify_with_failure_memory(const ObservedAction& action, const StructuralFacts& facts,
                                       const FailureTracker& failures)
{
    FailureKey key;
    key.actor      = action.actor.value;
    key.capability = action.capability.value;
    key.target     = fnv1a64(action.target);

    std::optional<qiven::context::ActionIntent> retry = failures.retry_intent_for(key);
    if (!retry.has_value())
    {
        return classify(action, facts);
    }

    // the floor plus the retry-discipline intent, deduplicated by
    // make_intent_set; the floor is never dropped (ADL §21)
    std::vector<IntentClassification> members;
    members.push_back(IntentClassification { classify(action, facts).members()[0].intent, ClassificationBasis::Mechanical });
    members.push_back(IntentClassification { std::move(*retry), ClassificationBasis::Mechanical });
    auto built = make_intent_set(std::move(members));
    QIVEN_VERIFY(built.is_ok());
    return std::move(built).value();
}
} // namespace qiven::runtime
