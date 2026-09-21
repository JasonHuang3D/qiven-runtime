#include <qiven/runtime/failure.hpp>

#include <qiven/contracts.hpp>

#include <cstdio>

namespace
{
using qiven::u64;
using qiven::context::ActionKind;
using qiven::context::FailureFingerprint;
using qiven::runtime::FailureKey;
using qiven::runtime::FailureTracker;
using qiven::runtime::StructuralFacts;

FailureKey key_of(u64 actor, u64 target)
{
    return FailureKey { actor, 5, target };
}

FailureFingerprint fingerprint_with(std::string tool, std::string message)
{
    FailureFingerprint fingerprint;
    fingerprint.tool          = std::move(tool);
    fingerprint.stableMessage = std::move(message);
    return fingerprint;
}
} // namespace

int main()
{
    // a recorded failure resurfaces as a RetryFailure intent for the
    // equivalent triple - the participant does not need to remember (C-17)
    {
        FailureTracker tracker;
        tracker.record(key_of(1, 100), fingerprint_with("msvc", "LNK1103 corrupt pdb"));
        const auto retry = tracker.retry_intent_for(key_of(1, 100));
        QIVEN_VERIFY(retry.has_value());
        QIVEN_VERIFY(retry->kind == ActionKind::RetryFailure);
        QIVEN_VERIFY(retry->priorFailure.has_value());
        QIVEN_VERIFY(retry->priorFailure->stableMessage == "LNK1103 corrupt pdb");
    }

    // a DIFFERENT triple (actor or target) carries no failure memory
    {
        FailureTracker tracker;
        tracker.record(key_of(1, 100), fingerprint_with("msvc", "x"));
        QIVEN_VERIFY(!tracker.retry_intent_for(key_of(2, 100)).has_value());
        QIVEN_VERIFY(!tracker.retry_intent_for(key_of(1, 200)).has_value());
    }

    // recurrence counting: repeated failures of the same triple are one
    // entry, the most recent fingerprint wins
    {
        FailureTracker tracker;
        tracker.record(key_of(1, 100), fingerprint_with("msvc", "first"));
        tracker.record(key_of(1, 100), fingerprint_with("cmake", "second"));
        QIVEN_VERIFY(tracker.distinct_failures() == 1);
        const auto retry = tracker.retry_intent_for(key_of(1, 100));
        QIVEN_VERIFY(retry->priorFailure->stableMessage == "second");
        QIVEN_VERIFY(retry->priorFailure->tool == "cmake");
    }

    // the classification hook: an action with a known failure classifies
    // with the mechanical floor PLUS the RetryFailure intent (C-17: the
    // next equivalent control state changes automatically); without
    // memory the floor is unchanged
    {
        const std::byte blob[] { std::byte { 0x00 } };
        const auto action = qiven::runtime::observe_action(
            qiven::runtime::AdapterInstanceId { 1 }, qiven::runtime::HarnessSessionId { 1 },
            qiven::runtime::ActorInstanceId { 42 }, qiven::runtime::CapabilityId { 5 }, "build", "src/main.cpp",
            std::span<const std::byte>(blob, 1));
        const StructuralFacts facts {};

        FailureTracker empty;
        const auto clean = qiven::runtime::classify_with_failure_memory(action, facts, empty);
        QIVEN_VERIFY(!clean.contains_kind(ActionKind::RetryFailure));
        QIVEN_VERIFY(clean.contains_kind(ActionKind::BeginTask)); // floor intact

        FailureTracker loaded;
        loaded.record(FailureKey { 42, 5, qiven::fnv1a64("src/main.cpp") },
                      fingerprint_with("msvc", "C2065 undeclared"));
        const auto retrying = qiven::runtime::classify_with_failure_memory(action, facts, loaded);
        QIVEN_VERIFY(retrying.contains_kind(ActionKind::RetryFailure));
        QIVEN_VERIFY(retrying.contains_kind(ActionKind::BeginTask)); // floor NOT narrowed (ADL 21)

        // the retry intent carries the prior failure for pit-recall derivation
        bool carries = false;
        for (const auto& member : retrying.members())
        {
            if (member.intent.kind == ActionKind::RetryFailure && member.intent.priorFailure.has_value() &&
                member.intent.priorFailure->stableMessage == "C2065 undeclared")
            {
                carries = true;
            }
        }
        QIVEN_VERIFY(carries);
    }

    std::printf("[ OK ] failure-retry\n");
    return 0;
}
